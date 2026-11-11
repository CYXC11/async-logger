# C++ Async Logger V1.6

`tinylog` 提供同步的 `Logger` 和带单个后台写线程的 `AsyncLogger`。两者都以追加模式打开文件，按 `[INFO] message` 的格式写入日志。

## 构建与验证

需要 CMake 3.20 或更新版本，以及支持 C++20 的编译器和标准库。故障测试使用 Linux 的 `/dev/full` 模拟文件写入失败。

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake 目标 `async_logger` 同时提供两种 logger。`logger_demo` 和 `async_logger_demo` 分别演示同步和异步写入。

## 接口语义

| 接口 | 调用行为 |
| --- | --- |
| `AsyncLogger(path)` | 追加打开日志文件，随后启动后台线程。文件打开失败时抛出 `std::runtime_error`。 |
| `log(level, message)` | 复制 `std::string_view` 指向的内容并入队，返回表示提交成功。调用方只需在调用期间保持消息有效。后台线程随后写入文件流。 |
| `flush()` | 在同一队列中插入刷新请求并等待结果。成功返回时，排在该请求之前的日志已写入，文件流缓冲区也已刷新。排在请求之后的日志由后续任务处理。 |
| `shutdown()` | 停止接收任务，唤醒等待队列空位的调用者，等待后台线程处理已有任务并执行最后一次刷新。文件随对象析构关闭。可以顺序重复调用，已发生的后台故障仍会抛出。 |
| `~AsyncLogger()` | 自动执行 `shutdown()` 并吞掉异常。需要观察退出时的 I/O 错误时，应显式调用 `shutdown()`。 |

后台写入或刷新失败后，实例会停止并丢弃未处理的日志。尚未完成的已入队刷新请求会收到异常，后续 `log()` 和 `flush()` 会抛出 `std::runtime_error`。`flush()` 和 `shutdown()` 的刷新范围是 C++ 文件流缓冲区，接口不提供断电后的持久化保证。

同步的 `Logger::log()` 在调用线程写入文件流，`Logger::flush()` 刷新该流。多个线程共享同一个 `Logger` 时，调用方须串行化访问。

## 并发使用限制

- 多个线程可以在同一个 `AsyncLogger` 实例上并发调用 `log()` 和 `flush()`。后台线程按入队顺序处理任务，各线程之间的顺序取决于实际入队时机。
- 队列最多容纳 1024 个待处理任务，日志和刷新请求共用容量。队列满时，`log()` 和 `flush()` 会阻塞到有空位或进入停止状态。`flush()` 入队后还会等待后台线程完成刷新。
- 在对象存活期间，一个线程可以调用 `shutdown()`，同时其他线程仍在调用 `log()` 或 `flush()`。关闭前已入队的任务按队列顺序处理，后台故障时按上述规则处理。关闭开始时还未入队的调用会收到 `std::runtime_error`。
- 调用方须串行调用 `shutdown()`，并确保所有成员调用都已结束后再销毁对象。`shutdown()` 只等待 logger 自身的后台线程退出，调用方仍需等待各生产者结束。
- 同一路径若由多个实例或进程同时写入，写入顺序和日志记录的完整性需要调用方协调。

故障复盘见 [V1.6 异步 `flush()` 故障复盘](docs/v1.6-flush-failure.md)。
