# GiDS 可配置并行 I/O

目标：fio 可通过参数选择 GiDS 的提交方式和并行资源，搜索给定 GPU、SSD、拓扑及负载下的最高可持续性能。Tutti 的 SSD I/O 调度是这个参数空间的一个子集。不能把某组旧 fio 参数的峰值当成 GiDS 或 Tutti 的硬件上限。

本版加入直接 PCIe 后端的多文件融合提交、多个 SQ/CQ、GPU worker 和 block 大小配置。2026-10-13 已在 146 的 MACA 3.9.0.18 环境完成构建、单元测试、两种模式的只读数据比对及第一轮性能短测；硬件验收仍有一条来源未定位的管理队列错误记录，见下文。整批回收、层预提交和计算依赖的差异尚未消除。不承诺当前补丁已经覆盖 Tutti 的全部流水线能力或达到 SSD 最大性能。

## 146 首轮快速验证

GPU0 / NUMA0、Solidigm 三块 Gen5×4 加一块 Gen4×4，51 组只读测试；候选配置各做两次 20 秒复测。现场结果保存在 SICTS 工程的 `results/gids-parallel-146-20261013/index.html`，包含原始 fio JSON、配置、源码摘要和恢复记录，不提交现场数据。

| 负载 | 新版复测 | 本次 CPU libaio 参考 | 新版配置 |
|---|---|---|---|
| 128 KiB read | 30.76–31.75 GiB/s | 48.56 GiB/s | 每盘一个 job，总 jobs=4；batched；每盘 8 QP；gids_qd=32；iodepth=1024；block_threads=1 |
| 4 KiB randread | 435.18–438.19 万 IOPS | 440.22 万 IOPS | 每 job 融合四盘，numjobs=4；workers；每盘每 job 32 QP；workers/job=1024；iodepth/job=4096；gids_qd=0；block_threads=32 |

两种配置均 `iodepth_batch_submit=iodepth`、`gids_host_buffers=0`。4 KiB 复测约为旧版本次单盘单 QP、每盘一个 QD128 job 对照配置的 11.25–11.32 倍；旧版没有穷尽调参，不能把该倍数当成相同并发资源下的纯插件收益。CPU 参考的数据终点是主机内存，GiDS 是 GPU 内存。大块读仍有明显差距，本轮短测不代表长期稳定性或硬件上限，也未直接比较 Tutti E2E。

两种模式的 128 KiB 与 4 KiB 读回校验各通过 765 请求，覆盖多盘、重复及部分批次；51 组 fio 数据 I/O 均无报错。但主扫描结束时 Solidigm A 新增一条 SQ0 `DATA_XFER_ERROR` 管理命令错误，具体来源尚未定位，原恢复守卫失败记录保留。核对并恢复后，后续每组检查 SMART，错误计数未再增加；媒体错误、写入计数、unsafe_shutdowns 未增加，未见新的 kernel I/O/GPU/AER 故障。最终恢复原始驱动绑定状态且无遗留 GPU 测试进程；不能把本轮记作全项无异常的硬件验收。

## 143 完成回传对照

2026-10-15 在 C500 GPU0、MACA 3.9.0.18、ACS 重定向关闭的条件下，对比
相同并行 I/O 实现的两种最终 D2H 提交时机。A/B 为 Gen5×4 SSD，D 为 Gen4×4；
每盘一个 job，128 KiB randread，iodepth/job=1024、每盘 8 QP、gids_qd=32、
block_threads=1、iodepth_batch_submit=1024、gids_host_buffers=0。
每组 20 秒测量加 5 秒预热，两个版本各重复三次。以下是各 job 共同测量窗口
的吞吐中位数，单位为十进制 GB/s；带追踪的两次运行不计入性能中位数。

| 盘组 | 提前排入最终 D2H | kernel 完成后排入 D2H | 中位数提升 |
|---|---:|---:|---:|
| AD | 18.24 | 19.98 | 9.5% |
| ABD | 21.64 | 32.76 | 51.4% |

旧版 ABD 三次为 21.39–27.68 GB/s，新版为 32.75–32.76 GB/s；51.4% 是
这组测试的中位数变化，不是固定收益。另一次新版四盘 ABCD 测得 41.09 GB/s，
只有单次结果，没有同条件旧版四盘对照，不能计算四盘提升比例。

时间线中快 job 的单批 kernel 平均仍约 9.5 ms，但相邻批次 kernel 启动间隔
由约 18.96 ms 降至 10.62 ms。D2H activity 平均由约 8.45 ms 降至 0.003 ms；
该 activity 包含依赖等待，不能解释为实际复制带宽提升几千倍。
收益来自减少完成回收等待并更早提交下一批，批次内部 SQ/CQ 调度没有改变。
单 job 仍整批回收，未实现逐请求补充新批次、持久 kernel 或计算重叠。

这批结果的源码、构建、功能与设备恢复记录保存在 SICTS 工程
`results/engines-143-plan-20261014/completion-fix-report.html`，原始数据单独归档。
只读功能覆盖及尚未验证的路径见 [TESTING.md](TESTING.md#当前实现的验证范围)。

## 参数的独立含义

| 资源 | 参数 | 作用范围 |
|---|---|---|
| SSD / namespace | `filename=/dev/NS0:/dev/NS1` | 一个 job 的目标列表；使用 fio 的文件选择策略分配 I/O |
| SQ/CQ 对 | `gids_qps=1..32` | 每文件、每 job；本版 vendor ABI 每队列组上限 32，控制器总配额还可能更低 |
| GPU stream | `numjobs` / 独立 job 段 | 每 job 一个独立 stream 和独立队列组 |
| fio 请求容量 | `iodepth=1..65536` | 每 job、所有盘合计；不是每盘或每 QP 的深度 |
| 每次提交规模 | `iodepth_batch_submit` | 每个 job 的一次 GPU 批次；吞吐扫描通常先设成 iodepth |
| 每 QP 在途上限 | `gids_qd` | 0 使用硬件环容量减一；显式值不能超过该容量 |
| 提交方式 | `gids_submit_mode=batched/workers` | 见下节；默认 batched |
| worker 数 | `gids_workers` | workers 模式整个 job 的 worker 数；0=256，最多 iodepth |
| block 线程数 | `gids_block_threads` | 0=32；实际取设置值与本批工作线程数的较小者；grid 向上取整 |
| 块大小、访问分布 | `bs`、`rw`、`file_service_type` | 由 fio 产生请求；不同负载须分别搜索和报告 |

RDMA 仍为每 job 单目标、单数据 QP，拒绝这些非默认 PCIe 参数。QD1 AP 分析只接受默认单文件、单 QP 模式。普通 fio 的 read/write/randrw、host_buffers 和 verify 语义保持不变。

## 两种提交方式

`batched`：主机将本批请求按目标 SSD 和 QP 稳定分组，同一 SSD 的请求轮流分给各 QP。每个非空分组交给一个 GPU 线程，此线程独占其 SQ/CQ，批量填写 SQE、敲 doorbell、按 CQE/CID 回收并补充命令。不同 SSD/QP 在同一 kernel 中推进；CID 是分组内的编号，完成后映射回原始 fio slot。单盘、单 QP、默认 block 参数继续走原有 kernel。

`workers`：多个 GPU 线程从同一个原子游标取请求，请求保存目标 SSD。线程按全局 `thread_id % gids_qps` 选择该 SSD 的队列，调用 vendor 中 Tutti 原有的共享 SQ/CQ 提交和完成原语，每线程最多保有一条命令；完成后取下一条。这覆盖当前 Tutti 融合读 kernel 的主要调度形态，不包含 Runtime、connector、KV 文件布局或逐层 event。

worker 数限制整个 GPU 批次的并发，不是每盘各自拥有这么多 worker。为避免某批请求全部落到一块盘时填满队列，初始化要求：

```text
effective_workers = min(iodepth, gids_workers or 256)
window[d] = min(iodepth, gids_qd or (ring_entries[d] - 1))
ceil(effective_workers / gids_qps) <= min(window[d])
```

不满足时明确报错，增加 QP 或减小 worker 数即可调整。不会静默减少 worker，也不会因软件 QD 设置改变物理 SQ/CQ 环大小。控制器不能提供请求的 QP 数时同样报错。

对一个批次，`N[d,q]` 表示分给该盘该队列的请求数：

```text
batched 在途上限 <= sum(d,q, min(N[d,q], window[d])) <= iodepth
workers 在途上限 <= min(本批请求数, effective_workers)
```

这些是容量上限，实际在途数还取决于 GPU 调度、请求分布、SSD 延迟及阶段性排空。增加 iodepth 可以延长批次，不能替代增加 worker 或 QP。

`gids_block_threads=1` 可使各 owner/worker 落在不同 block；32 可表达 Tutti 常用形态，64/128 等可比较 block 布局。block 数不是实际占用计算单元数，需要设备时间线确认。不要先验认为更多线程或更小 block 必然更快。

## 单 job 多盘示例

先按 [README](README.md) 核对 GPU、独占控制器、驱动及容器设备映射，再设置变量。以下配置只读取每盘 offset=1 GiB 后的 1 GiB 范围，每盘需至少 2 GiB。设备名不能从其他节点照搬。

```bash
export GIDS_ENGINE="$PWD/engines/gids/build/gids.so"
export GIDS_GPU=0
export GIDS_NAMESPACES='/dev/REPLACE_NS0:/dev/REPLACE_NS1'
./fio --readonly engines/gids/examples/pcie-parallel.fio \
  --output-format=json --output=parallel-workers.json
```

示例使用 `numjobs=1`、两个目标、每盘 32 QP、总计 256 worker、iodepth=2048、32 线程/block。大批次启动时最多 8 个工作 block、64 个已分配 QP，总在途容量上限 256 条。即使均匀分盘，也只是期望约每盘 128 条、每 QP 4 条；动态取任务不保证这种分布。

本版本 fio 将 `filesize=2g` 配合 `offset=1g` 解释为每文件的 [1 GiB, 2 GiB) 范围；`size` 是 job 的总量，不能把原单盘 size 原样解释成每盘容量。`file_service_type=roundrobin` 有助于同一批次包含多个 SSD。生产 KV 的 chunk striping 和请求分布需要另做匹配；文件列表本身不会自动创建 stripe 文件布局。

同样负载可用下面的覆盖参数对照队列 owner 模式：

```bash
./fio --readonly engines/gids/examples/pcie-parallel.fio \
  --gids_submit_mode=batched --gids_workers=0 \
  --gids_qps=8 --gids_qd=32 --gids_block_threads=1 \
  --output-format=json --output=parallel-batched.json
```

两盘时，此配置最多 16 个 owner block、每 QP 32 条、总计 512 条在途命令，仍受实际批次分盘限制。示例参数是比较起点，不是推荐的最优值。

默认 `gids_host_buffers=0` 时只搬运请求和完成信息，数据留在 GPU；开启 verify 后包含 H2D/D2H，正确性测量须与纯性能结果分开。

## 目前仍存在的限制

- 一个 job 只有一个在途批次，GPU 完成、fio 收完全部事件后才提交下一批。Tutti 可在 host 侧预排后续层；当前 fio 尚不能用单 job 复现这部分流水线。可以用更大批次和多个 job 测试空隙是否被摊薄，但若还有瓶颈，需要增加批次预提交或持续提交机制。
- 多盘在同一 job 的 fio JSON 是合计统计。核对逐盘流量需要设备计数/时间线或独立诊断；不能仅凭总带宽推断所有盘都忙。
- GPU payload 对每个 SSD 分别登记，使用该设备自己的 IOVA/PRP-list；不能假设不同控制器映射相同。当前各 SSD 均登记整个 job 的 GPU buffer，盘数和 iodepth 增加时映射资源也会增长。
- worker 模式复用 vendored Tutti 的原语。共享队列争用、回绕和超时必须重新验收，旧单线程 fio 结果不能作为新模式的通过证据。
- 增加 stream 还受 MACA 执行队列策略影响，测试需记录实际环境值；单个 stream 内的 block 并发另行测量。

## 验收和搜索顺序

1. 在 Linux/MACA 上重新构建 fio/plugin，运行 `make -C engines/gids test` 和 `--enghelp`。默认单盘参数先回归；分别验证两种新模式的单盘、2/3/4 盘与少量/较多 QP。AP、RDMA 参数隔离需保持有效。
2. 使用稳定的已知内容进行只读逐字节对照，覆盖不同容量/LBA、不同 PRP 地址、跨 GPU 页 PRP-list、部分批次和不均匀分盘。需要写校验时只用获准覆盖的专用范围。
3. 覆盖每个 IO 恰好完成一次、重复批次与 CQ/SQ 回绕、初始化中途失败、资源不足、超时和退出排空。核对测试前后控制器绑定、GPU buffer 和 DMA 映射的释放。
4. 固定 GPU/NUMA/盘组/范围/bs/rw，保留旧模式作为基准；先用单 job 分别扫描 batched 的 QP×QD，以及 workers 的 QP×worker。起点 QP=1/4/8/16/32，QD=8/32/128，worker=32/128/256/512/1024；由容量检查过滤不可行项。先粗扫、再在较优区域调整 iodepth/batch 和 block 布局，最后检查 numjobs=2/4 的收益，不把它作为唯一并发轴。
5. 若某组 numjobs 在小 QD 较慢，不据此跳过其全部高 QD 组合。记录实际扫描覆盖和未测项；每个候选最优点至少重复两次并加持续运行，报告波动、错误、GPU/CPU占用、逐盘流量和实际配置。
6. 与 Tutti 对照时固定同硬件、同请求大小和分盘分布、同 QP/worker 资源，区分设备 I/O 与 KV/E2E 时间。若相同负载下 fio 最佳结果仍显著低于 Tutti，应继续定位 fio/adapter 的提交或回收限制；不能把该 fio 结果发布为 GiDS 性能上限。

“最大性能”的验收口径是指定硬件及负载下，扩大可用并行资源已无可重复收益，且已排查测试器本身限制。PCIe/NUMA 拓扑或设备能力形成的上限需有测量证据，不以总带宽平台形状直接推断。
## GPU write payload

`gids_buffer_seed=20261014` initializes the GPU data pool with reproducible
pseudorandom 64-bit words before I/O starts. With `gids_host_buffers=0`, slots
reuse this pool: fio `refill_buffers` and `buffer_pattern` do not transfer new
host payloads to the GPU. This measures storage I/O using resident data, not
the cost of generating or copying fresh data per request. Pure reads can
overwrite the pool; mixed read/write workloads therefore do not guarantee
the initial pattern remains in a reused slot. Use `gids_host_buffers=1` for
fio-controlled data patterns and verification, and report its copy overhead
separately. Adapter callers must rebuild with the updated `adapter.h`.
