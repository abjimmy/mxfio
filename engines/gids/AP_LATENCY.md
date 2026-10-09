# AP 写 SQE 到读取显存数据的延迟

使用 `gids_ap_latency` 在同一个 GPU kernel、同一个 AP lane 上记录时间戳，再单独统计。CPU 调度、kernel launch、结果回收和 CSV 写入均不在主指标的计时区间内。fio 默认的 `slat/clat/lat` 使用不同的计时范围。

本文介绍固定 LBA 只读基线。全 namespace 随机读、随机写的方法见 [RANDOM_LATENCY.md](RANDOM_LATENCY.md)；`gids_ap_plan` 配合 fio `read_iolog` 重放地址列表。
RDMA 测试须先按 [BUILD.md](BUILD.md) 使用 `GIDS_RDMA=mlx5` 构建插件。

## 测量口径

| CSV 字段 | 打点位置 |
|---|---|
| `sq_begin` | PCIe：首次向 NVMe SQ 复制 SQE 之前；RDMA：构造并写入显存中的 NVMe 命令 capsule 之前，包含命令字段构造、后续 mlx5 WQE 构造和发布 |
| `doorbell` | AP 执行 NVMe SQ / NIC UAR 门铃存储之后；表示存储指令已执行，不表示设备已收到门铃 |
| `cq_seen` | PCIe：观察到匹配的 NVMe CQE；RDMA：处理收到响应的 mlx5 CQE |
| `cqe_observed` | 上游 per-entry timing 的同事件第二打点（`cq_poll_bounded` 返回处）；与 `cq_seen` 的差用于互验时钟与钩子对齐。仅 PCIe 路径填写，RDMA 路径恒为 0 |
| `slot_reclaimed` | `cq_dequeue` + `put_cid` 完成后、`submit_*_one` 返回前；即完成槽位回收结束。仅 PCIe 路径填写，RDMA 路径恒为 0 |
| `first_read` | 完成响应检查及队列处理后，AP 通过 volatile load 读到 payload 首个 64 位字，并与预期值比较成功之后 |
| `all_read` | 同一 AP lane 逐个读取并比较整块 payload，全部一致之后 |

主指标是 `first_read - sq_begin`，即按协议确认完成后，AP 首次安全消费数据的延迟。它不是 DMA 最早到达首字节的时刻。`all_read - sq_begin` 另外包含单 lane 串行读取数据、读取参考副本及比较的开销，不能当作纯 DMA 传输耗时或并行消费整块数据的性能上限。

每个 job 首先执行一次不计入样本的读取，将该固定 LBA 的内容在显存内复制为参考值。每次测量前，AP 将目标显存的每个字填为参考值的按位取反，并执行 system fence。这保证上一次读取残留的 payload 不能通过本次校验。测量过程中数据不复制到 CPU；仅请求、状态和时间戳经过主机。磁盘操作全部只读。

N300/MACA 3.7.0 上的 `clock64()` 计数速率会变化，不能按固定频率换算。
本测量使用 `__builtin_mxc_get_realtime()`，在 job 前后分别以主机 steady clock
和映射内存握手进行 20/40/60 ms 校准。三次校准跨度或前后频率偏差超过 2% 时，
拒绝生成有效统计。校准包含主机/GPU 握手误差，测量也未扣除打点开销；
相邻时间戳相等只表示未分辨出时间差，不代表读取计时器没有成本。

本地 NVMe 的三个队列打点在构建时生成的头文件中插入，正常模式不启用这些打点。
`cqe_observed` / `slot_reclaimed` 使用 Tutti 的 `submit_*_one` 可选 timing 参数，
与队列头打点使用同一实时钟。Tutti 的 MACA 计时宏按 10 ns/tick 转为纳秒；
该系数经 N300/MACA 3.7、3.8.2 和 C500/MACA 3.8.2.302 校准支持。
PCIe profiler 按同一系数转回原始 ticks，CSV 时间戳统一使用各 job 实测的 ticks/µs
换算。纳秒单位不代表 1 ns 分辨率或跨 GPU 时钟同步，新型号/SDK 仍需校准。

统计允许旧 CSV 同时缺少两个新增列，也允许 RDMA CSV 两列在所有样本中均为零。只缺一列、部分样本缺打点、时间戳倒序，以及声明为 PCIe 且带有新增列但全零的记录均拒绝统计；warmup 样本也执行相同校验。

## 复现

先按 [README.md](README.md#新机只读验收) 准备插件、GPU、需要的本地 snvme 模块及本机 `NVME_NS` / `RDMA_*` 等变量。以下命令从 mxFIO 根目录执行，CSV 必须是尚不存在的路径；`size=bs` 配合 `loops` 固定读取一个块。不指定 `gids_ap_plan` 时要求 `rw=read`、QD1、`gids_host_buffers=0`、不启用 fio verify，只支持固定 LBA；随机地址和写入使用上面的计划重放模式。

```bash
: "${MACA_PATH:?set the MACA SDK used to build the plugin}"
export LD_LIBRARY_PATH="$MACA_PATH/lib:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export AP_OUT="$(mktemp -d /srv/tutti/logs/ap-read-XXXXXX)"
# 本地 PCIe；只测 RDMA 时跳过这一段。
./fio --readonly --name=ap-pcie --thread=1 \
  --ioengine=$PWD/engines/gids/build/gids.so \
  --gids_gpu="${GIDS_GPU:?}" --gids_transport=pcie --filename="${NVME_NS:?}" \
  --rw=read --bs=4k --size=4k --offset=1G --loops=5200 --iodepth=1 \
  --gids_ap_latency="$AP_OUT/pcie.csv" \
  --eta=never --output-format=json --output="$AP_OUT/pcie-fio.json"

# NVMe-oF/RDMA；只测 PCIe 时跳过这一段。
./fio --readonly --name=ap-rdma --thread=1 \
  --ioengine=$PWD/engines/gids/build/gids.so \
  --gids_gpu="${GIDS_GPU:?}" --gids_transport=rdma --filename=rdma \
  --gids_traddr="${RDMA_ADDR:?}" --gids_trsvcid="${RDMA_PORT:?}" \
  --gids_subnqn="${RDMA_SUBNQN:?}" --gids_hostnqn="${RDMA_HOSTNQN:?}" \
  --gids_nsid="${RDMA_NSID:?}" \
  --rw=read --bs=4k --size=4k --offset=1G --loops=5200 --iodepth=1 \
  --gids_ap_latency="$AP_OUT/rdma.csv" \
  --eta=never --output-format=json --output="$AP_OUT/rdma-fio.json"

# 仅传入实际生成的 CSV；每次 fio 返回成功后再统计。
python3 engines/gids/scripts/summarize_ap.py --warmup=200 \
  "$AP_OUT"/*.csv --output="$AP_OUT/summary.json"
```

128 KiB 测试同时将 `bs` 和 `size` 改为 `128k`，并使用新的输出文件名。GPU ordinal 可能随驱动枚举变化，运行前确认 PCI BDF；每个 CSV 都记录实际 BDF。测试结束后确认指定 NVMe BDF 恢复 `nvme` 驱动。

## 结果解释与检查

每组检查 fio 的错误码、实际 I/O 数量以及 CSV 的 `status` 和 `mismatches`，
再运行统计脚本；不能只凭 fio 的退出码判断样本完整。
保存全部原始样本、软件版本、设备身份、实际命令及校准结果，不删除正式样本的长尾。

PCIe 的 `cqe_observed` → `slot_reclaimed` 区间包含队列回收、原子操作及 CQ 门铃，
`slot_reclaimed` → `first_read` 才是返回后到首字消费的区间。比较传输路径时应分别报告，
不能将本地与 RDMA 的端到端差值直接解释为网络开销。

该负载是单 job、QD1、固定 LBA 的重复读取；参考预读、完整数据校验和请求间隔会影响
设备状态。它不代表冷数据、随机地址或满载吞吐。CPU、SPDK 和 mcFile 的对比方法见
[COMPARISON.md](COMPARISON.md)。
