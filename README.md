# C++ Async Logger V1.8

`tinylog` 提供同步的 `Logger` 和带单个后台写线程的 `AsyncLogger`。两者都以追加模式打开文件，按 `[INFO] message`（随日志级别变化）的格式写入日志。

V1.8 在保持接口语义与故障安全保障不变的基础上，将异步队列由逐条消费的 `std::queue<Task>` 升级为预分配 `std::vector<Task>` 批量 `swap` 模型，并将日志构造移至锁外、按需触发条件变量通知。

## 构建、测试与基准运行

需要 CMake 3.20 或更新版本，以及支持 C++20 的编译器和标准库。故障回归测试使用 Linux 的 `/dev/full` 模拟文件写入和刷新失败。

### 构建并运行测试（7 项 CTest）

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

TSan 并发检测构建：

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

### 可执行目标

- **库与示例**：静态库 `async_logger` 同时提供 `Logger` 和 `AsyncLogger`；`logger_demo` 与 `async_logger_demo` 分别演示同步与异步写入。
- **单元与故障测试**：`logger_test`、`async_logger_test`、`async_flush_failure_test`（共注册为 7 个 CTest 用例，覆盖基础读写、并发多生产者、`shutdown()`/`flush()` 故障传播、同批故障等待唤醒及满队列背压）。
- **性能基准（Release `-O3 -DNDEBUG`）**：`tinylog_single_producer_bench`、`tinylog_multi_producer_bench`、`tinylog_backpressure_bench`。

## V1.8 优化与性能概览

相对 V1.7 的单锁逐条入队/出队模型，V1.8 主要改进包括：

- **锁外构造 `LogEntry`**：在获取 `mutex_` 前完成 `std::string` 拷贝与堆分配，缩短临界区持锁时间。
- **空队列按需唤醒**：`log()` 仅在入队前队列为空时，先 `unlock()` 再调用 `cv_.notify_one()`。
- **批量 `swap` 消费**：`queue_` 改用预留 1024 容量的 `std::vector<Task>`，worker 持锁执行 `std::swap(batch, queue_)` 后解锁顺序写入整批任务。
- **按需释放背压等待者**：仅当出队前队列已满（`was_full`）时在解锁后调用 `cv_not_full_.notify_all()`。
- **同批异常安全清理**：写入或刷新失败进入异常处理时，统一通过 `complete_failed_flush()` 交付 `batch[next_index..]` 与 `queue_` 中尚未完成的 `FlushRequest`。

基于 Release（`-O3 -DNDEBUG`，64 字节 payload，写入 OS page cache）实测，对比 [V1.7 性能基线](docs/benchmarks_v1.7.md)，关键变化如下（详见 [V1.8 性能与验证归档](docs/benchmarks_v1.8.md)）：

| 基准场景 | V1.7 Async | V1.8 Async | V1.8 / V1.7 | V1.8 相对 Sync 对照组 |
| --- | ---: | ---: | ---: | ---: |
| 单生产者（100,000 条，5 次中位数） | 2,449,865 logs/sec | 11,324,538 logs/sec | **4.62x** | 0.91x（vs 无锁单线程 Sync） |
| 多生产者 P=2（100,000 条） | 1,855,653 logs/sec | 5,820,812 logs/sec | **3.14x** | **1.13x**（反超外部加锁 Sync） |
| 多生产者 P=4（100,000 条） | 1,039,200 logs/sec | 4,692,503 logs/sec | **4.52x** | **1.01x**（反超外部加锁 Sync） |
| 多生产者 P=8（100,000 条） | 951,090 logs/sec | 3,714,603 logs/sec | **3.91x** | **1.15x**（反超外部加锁 Sync） |
| 背压 P=8 / 100,000 条（`shutdown`） | 932,592 logs/sec | 3,857,903 logs/sec | **4.14x** | **1.24x**（512–100k 全档反超 Sync） |

验证结果：Release CTest **7/7 通过**，TSan CTest **7/7 通过**，异步/同批故障/背压 TSan 25 次重复压测（6 项 × 25 = 150 次）**全部通过**，三项基准全部轮次行数校验及 40 份末轮日志逐字节复核通过。

## 接口语义

| 接口 | 调用行为 |
| --- | --- |
| `AsyncLogger(path)` | 追加打开日志文件，预分配 1024 槽位队列，随后启动后台线程。文件打开失败时抛出 `std::runtime_error`。 |
| `log(level, message)` | 在锁外复制 `std::string_view` 指向的内容构造 `LogEntry`，随后加锁入队，返回表示提交成功。调用方只需在调用期间保持消息有效。后台线程随后批量写入文件流。 |
| `flush()` | 在同一队列中插入刷新请求并等待结果。成功返回时，排在该请求之前的日志已写入，文件流缓冲区也已刷新。排在请求之后的日志由后续任务处理。 |
| `shutdown()` | 停止接收任务，唤醒等待队列空位的调用者，等待后台线程处理已有任务并执行最后一次刷新。文件随对象析构关闭。可以顺序重复调用，已发生的后台故障仍会抛出。 |
| `~AsyncLogger()` | 自动执行 `shutdown()` 并吞掉异常。需要观察退出时的 I/O 错误时，应显式调用 `shutdown()`。 |

后台写入或刷新失败后，实例会停止并丢弃未处理的日志。同批剩余及队列中尚未完成的刷新请求均会收到异常，后续 `log()` 和 `flush()` 会抛出 `std::runtime_error`。`flush()` 和 `shutdown()` 的刷新范围是 C++ 文件流缓冲区，接口不提供断电后的持久化保证。

同步的 `Logger::log()` 在调用线程写入文件流，`Logger::flush()` 刷新该流。多个线程共享同一个 `Logger` 时，调用方须串行化访问。

## 并发使用限制

- 多个线程可以在同一个 `AsyncLogger` 实例上并发调用 `log()` 和 `flush()`。后台线程按入队顺序批量处理任务，各线程之间的顺序取决于实际入队时机。
- 队列最多容纳 1024 个待处理任务，日志和刷新请求共用容量。队列满时，`log()` 和 `flush()` 会阻塞到有空位或进入停止状态。`flush()` 入队后还会等待后台线程完成刷新。
- 在对象存活期间，一个线程可以调用 `shutdown()`，同时其他线程仍在调用 `log()` 或 `flush()`。关闭前已入队的任务按队列顺序处理，后台故障时按上述规则处理。关闭开始时还未入队的调用会收到 `std::runtime_error`。
- 调用方须串行调用 `shutdown()`，并确保所有成员调用都已结束后再销毁对象。`shutdown()` 只等待 logger 自身的后台线程退出，调用方仍需等待各生产者结束。
- 同一路径若由多个实例或进程同时写入，写入顺序和日志记录的完整性需要调用方协调。

## 文档索引

- [V1.6 异步 `flush()` 故障复盘](docs/v1.6-flush-failure.md)
- [V1.7 Logger 性能基线](docs/benchmarks_v1.7.md)
- [V1.8 Logger 性能与验证归档](docs/benchmarks_v1.8.md)
