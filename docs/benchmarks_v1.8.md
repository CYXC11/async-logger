# V1.8 Logger 性能与验证归档

## V1.8 优化改动摘要

对比 V1.7 的单锁逐条入队/出队模型，V1.8 对 [`include/tinylog/async_logger.h`](../include/tinylog/async_logger.h) 与 [`src/async_logger.cpp`](../src/async_logger.cpp) 做了以下核心改动（队列容量仍为 `capacity_ = 1024`）：

1. **锁外构造 `LogEntry`**：`AsyncLogger::log()` 在获取 `mutex_` 之前完成 `LogEntry{.level = level, .message = std::string(message)}` 构造，将字符串拷贝及超过 SSO 容量（当前 libstdc++ 为 15 字节）时的堆分配移出临界区，缩短持锁时间。
2. **空队列按需 `notify`**：`log()` 入队前记录 `should_notify = queue_.empty()`，入队后显式执行 `lock.unlock()`，仅在 `should_notify` 为真（队列由空变非空）时调用 `cv_.notify_one()`，避免队列非空时生产者逐条触发条件变量唤醒并减少锁内唤醒带来的无效抢占。
3. **`std::vector<Task>` 批量 `swap`**：将底层队列 `queue_` 从 `std::queue<Task>` 替换为 `std::vector<Task>`，并在构造函数中调用 `queue_.reserve(capacity_)`；`worker_loop()` 预先构造并预留 `capacity_` 容量的局部 `batch` 向量，等待唤醒后持锁执行 `std::swap(batch, queue_)` 并立即解锁，随后在无锁状态下顺序消费整批任务，批末重置 `next_index = 0` 并 `batch.clear()`（保留容量）。
4. **`was_full` 唤醒 `cv_not_full_.notify_all()`**：worker 在 `std::swap(batch, queue_)` 前持锁记录 `was_full = (queue_.size() >= capacity_)`，解锁后仅当 `was_full` 为真时调用 `cv_not_full_.notify_all()`，替代 V1.7 每出队一条就调用一次 `cv_not_full_.notify_one()` 的做法，在批量腾空满队列时一次放行等待中的生产者，同时消除未满队列上的冗余唤醒。
5. **异常路径清理 `batch[next_index..]` 与剩余 `queue_`** ：`worker_loop()` 消费任务时通过 `batch[next_index++]` 推进游标；当文件写入或刷新抛出异常进入外层 `catch (...)` 时，持锁设置 `worker_failed_ = true` 与 `stopping_ = true`，并遍历尚未处理的 `batch[next_index..]` 与 `queue_` 中所有 `FlushRequest`，通过 `complete_failed_flush()` 向等待方交付错误信息后清空两个容器，最后调用 `cv_not_full_.notify_all()` 唤醒阻塞生产者。

## 测试环境快照、二进制 SHA-256 与复现命令

### 测试环境快照

| 项目 | 采集值 |
| --- | --- |
| 正式计时窗口 | 2026-11-16 22:49:02–22:49:05（Asia/Shanghai，UTC+08:00） |
| Git HEAD 与工作区状态 | `6197a20`（`git rev-parse --short HEAD`）。工作区基于当前未提交的 V1.8 优化源码、基准与扩展测试重新构建（修改：`CMakeLists.txt`、`benchmarks/single_producer.cpp`、`docs/v1.6-flush-failure.md`、`include/tinylog/async_logger.h`、`src/async_logger.cpp`、`tests/async_flush_failure_test.cpp`、`tests/async_logger_test.cpp`；新增：`benchmarks/backpressure_analysis.cpp`、`benchmarks/benchmark_common.h`、`benchmarks/concurrent_benchmark.h`、`benchmarks/multi_producer.cpp`、`docs/benchmarks_v1.7.md`）。 |
| CPU | Intel(R) Core(TM) i7-14650HX，x86_64，16 核、24 个逻辑 CPU（`lscpu`） |
| 内存 | 31.05 GiB（`/proc/meminfo` 的 `MemTotal` 为 `32,557,176 kB`） |
| 电源 | 已插电。测前测后均为 `ACAD/online=1`、`BAT1/status=Full`。 |
| CPU 调频 | `cpu0` 的 `scaling_governor=powersave`、`energy_performance_preference=performance`（测前测后一致），驱动为 `intel_pstate`。 |
| OS 与内核 | Ubuntu 26.04.1 LTS，Linux `7.0.0-43-generic`（`uname -r`） |
| 文件系统 | 仓库与构建目录均位于 `/dev/nvme1n1p3` 上的 `ext4`（`df -T . build-v17-release`）。 |
| 工具链 | `g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`，libstdc++，`cmake version 4.2.3`，`GNU Make 4.4.1` |
| Release 构建 | `build-v17-release` 目录通过 `--clean-first` 重新构建，三个基准目标的 `CXX_FLAGS` 均为 `-O3 -DNDEBUG -std=c++20`。 |
| TSan 构建 | `build-tsan` 目录通过 `--clean-first` 重新构建，`CMAKE_BUILD_TYPE=RelWithDebInfo`，`CXX_FLAGS` 为 `-fsanitize=thread -fno-omit-frame-pointer -O2 -g -DNDEBUG -std=c++20`。 |
| 后台负载 | 正式采集串行执行，未挂载 `perf` 或并行任务。用 `vmstat 1 4` 检查系统负载（跳过首行开机以来均值），测前 3 个 1 秒采样 CPU 空闲率 `id` 为 98%、97%、98%，测后为 98%、95%、97%。 |

### Release 基准二进制 SHA-256

以下校验和均取自基于当前最新源码 `--clean-first` 构建后的 `build-v17-release` 成品：

```text
ceee4e7379224fb5a2cd800c5e9f0745860272699a8de58d4511a129963af112  tinylog_single_producer_bench
a376653e97ba4f792696aec600a276951bf352fd9b3560fa2de9702dd05aec11  tinylog_multi_producer_bench
2df850adc9c5fae8e61bfc456a338f82d7a3baf70686eb23272b48655ecabd9b  tinylog_backpressure_bench
```

### 完整复现命令

在仓库根目录依次执行构建、正式采集与验证（正式采集与 V1.7 保持完全相同的执行顺序，且不挂载 `perf`）：

```bash
# 1. 基于当前源码干净构建 Release 与 TSan 目录
cmake -S . -B build-v17-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-v17-release --clean-first --parallel

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'
cmake --build build-tsan --clean-first --parallel

# 2. 空闲环境下执行正式采集与 Release CTest
(
    set -e
    export LC_ALL=C TZ=Asia/Shanghai
    cd build-v17-release
    vmstat 1 4
    date -Is
    cat /sys/class/power_supply/ACAD/online /sys/class/power_supply/BAT1/status \
        /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
        /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference
    for run in 1 2 3 4 5; do
        printf '\nSingle producer, invocation %s\n' "$run"
        ./tinylog_single_producer_bench
    done
    ./tinylog_multi_producer_bench
    ./tinylog_backpressure_bench
    date -Is
    cat /sys/class/power_supply/ACAD/online /sys/class/power_supply/BAT1/status \
        /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
        /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference
    vmstat 1 4
) && ctest --test-dir build-v17-release -C Release --output-on-failure

# 3. 单独执行 TSan CTest 与并发重复压测
ctest --test-dir build-tsan --output-on-failure
ctest --test-dir build-tsan -R 'async|releases_same_batch' --repeat until-fail:25 --output-on-failure

# 4. 单独执行 perf 软件计数与热点采样（本机 perf_event_paranoid=4，需 sudo）
sudo perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
  ./build-v17-release/tinylog_multi_producer_bench
sudo perf record -F 999 -g -- ./build-v17-release/tinylog_multi_producer_bench
sudo perf report -f --stdio --no-children -n --percent-limit 1
```

## 测量边界

与 [`docs/benchmarks_v1.7.md`](benchmarks_v1.7.md) 保持完全一致的统计与测量边界：

- 计时使用 `std::chrono::steady_clock`。旧文件清理、payload 构造、Logger 构造和析构、行数校验均在计时窗口之外。
- `T_submit` 截止到全部 `log()` 调用结束。单生产者基准直接在主线程执行循环；多生产者与背压基准通过 `std::latch` 在计时起点统一放行生产者线程，提交耗时包含线程唤醒与完成同步，线程创建与 `join()` 在计时之外。
- `T_total` 从同一起点计至完成 API 返回。同步组调用 `flush()`（多生产者与背压基准中通过基准侧外部 `std::mutex` 序列化 `Logger` 调用）；异步 `shutdown()` 等待 worker 排空队列、刷新并退出；异步 `flush()` 等待入队的刷新请求完成，worker 在计时之外随 Logger 析构退出。
- 完成口径仍为日志通过 `std::ofstream` 写入并由 `flush()` 刷入 OS page cache 为止，`fsync()` 与物理落盘不在测量范围之内。吞吐量按日志总量除以 `T_total` 计算。
- 三个基准共用 64 字节 payload，于计时前构造。当前 libstdc++ 的 `std::string` 对 `char` 的 SSO 容量为 15 字节，因此 `AsyncLogger::log()` 构造 `LogEntry` 中的 `std::string(message)` 时仍会为每条 64 字节消息触发堆分配（V1.8 仅将该堆分配移至 `mutex_` 锁外，并未消除堆分配本身）。

## V1.8 单生产者（V1.8-01）

单生产者基准连续执行 5 个独立进程，每个进程内无预热，依次运行 Sync（`Logger::flush()`）与 Async（`AsyncLogger::shutdown()`）各 1 轮，每轮提交 100,000 条日志。下表给出 5 次实测原始值及逐列中位数：

| 次数 | Sync T_submit (ms) | Sync T_total (ms) | Sync (logs/sec) | Async T_submit (ms) | Async T_total (ms) | Async (logs/sec) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 6.642 | 6.648 | 15,042,387 | 12.295 | 12.447 | 8,033,746 |
| 2 | 6.519 | 6.524 | 15,328,255 | 8.205 | 8.357 | 11,966,553 |
| 3 | 9.304 | 9.309 | 10,742,344 | 8.679 | 8.830 | 11,324,538 |
| 4 | 8.022 | 8.027 | 12,458,169 | 10.490 | 10.598 | 9,436,159 |
| 5 | 8.969 | 8.973 | 11,143,941 | 8.693 | 8.810 | 11,350,901 |
| **中位数** | **8.022** | **8.027** | **12,458,169** | **8.693** | **8.830** | **11,324,538** |

## V1.8 多生产者（V1.8-02）

对 1、2、4、8 个生产者各跑 5 轮并取逐列中位数。每轮固定先同步后异步，总日志量固定为 100,000 条并按生产者均分。Sync 组使用基准侧外部 `std::mutex` 包装 `Logger`，Async 组以 `shutdown()` 收尾：

| 生产者 | Logger | T_submit (ms) | T_total (ms) | 完成吞吐量 (logs/sec) |
| ---: | --- | ---: | ---: | ---: |
| 1 | Sync (external mutex) | 7.995 | 8.022 | 12,465,267 |
| 1 | Async (shutdown) | 12.761 | 12.855 | 7,778,898 |
| 2 | Sync (external mutex) | 19.458 | 19.467 | 5,136,994 |
| 2 | Async (shutdown) | 17.081 | 17.180 | 5,820,812 |
| 4 | Sync (external mutex) | 21.417 | 21.422 | 4,668,205 |
| 4 | Async (shutdown) | 21.207 | 21.311 | 4,692,503 |
| 8 | Sync (external mutex) | 30.962 | 30.975 | 3,228,397 |
| 8 | Async (shutdown) | 26.743 | 26.921 | 3,714,603 |

## V1.8 背压实验（V1.8-03）

每个生产者数与日志量组合先运行 1 轮不计时预热，再执行 9 轮正式测量并轮换三种模式的执行顺序（每种模式在各顺位各出现 3 次）。表格报告逐列中位数，`T_tail = T_total - T_submit` 与 `T_submit / T_total` 均先按每轮原始耗时计算再分别取中位数，完成吞吐量按日志总量除以 `T_total` 中位数计算。

### 1 个生产者

| 日志量 | Logger | T_submit (ms) | T_total (ms) | T_tail (ms) | T_submit / T_total | 完成吞吐量 (logs/sec) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 512 | Sync (external mutex, flush) | 0.069 | 0.072 | 0.004 | 96.62% | 7,087,879 |
| 512 | Async (shutdown) | 0.043 | 0.113 | 0.069 | 35.25% | 4,533,862 |
| 512 | Async (flush) | 0.041 | 0.106 | 0.060 | 38.26% | 4,832,377 |
| 1,024 | Sync (external mutex, flush) | 0.110 | 0.113 | 0.003 | 97.40% | 9,049,854 |
| 1,024 | Async (shutdown) | 0.100 | 0.191 | 0.096 | 51.01% | 5,371,240 |
| 1,024 | Async (flush) | 0.118 | 0.198 | 0.086 | 59.03% | 5,160,250 |
| 2,048 | Sync (external mutex, flush) | 0.213 | 0.222 | 0.006 | 97.32% | 9,243,462 |
| 2,048 | Async (shutdown) | 0.224 | 0.365 | 0.141 | 60.95% | 5,614,835 |
| 2,048 | Async (flush) | 0.198 | 0.331 | 0.124 | 65.37% | 6,192,325 |
| 8,192 | Sync (external mutex, flush) | 0.787 | 0.819 | 0.025 | 97.27% | 10,007,733 |
| 8,192 | Async (shutdown) | 1.071 | 1.220 | 0.127 | 88.32% | 6,714,919 |
| 8,192 | Async (flush) | 1.130 | 1.252 | 0.088 | 91.99% | 6,543,722 |
| 100,000 | Sync (external mutex, flush) | 7.284 | 7.292 | 0.020 | 99.78% | 13,714,249 |
| 100,000 | Async (shutdown) | 12.084 | 12.194 | 0.110 | 99.10% | 8,200,551 |
| 100,000 | Async (flush) | 12.444 | 12.460 | 0.087 | 99.27% | 8,025,765 |

### 8 个生产者

| 日志量 | Logger | T_submit (ms) | T_total (ms) | T_tail (ms) | T_submit / T_total | 完成吞吐量 (logs/sec) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 512 | Sync (external mutex, flush) | 0.171 | 0.174 | 0.002 | 98.71% | 2,938,779 |
| 512 | Async (shutdown) | 0.116 | 0.149 | 0.034 | 78.75% | 3,443,568 |
| 512 | Async (flush) | 0.125 | 0.142 | 0.022 | 86.49% | 3,598,538 |
| 1,024 | Sync (external mutex, flush) | 0.359 | 0.367 | 0.008 | 97.76% | 2,786,395 |
| 1,024 | Async (shutdown) | 0.243 | 0.273 | 0.031 | 88.62% | 3,753,597 |
| 1,024 | Async (flush) | 0.260 | 0.283 | 0.018 | 92.99% | 3,621,907 |
| 2,048 | Sync (external mutex, flush) | 0.687 | 0.697 | 0.005 | 99.19% | 2,940,079 |
| 2,048 | Async (shutdown) | 0.493 | 0.542 | 0.049 | 90.95% | 3,778,277 |
| 2,048 | Async (flush) | 0.489 | 0.538 | 0.050 | 90.39% | 3,807,456 |
| 8,192 | Sync (external mutex, flush) | 2.666 | 2.702 | 0.014 | 99.49% | 3,031,622 |
| 8,192 | Async (shutdown) | 2.094 | 2.118 | 0.021 | 99.05% | 3,868,214 |
| 8,192 | Async (flush) | 2.033 | 2.114 | 0.068 | 96.72% | 3,874,889 |
| 100,000 | Sync (external mutex, flush) | 32.258 | 32.268 | 0.010 | 99.97% | 3,099,006 |
| 100,000 | Async (shutdown) | 25.796 | 25.921 | 0.124 | 99.52% | 3,857,903 |
| 100,000 | Async (flush) | 26.232 | 26.362 | 0.082 | 99.68% | 3,793,317 |

## V1.8 与 V1.7 关键对比

下表汇总 V1.8 与 [`docs/benchmarks_v1.7.md`](benchmarks_v1.7.md) 在相同测试矩阵下的完成吞吐量对比、V1.8/V1.7 提速倍数以及各版本中 Async 相对同测点 Sync 对照组的性能比值（`Async / Sync`）。

### 单生产者与多生产者关键对比（100,000 条日志）

| 基准与配置 | Logger | V1.7 吞吐量 (logs/sec) | V1.8 吞吐量 (logs/sec) | V1.8 / V1.7 | V1.7 vs Sync | V1.8 vs Sync |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 单生产者（5 次中位数） | Sync (flush) | 10,806,910 | 12,458,169 | 1.15x | 1.00x | 1.00x |
| 单生产者（5 次中位数） | Async (shutdown) | 2,449,865 | 11,324,538 | **4.62x** | 0.23x | **0.91x** |
| 多生产者 P=1 | Sync (external mutex) | 10,381,998 | 12,465,267 | 1.20x | 1.00x | 1.00x |
| 多生产者 P=1 | Async (shutdown) | 2,349,752 | 7,778,898 | **3.31x** | 0.23x | **0.62x** |
| 多生产者 P=2 | Sync (external mutex) | 5,934,523 | 5,136,994 | 0.87x | 1.00x | 1.00x |
| 多生产者 P=2 | Async (shutdown) | 1,855,653 | 5,820,812 | **3.14x** | 0.31x | **1.13x** |
| 多生产者 P=4 | Sync (external mutex) | 4,760,196 | 4,668,205 | 0.98x | 1.00x | 1.00x |
| 多生产者 P=4 | Async (shutdown) | 1,039,200 | 4,692,503 | **4.52x** | 0.22x | **1.01x** |
| 多生产者 P=8 | Sync (external mutex) | 3,197,404 | 3,228,397 | 1.01x | 1.00x | 1.00x |
| 多生产者 P=8 | Async (shutdown) | 951,090 | 3,714,603 | **3.91x** | 0.30x | **1.15x** |

### 背压关键对比（1 个生产者与 8 个生产者）

| 生产者 | 日志量 | Logger | V1.7 吞吐量 (logs/sec) | V1.8 吞吐量 (logs/sec) | V1.8 / V1.7 | V1.7 vs Sync | V1.8 vs Sync |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 512 | Sync (external mutex, flush) | 7,479,038 | 7,087,879 | 0.95x | 1.00x | 1.00x |
| 1 | 512 | Async (shutdown) | 3,506,201 | 4,533,862 | **1.29x** | 0.47x | 0.64x |
| 1 | 512 | Async (flush) | 3,681,308 | 4,832,377 | **1.31x** | 0.49x | 0.68x |
| 1 | 1,024 | Sync (external mutex, flush) | 8,802,771 | 9,049,854 | 1.03x | 1.00x | 1.00x |
| 1 | 1,024 | Async (shutdown) | 3,896,974 | 5,371,240 | **1.38x** | 0.44x | 0.59x |
| 1 | 1,024 | Async (flush) | 4,034,387 | 5,160,250 | **1.28x** | 0.46x | 0.57x |
| 1 | 2,048 | Sync (external mutex, flush) | 8,882,682 | 9,243,462 | 1.04x | 1.00x | 1.00x |
| 1 | 2,048 | Async (shutdown) | 2,441,249 | 5,614,835 | **2.30x** | 0.27x | 0.61x |
| 1 | 2,048 | Async (flush) | 2,439,539 | 6,192,325 | **2.54x** | 0.27x | 0.67x |
| 1 | 8,192 | Sync (external mutex, flush) | 9,307,748 | 10,007,733 | 1.08x | 1.00x | 1.00x |
| 1 | 8,192 | Async (shutdown) | 1,938,515 | 6,714,919 | **3.46x** | 0.21x | 0.67x |
| 1 | 8,192 | Async (flush) | 2,047,152 | 6,543,722 | **3.20x** | 0.22x | 0.65x |
| 1 | 100,000 | Sync (external mutex, flush) | 12,352,700 | 13,714,249 | 1.11x | 1.00x | 1.00x |
| 1 | 100,000 | Async (shutdown) | 2,422,526 | 8,200,551 | **3.39x** | 0.20x | 0.60x |
| 1 | 100,000 | Async (flush) | 2,275,571 | 8,025,765 | **3.53x** | 0.18x | 0.59x |
| 8 | 512 | Sync (external mutex, flush) | 3,120,391 | 2,938,779 | 0.94x | 1.00x | 1.00x |
| 8 | 512 | Async (shutdown) | 2,262,084 | 3,443,568 | **1.52x** | 0.72x | **1.17x** |
| 8 | 512 | Async (flush) | 2,270,017 | 3,598,538 | **1.59x** | 0.73x | **1.22x** |
| 8 | 1,024 | Sync (external mutex, flush) | 2,816,158 | 2,786,395 | 0.99x | 1.00x | 1.00x |
| 8 | 1,024 | Async (shutdown) | 2,343,416 | 3,753,597 | **1.60x** | 0.83x | **1.35x** |
| 8 | 1,024 | Async (flush) | 2,397,307 | 3,621,907 | **1.51x** | 0.85x | **1.30x** |
| 8 | 2,048 | Sync (external mutex, flush) | 2,790,890 | 2,940,079 | 1.05x | 1.00x | 1.00x |
| 8 | 2,048 | Async (shutdown) | 1,427,711 | 3,778,277 | **2.65x** | 0.51x | **1.29x** |
| 8 | 2,048 | Async (flush) | 1,402,861 | 3,807,456 | **2.71x** | 0.50x | **1.30x** |
| 8 | 8,192 | Sync (external mutex, flush) | 2,881,701 | 3,031,622 | 1.05x | 1.00x | 1.00x |
| 8 | 8,192 | Async (shutdown) | 880,357 | 3,868,214 | **4.39x** | 0.31x | **1.28x** |
| 8 | 8,192 | Async (flush) | 873,435 | 3,874,889 | **4.44x** | 0.30x | **1.28x** |
| 8 | 100,000 | Sync (external mutex, flush) | 3,221,096 | 3,099,006 | 0.96x | 1.00x | 1.00x |
| 8 | 100,000 | Async (shutdown) | 932,592 | 3,857,903 | **4.14x** | 0.29x | **1.24x** |
| 8 | 100,000 | Async (flush) | 951,401 | 3,793,317 | **3.99x** | 0.30x | **1.22x** |

## perf 系统开销与热点观察

正式计时结束后，另行通过 `sudo` 针对 `./build-v17-release/tinylog_multi_producer_bench` 单独运行软件计数 `perf stat` 与采样 `perf report`（本机 `/proc/sys/kernel/perf_event_paranoid = 4`，非特权会话无 `CAP_PERFMON` / `CAP_SYS_PTRACE` / `CAP_SYS_ADMIN` 时直接运行 `perf` 会被内核拒绝）。

> **归因边界说明**：`tinylog_multi_producer_bench` 单次执行包含 `1/2/4/8` 生产者 × 5 轮的 **Sync (external mutex)**、**Async (shutdown)** 以及计时外的 **`verify_line_count`**（共 40 轮写入与 40 次全量读回校验）。下列系统计数与函数热点均为该混合二进制整进程结果，**不能**将全部开销直接等同于 Async 单一路径。

### 1. `perf stat` 软件计数结果

运行命令：`sudo perf stat -e task-clock,context-switches,cpu-migrations,page-faults ./build-v17-release/tinylog_multi_producer_bench`

| 计数事件 / 指标 | 实测值 | 说明 |
| --- | ---: | --- |
| `task-clock` | `2,728.22 msec` | 进程总 CPU 任务时钟 |
| `context-switches` | `351,916` | 整进程上下文切换次数 |
| `cpu-migrations` | `1,292` | 线程跨核迁移次数 |
| `page-faults` | `602` | 整进程缺页次数 |
| `time elapsed` | `0.891239645 s` | 挂钟总耗时 |
| `user` / `sys` | `1.061983000 s` / `1.780666000 s` | 内核态 CPU 时间占 `user + sys` 约 62.6% |

本次独立 `perf stat` 进程自身输出的吞吐量中位数为：Sync (external mutex) 在 1/2/4/8 生产者下为 `7,503,708` / `5,481,384` / `4,877,525` / `3,498,034 logs/sec`，Async (shutdown) 为 `8,260,592` / `5,680,844` / `5,089,958` / `3,901,537 logs/sec`（与上方无 `perf` 正式表相互独立）。此外，另行在非特权下运行 `/usr/bin/time -v ./tinylog_multi_producer_bench` 测得整进程 `0.92 s` 挂钟、`1.28 s` user、`1.87 s` sys、`422,096` 次自愿切换与 `148` 次非自愿切换、`624` 次 minor page faults、`0` 次 major page faults，数量级与 `perf stat` 一致。

### 2. `perf report` 热点采样摘要

运行命令：`sudo perf report -f --stdio --no-children -n --percent-limit 1`（`Total Lost Samples: 0`；以下严格保留实测输出片段，其中部经分页器 `...skipping...` 略过若干行，行末 `>` 为终端列宽截断符号）：

**片段 A（事件 `cpu_atom/cycles/P`，`Samples: 5K`，`Event count (approx.): 2,396,328,368`）：**

| Overhead | Samples | Shared Object | Symbol |
| ---: | ---: | --- | --- |
| 15.80% | 816 | `[kernel.kallsyms]` | `[k] native_queued_spin_lock_slowpath` |
| 6.94% | 360 | `[kernel.kallsyms]` | `[k] entry_SYSRETQ_unsafe_stack` |
| 4.32% | 219 | `libc.so.6` | `[.] pthread_mutex_lock@@GLIBC_2.2.5` |
| 4.02% | 195 | `libc.so.6` | `[.] __GI___lll_lock_wait` |
| 3.17% | 167 | `libc.so.6` | `[.] pthread_mutex_unlock@@GLIBC_2.2.5` |
| 2.43% | 114 | `libc.so.6` | `[.] __libc_malloc2` |
| 2.42% | 129 | `[kernel.kallsyms]` | `[k] futex_wait_setup` |
| 2.15% | 109 | `[kernel.kallsyms]` | `[k] futex_wake` |
| 1.91% | 83 | `[kernel.kallsyms]` | `[k] _raw_spin_lock` |
| 1.62% | 72 | `libstdc++.so.6.0.35` | `[.] std::basic_ostream<char, std::char_traits<char> >& >` |

**片段 B（经 `...skipping...` 跳转后的后续输出段）：**

> 注：`entry_SYSRETQ_unsafe_stack` 在片段 B 中再次出现（`1.21%` / `95` samples，样本总分母与片段 A 不同），符合混合架构 CPU 上 `perf report` 进入另一 PMU 事件分区的表现；因分页略过了该分段表头，下表不臆测省略的头部名称，仅按实测列值记录：

| Overhead | Samples | Shared Object | Symbol |
| ---: | ---: | --- | --- |
| 1.35% | 98 | `libc.so.6` | `[.] _int_malloc` |
| 1.34% | 75 | `libstdc++.so.6.0.35` | `[.] std::basic_istream<char, std::char_traits<char> >& >` |
| 1.29% | 97 | `libstdc++.so.6.0.35` | `[.] std::basic_filebuf<char, std::char_traits<char> >::>` |
| 1.21% | 95 | `[kernel.kallsyms]` | `[k] entry_SYSRETQ_unsafe_stack` |
| 1.20% | 102 | `[kernel.kallsyms]` | `[k] futex_q_lock` |
| 1.13% | 77 | `libc.so.6` | `[.] __memmove_avx_unaligned_erms` |
| 1.06% | 75 | `libstdc++.so.6.0.35` | `[.] std::ostream::put(char)` |
| 1.01% | 73 | `libstdc++.so.6.0.35` | `[.] std::ostream::sentry::sentry(std::ostream&)` |

### 3. 热点分布与路径拆解

1. **互斥锁竞争与 futex 调度开销显著**：`native_queued_spin_lock_slowpath`（15.80%）、`entry_SYSRETQ_unsafe_stack`（6.94% / 1.21%）、`pthread_mutex_lock`（4.32%）、`__GI___lll_lock_wait`（4.02%）、`pthread_mutex_unlock`（3.17%）、`futex_wait_setup`（2.42%）、`futex_wake`（2.15%）、`_raw_spin_lock`（1.91%）及 `futex_q_lock`（1.20%）合计构成了最主要的热点类别，并与 `perf stat` 中 `351,916` 次上下文切换及 `1.781 s` 内核态时间互相印证。该部分同时涵盖 Sync 外部 `std::mutex` 与 Async 队列 `mutex_`/条件变量等待唤醒。
2. **堆内存分配与数据拷贝可见于用户态热点**：`__libc_malloc2`（2.43%）、`_int_malloc`（1.35%）与 `__memmove_avx_unaligned_erms`（1.13%）直接反映了 64 字节 payload 超过 SSO 上限触发的动态分配及缓冲区拷贝。
3. **文件流读写与基准校验并存**：`std::basic_ostream`、`std::basic_filebuf`、`std::ostream::put` 与 `std::ostream::sentry` 归因于 Sync 与 Async 两条路径的文件写入；`std::basic_istream`（1.34%）则来自计时外 `verify_line_count` 对全部 40 份输出文件（共 4,000,000 行）的逐行读回校验。

## 正确性与并发压测汇总

| 验证项 | 构建与命令 | 实测结果 |
| --- | --- | --- |
| 单生产者行数校验 | `build-v17-release/tinylog_single_producer_bench`（5 次） | 10/10 轮通过，每轮 `100,000/100,000` 行 |
| 多生产者行数校验 | `build-v17-release/tinylog_multi_producer_bench` | 40/40 轮通过，每轮 `100,000/100,000` 行 |
| 背压分析行数校验 | `build-v17-release/tinylog_backpressure_bench` | 300/300 轮通过（30 轮预热 + 270 轮正式测量），每轮行数严格等于对应档位日志量 |
| 末轮输出文件逐字节复核 | 对三项基准留存的全部末轮日志独立校验 | **40/40 份**文件（单生产者 2 份 + 多生产者 8 份 + 背压 30 份）逐字节匹配 `[INFO] ` + 64 个 `'x'` + `'\n'`（每条 72 字节） |
| Release CTest | `ctest --test-dir build-v17-release -C Release --output-on-failure` | **7/7 通过**（耗时 `0.02 sec`），含 `logger_basic`、`async_logger_basic`、`async_flush_failure_rejects_log`、`async_flush_failure_releases_waiters`、`releases_same_batch_flush_failure`、`releases_same_batch_write_failure`、`async_logger_backpressure` |
| TSan CTest | `ctest --test-dir build-tsan --output-on-failure` | **7/7 通过**（耗时 `0.14 sec`），零 Data Race 或异常退出 |
| TSan 异步/同批故障/背压重复压测 | `ctest --test-dir build-tsan -R "async\|releases_same_batch" --repeat until-fail:25 --output-on-failure` | **6 项测试 × 25 次（共 150 次）全部通过**（耗时 `2.99 sec`，0 失败） |

## V1.8 结论与残留瓶颈

1. **批量 `swap`、锁外构造与按需通知在全矩阵上显著提升了 `AsyncLogger` 吞吐，并在多生产者场景反超外部加锁的 Sync 对照组。**
   - 单生产者基准下，Async 中位数完成吞吐量由 V1.7 的 `2,449,865 logs/sec`（`40.819 ms`）提升至 `11,324,538 logs/sec`（`8.830 ms`），提速 **4.62x**，达到无锁单线程 Sync（`12,458,169 logs/sec`）的 **90.9%**。
   - 多生产者基准下，Async 在 `2`、`4`、`8` 生产者下分别提升 **3.14x**、**4.52x**、**3.91x**，并以 `5.82 M`、`4.69 M`、`3.71 M logs/sec` 全部超过 `Sync (external mutex)`（`5.14 M`、`4.67 M`、`3.23 M logs/sec`）。
   - 8 生产者背压实验下，Async（Shutdown 与 Flush）在从 `512` 到 `100,000` 条的全部 5 个规模档位上均超过 Sync（`1.17x–1.35x`）；在 `8,192` 与 `100,000` 条高背压区间提速尤为明显，达到 V1.7 的 **3.99x–4.44x**。
2. **背压特征从 V1.7 的“过半容量后吞吐断崖跌落”转变为“随批量增大而提升至稳定平台”。**
   - V1.7 中 1 生产者 Async 在跨越 `1,024` 队列容量至 `2,048` 时吞吐量从 `3.90 M` 跌至 `2.44 M logs/sec`；而 V1.8 的 1 生产者 Async（Shutdown）从 `512` 条到 `100,000` 条单调升至 `8.20 M logs/sec`（`2,048` 条为 `5.61 M`），8 生产者 Async 在 `1,024`–`100,000` 条区间稳定在 `3.62 M–3.87 M logs/sec`。
   - 不过在 1 生产者场景下，随日志量从 `512` 增至 `100,000`，`T_submit / T_total` 仍从 `35.25%–38.26%` 升至 `99.10%–99.27%`；1,024 条（不触发满队阻塞）均摊提交耗时约为 `97.7 ns/条`（Shutdown）与 `115.2 ns/条`（Flush）。
3. **当前已观测到的残留瓶颈：**
   - **每条日志的堆分配与释放开销尚未消除**：64 字节 payload 超过 libstdc++ 15 字节 SSO 上限，即使移至锁外，单次 `log()` 仍需分配 `std::string` 并由后台消费后释放（在 `perf report` 中也可见 `__libc_malloc2`、`_int_malloc` 与 `__memmove_avx_unaligned_erms` 热点）。单线程/单生产者无锁竞争时，这一额外分配使单生产者 Async 中位数（`8.830 ms`）仍略慢于直接格式化写入缓冲区的 Sync（`8.027 ms`）；多生产者 `P=1`（`12.855 ms` vs `8.022 ms`）与 1 生产者 100,000 条背压（`8.20 M` vs `13.71 M logs/sec`）中 Async 同样低于 Sync。
   - **单把共享 `mutex_` 下的并发扩展性仍随线程数下降**：在多生产者基准中，生产者从 `1` 增至 `8` 时，Async `T_submit` 从 `12.761 ms` 升至 `26.743 ms`，吞吐量由 `7.78 M` 降至 `3.71 M logs/sec`（下降约 52%）；8 生产者 `100,000` 条背压下 `T_submit / T_total` 仍达 `99.52%–99.68%`（混合 `perf` 观测中亦以 `native_queued_spin_lock_slowpath`、`pthread_mutex_lock`、`__GI___lll_lock_wait` 与 `futex_*` 及内核态切换开销最为突出），表明所有生产者与后台 worker 之间围绕单一 `mutex_` 的抢占及满队列 `cv_not_full_` 等待/唤醒仍主导高并发提交耗时。
   - **小批量唤醒与排空尾延迟**：1 生产者 `512` 条下 Async 的 `T_tail` 中位数为 `0.060–0.069 ms`，仍占总耗时的 `61%–65%`，因此在极短突发规模上总耗时仍高于同步写入。
   - 注：当前仓库尚未实现无锁 SPSC/MPSC 队列或对象池复用，本文仅记录上述 V1.8 实测瓶颈。
