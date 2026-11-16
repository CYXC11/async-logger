# V1.7 Logger 性能基线

## 测试环境快照

| 项目 | 采集值 |
| --- | --- |
| 时间 | 2026-11-16 21:07:27–21:07:33（Asia/Shanghai，UTC+08:00） |
| Git HEAD | `6197a20`（`git rev-parse --short HEAD`）。实测使用含未提交 V1.7 基准代码的工作区已有二进制。 |
| CPU | Intel Core i7-14650HX，x86_64，16 核、24 个逻辑 CPU（`lscpu`） |
| 内存 | 31.05 GiB（`/proc/meminfo` 的 `MemTotal` 为 32,557,176 KiB） |
| 电源 | 已插电。测前测后均为 `ACAD/online=1`、`BAT1/status=Full`。 |
| CPU 调频 | `cpu0` 的 `scaling_governor=powersave`（测前测后一致），驱动为 `intel_pstate`，采集前 EPP 为 `performance`。 |
| OS 与内核 | Ubuntu 26.04.1 LTS，Linux `7.0.0-43-generic`（`uname -r`） |
| 文件系统 | 仓库与运行目录均为 `/dev/nvme1n1p3` 上的 ext4（`df -T . build-v17-release`）。 |
| 工具链 | GCC/G++ 15.2.0（Ubuntu `15.2.0-16ubuntu1`），libstdc++，CMake 4.2.3，Unix Makefiles |
| 构建 | CMake `Release`，三个目标的 `CXX_FLAGS` 均为 `-O3 -DNDEBUG -std=c++20`。 |
| 后台负载 | 采集串行执行，期间未并行启动其他测试。用 `vmstat 1 4` 检查系统负载，跳过首行的开机以来平均值。测前三个 1 秒采样的 CPU 空闲率为 94%、97%、97%，测后为 97%、91%、98%。 |

本次直接执行 `build-v17-release` 中的成品。二进制 SHA-256 如下。

```text
cb4c424ed1b3854c11bfd3447e261165aaa00326b6830ac59288c0fe62052497  tinylog_single_producer_bench
489d08f750098d9fec53265b4f5fde7d75898eccc7ab6c1f6569110483bba55a  tinylog_multi_producer_bench
e3db7b8985f37585bcd302551fbc7f4ab3db6813420551249029b50db65fb505  tinylog_backpressure_bench
```

## 复现命令

在仓库根目录运行。采集顺序固定为单生产者五次、多生产者一次、背压分析一次。CTest 在三个基准完成后单独运行。

```bash
(
    set -e
    export LC_ALL=C TZ=Asia/Shanghai
    cd build-v17-release
    vmstat 1 4
    date -Is
    cat /sys/class/power_supply/ACAD/online /sys/class/power_supply/BAT1/status \
        /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
    for run in 1 2 3 4 5; do
        printf '\nSingle producer, invocation %s\n' "$run"
        ./tinylog_single_producer_bench
    done
    ./tinylog_multi_producer_bench
    ./tinylog_backpressure_bench
    date -Is
    cat /sys/class/power_supply/ACAD/online /sys/class/power_supply/BAT1/status \
        /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
    vmstat 1 4
) && ctest --test-dir build-v17-release -C Release --output-on-failure
```

## 测量边界

- 计时使用 `std::chrono::steady_clock`。旧文件清理、payload 构造、Logger 构造和析构、行数校验都在计时外。
- `T_submit` 截止到全部 `log()` 调用结束。单生产者基准直接在主线程执行循环。后两个基准由 `std::latch` 在计时起点统一放行生产者，提交耗时包含唤醒和完成同步，生产者线程创建与 `join()` 在计时外。
- `T_total` 从同一起点计至完成 API 返回。同步调用 `flush()`，后两个基准的同步组在外部 `std::mutex` 下调用。异步 `shutdown()` 等待 worker 排空队列、刷新并退出。异步 `flush()` 等待队列中的刷新请求完成，worker 在计时外随 Logger 析构收尾。
- 完成口径到日志写入 `std::ofstream` 并由 `flush()` 刷入 OS page cache 为止。`fsync()` 及物理磁盘持久化在测量范围之外。吞吐量按日志总量除以 `T_total`。
- 三个基准共用 64 字节 payload，计时前构造。当前 libstdc++ 的 `std::string` 对 `char` 的 SSO 容量为 15 字节，异步 `log()` 在互斥锁内把 64 字节 payload 拷贝进持有内容的 `std::string` 时会发生堆分配。

## 单生产者（V1.7-01）

每个进程对 Sync 和 Async 各跑 1 轮，无预热，固定先同步后异步。每种 Logger 每轮提交 100,000 条。下表列出五次调用和逐列中位数，同步直接使用 `Logger`，异步以 `shutdown()` 收尾。

| 次数 | Sync T_submit (ms) | Sync T_total (ms) | Sync (logs/sec) | Async T_submit (ms) | Async T_total (ms) | Async (logs/sec) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 7.941 | 7.945 | 12,586,825 | 40.695 | 40.819 | 2,449,865 |
| 2 | 10.105 | 10.112 | 9,889,698 | 43.311 | 43.436 | 2,302,252 |
| 3 | 9.249 | 9.253 | 10,806,910 | 39.019 | 39.145 | 2,554,625 |
| 4 | 6.817 | 6.821 | 14,661,568 | 45.248 | 45.370 | 2,204,106 |
| 5 | 10.461 | 10.467 | 9,553,411 | 38.592 | 38.749 | 2,580,701 |
| **中位数** | 9.249 | 9.253 | 10,806,910 | 40.695 | 40.819 | 2,449,865 |

## 多生产者（V1.7-02）

每种 Logger 对 1、2、4、8 个生产者各测 5 轮，逐列取中位数。每轮先同步后异步，总量固定为 100,000 条并按生产者均分。Sync 使用基准侧外部 `std::mutex` 包装 `Logger`，Async 以 `shutdown()` 收尾。

| 生产者 | Logger | T_submit (ms) | T_total (ms) | 完成吞吐量 (logs/sec) |
| ---: | --- | ---: | ---: | ---: |
| 1 | Sync (external mutex) | 9.628 | 9.632 | 10,381,998 |
| 1 | Async (shutdown) | 42.460 | 42.558 | 2,349,752 |
| 2 | Sync (external mutex) | 16.830 | 16.851 | 5,934,523 |
| 2 | Async (shutdown) | 53.786 | 53.889 | 1,855,653 |
| 4 | Sync (external mutex) | 20.993 | 21.008 | 4,760,196 |
| 4 | Async (shutdown) | 96.129 | 96.228 | 1,039,200 |
| 8 | Sync (external mutex) | 31.272 | 31.275 | 3,197,404 |
| 8 | Async (shutdown) | 105.040 | 105.143 | 951,090 |

## 背压实验（V1.7-03）

每种模式在每个生产者数量和日志量组合下先做 1 轮不计时预热，再跑 9 轮，轮换三种模式的执行顺序，使每种模式在每个顺位出现三次。日志量按生产者均分。两个 Async 组共用提交路径。

表格报告逐列中位数。`T_tail = T_total - T_submit` 和 `T_submit / T_total` 都用每轮原始时间计算，再分别取中位数。完成吞吐量以 `T_total` 的中位数计算。

### 1 个生产者

| 日志量 | Logger | T_submit (ms) | T_total (ms) | T_tail (ms) | T_submit / T_total | 完成吞吐量 (logs/sec) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 512 | Sync (external mutex, flush) | 0.065 | 0.068 | 0.004 | 94.64% | 7,479,038 |
| 512 | Async (shutdown) | 0.061 | 0.146 | 0.084 | 43.06% | 3,506,201 |
| 512 | Async (flush) | 0.059 | 0.139 | 0.080 | 41.93% | 3,681,308 |
| 1,024 | Sync (external mutex, flush) | 0.113 | 0.116 | 0.003 | 97.41% | 8,802,771 |
| 1,024 | Async (shutdown) | 0.109 | 0.263 | 0.154 | 41.42% | 3,896,974 |
| 1,024 | Async (flush) | 0.112 | 0.254 | 0.141 | 44.51% | 4,034,387 |
| 2,048 | Sync (external mutex, flush) | 0.228 | 0.231 | 0.003 | 98.78% | 8,882,682 |
| 2,048 | Async (shutdown) | 0.684 | 0.839 | 0.156 | 81.49% | 2,441,249 |
| 2,048 | Async (flush) | 0.695 | 0.840 | 0.149 | 82.05% | 2,439,539 |
| 8,192 | Sync (external mutex, flush) | 0.853 | 0.880 | 0.027 | 96.95% | 9,307,748 |
| 8,192 | Async (shutdown) | 3.974 | 4.226 | 0.156 | 96.67% | 1,938,515 |
| 8,192 | Async (flush) | 3.904 | 4.002 | 0.153 | 97.00% | 2,047,152 |
| 100,000 | Sync (external mutex, flush) | 8.081 | 8.095 | 0.020 | 99.78% | 12,352,700 |
| 100,000 | Async (shutdown) | 41.180 | 41.279 | 0.101 | 99.75% | 2,422,526 |
| 100,000 | Async (flush) | 43.791 | 43.945 | 0.099 | 99.77% | 2,275,571 |

### 8 个生产者

| 日志量 | Logger | T_submit (ms) | T_total (ms) | T_tail (ms) | T_submit / T_total | 完成吞吐量 (logs/sec) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 512 | Sync (external mutex, flush) | 0.162 | 0.164 | 0.002 | 98.69% | 3,120,391 |
| 512 | Async (shutdown) | 0.138 | 0.226 | 0.088 | 60.85% | 2,262,084 |
| 512 | Async (flush) | 0.139 | 0.226 | 0.083 | 62.10% | 2,270,017 |
| 1,024 | Sync (external mutex, flush) | 0.350 | 0.364 | 0.008 | 97.68% | 2,816,158 |
| 1,024 | Async (shutdown) | 0.282 | 0.437 | 0.148 | 65.35% | 2,343,416 |
| 1,024 | Async (flush) | 0.281 | 0.427 | 0.150 | 65.70% | 2,397,307 |
| 2,048 | Sync (external mutex, flush) | 0.712 | 0.734 | 0.009 | 98.84% | 2,790,890 |
| 2,048 | Async (shutdown) | 1.337 | 1.434 | 0.101 | 92.02% | 1,427,711 |
| 2,048 | Async (flush) | 1.364 | 1.460 | 0.096 | 93.26% | 1,402,861 |
| 8,192 | Sync (external mutex, flush) | 2.813 | 2.843 | 0.009 | 99.69% | 2,881,701 |
| 8,192 | Async (shutdown) | 9.194 | 9.305 | 0.113 | 98.60% | 880,357 |
| 8,192 | Async (flush) | 9.260 | 9.379 | 0.118 | 98.69% | 873,435 |
| 100,000 | Sync (external mutex, flush) | 31.040 | 31.045 | 0.005 | 99.98% | 3,221,096 |
| 100,000 | Async (shutdown) | 107.124 | 107.228 | 0.104 | 99.90% | 932,592 |
| 100,000 | Async (flush) | 104.995 | 105.108 | 0.096 | 99.91% | 951,401 |

## 基线结论

1. **同步的完成吞吐量在本次单、多生产者各测点均高于当前互斥队列版 Async。** 单生产者基准的中位数为 10.81 M logs/s 对 2.45 M logs/s。多生产者基准在 8 生产者时为 3.20 M 对 0.95 M logs/s。小批量 Async 的提交有时比 Sync 快，排空与刷新会增加完成时间。
2. **多生产者下 Async 进一步退化，额外开销集中于提交阶段。** 固定 100,000 条时，从 1 到 8 个生产者，Async 的 `T_submit` 从 42.460 ms 升至 105.040 ms，完成吞吐量从 2.35 M 降至 0.95 M logs/s。实现上，生产者与 worker 共用队列的 `mutex_`，队列满时生产者在 `cv_not_full_` 上等待。这组数据涵盖锁竞争、队列满等待和调度，分项耗时需要独立打点。
3. **首个可见的背压拐点在 1,024 到 2,048 条之间。** 单生产者 Async 的提交占比从 41%–45% 升至约 82%，8 生产者从 65%–66% 升至 92%–93%。100,000 条时四组 Async 均超过 99.7%，尾耗时中位数为 0.096–0.104 ms。这里的拐点根据负载梯度上的计时变化判定，精确定位队列首次填满需要直接观察队列占用或等待事件。
4. **无需等待队列空位时，单生产者异步提交约为 110 ns/条。** 1,024 条这档等于队列容量，每次入队前至多已有 1,023 条日志。用这一档的 `T_submit` 除以条数，Shutdown 组约为 106 ns/条，Flush 组约为 109 ns/条。这是包含起跑与完成同步的批量均摊值。

## 正确性校验

每轮运行前清理旧日志，计时结束且 Logger 析构后再读回计数。行数不符时基准以非零状态退出。

| 基准 | 本次行数校验 | 独立复核 |
| --- | --- | --- |
| 单生产者 | 10/10 轮，每轮 100,000 行 | 末轮 2 份文件逐字节匹配 |
| 多生产者 | 40/40 轮，每轮 100,000 行 | 8 个组合各 1 份末轮文件逐字节匹配 |
| 背压分析 | 300/300 轮，含 30 轮预热与 270 轮正式测量，每轮行数等于该档日志量 | 30 个组合各 1 份末轮文件逐字节匹配 |

独立复核按每条 `[INFO] `、64 个 `x` 和换行的完整字节序列比对。Release CTest **4/4 通过**。
