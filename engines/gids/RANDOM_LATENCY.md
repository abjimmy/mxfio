# GiDS 随机读写 AP 延迟

在 [固定 LBA 测量](AP_LATENCY.md) 基础上，用 `gids_ap_plan` 和 fio `read_iolog`
重放随机地址列表。先按 [新机只读验收](README.md#新机只读验收) 配置 GPU 和设备变量。
写测试会覆盖选中 namespace 内的抽样地址，只能对允许覆盖的专用测试盘执行。

每组一个 job、`thread=1`、`iodepth=1`；建议先测 4 KiB，再测 128 KiB。
默认生成 5200 笔请求并剔除最初 200 笔预热，报告剩余全部样本。
生成器按块大小对齐，在 `[0, capacity)` 内均匀有放回抽样，末尾不越界；
固定 seed 可重放同一序列。全盘地址抽样不表示访问每个扇区，也不是稳态预处理。

## 测量和校验方法

读请求：初始化阶段先按完整随机列表读取一遍，将每笔请求的参考数据留在显存，分别占约 20.3 MiB／650 MiB。随后才重放计时请求；没有在每笔计时读之前立即读同一个 LBA。AP 在打点前将接收缓冲区填为参考数据的按位取反，发起读，观察 CQE，读取并比较首个 64-bit 字，然后逐字比较整块数据。预读可能温热 SSD 内部缓存，因此这是**经过参考预读的随机地址测试**，不是严格冷读。

写请求：先在 GPU 按 seed、offset、sample index、word index 生成确定性 payload，再由 AP 发起写。AP 观察完成并处理状态后记录 `ap_done`。之后在计时区间外，将显存缓冲区填为写入值的按位取反、对相同地址发起读回、逐字比较整块；校验失败时整笔样本无效。写后读回也是 GPU 直达数据通路。

| 指标 | 区间 | 含义 |
|---|---|---|
| 读主指标 | `first_read - sq_begin` | AP 开始写 SQE/capsule → AP 读取并校验显存首字 |
| 读／写 CQ 指标 | `cq_seen - sq_begin` | AP 开始写 SQE/capsule → 观察到完成 CQE |
| 写 AP 完成指标 | `ap_done - sq_begin` | 加上 CQ 后的队列处理和完成状态检查 |
| 读整块串行校验 | `all_read - sq_begin` | 额外包含单 lane 读取显存参考副本并逐字比较，不能当作纯 DMA 时间 |

PCIe 的 `sq_begin` 在 Tutti 向 SQ 复制 SQE 之前，`cq_seen` 在找到匹配 NVMe CQE 时；RDMA 的起点在构造命令 capsule 之前，终点为收到响应的 mlx5 CQE，后续仍需检查 NVMe CID/status。沿用同一 realtime GPU 计数器和 job 前后校准，拒绝频率偏差超过 2%、时间戳倒序、错误状态、缺样本或校验不匹配的运行。

写测试未设置 FUA，也没有插入 Flush；报告命令完成延迟。写后读回验证数据可见性，不能据此推断掉电持久化保证。

GPU payload 准备、读参考预载、kernel launch、CPU 调度、状态回收和写后读回均不计入主 AP 区间。逐笔整块验证及主机调度使请求之间存在间隔；写请求之间还插入了一笔验证读。因此这是带逐笔校验的低负载写延迟，不是连续纯写的饱和测试。fio 默认 `slat/clat/lat` 包含不同范围的上述开销，不能代替这里的 AP 时间。

## 复现

先按 [README.md](README.md) 编译插件及准备匹配的 snvme 模块。本文使用独立产物 `build-random/gids.so`。

```bash
make -C engines/gids BUILD=build-random -j8
make -C engines/gids BUILD=build-random test
: "${MACA_PATH:?set the MACA SDK used to build the plugin}"
export LD_LIBRARY_PATH="$MACA_PATH/lib:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

dev="${NVME_NS:?}"  # 已核对的空闲 namespace
capacity=$(blockdev --getsize64 "$dev")
bs=4096
prefix="$(mktemp -d /srv/tutti/logs/ap-random-XXXXXX)/read-4k"
python3 engines/gids/scripts/make_ap_plan.py \
  --capacity="$capacity" --bs="$bs" --count=5200 --seed=20260921 \
  --operation=read --filename="$dev" --prefix="$prefix"
./fio --readonly --name=ap-random --thread=1 --iodepth=1 \
  --ioengine="$PWD/engines/gids/build-random/gids.so" \
  --gids_gpu="${GIDS_GPU:?}" --gids_transport=pcie --filename="$dev" \
  --rw=read --bs="$bs" --size="$capacity" \
  --read_iolog="$prefix.iolog" --replay_no_stall=1 \
  --gids_ap_plan="$prefix.plan" --gids_ap_latency="$prefix.csv" \
  --eta=never --output-format=json --output="$prefix-fio.json"
python3 engines/gids/scripts/summarize_ap.py "$prefix.csv" \
  --warmup=200 --output="$prefix-summary.json"
```

128 KiB 使用 `bs=131072` 并重新生成匹配计划。写测试会覆盖计划地址的原数据：仅对已授权盘使用新 prefix，以 `--operation=write` 生成计划，fio 改为 `--rw=write` 并移除 `--readonly`；其余参数相同。所有输出路径必须尚不存在。

RDMA 先用 `make -C engines/gids BUILD=build-random GIDS_RDMA=mlx5 -j8` 启用 mlx5 后端。
使用目标 namespace 的实际容量以及逻辑 `filename=rdma` 重新生成匹配计划和 iolog，将 PCIe 参数替换为：

```bash
--gids_transport=rdma --filename=rdma \
--gids_traddr="${RDMA_ADDR:?}" --gids_trsvcid="${RDMA_PORT:?}" \
--gids_subnqn="${RDMA_SUBNQN:?}" --gids_nsid="${RDMA_NSID:?}" \
--gids_hostnqn="${RDMA_HOSTNQN:?}"
```

读计划最多 65,536 笔且参考副本不超过 2 GiB；写计划最多 65,536 笔、只保留一块参考数据。计划容量必须与控制器 Identify/实际块设备容量一致，偏移按请求块大小及 namespace LBA 对齐。新增 AP 随机计时需要计划重放；普通 GiDS 引擎的 `randread/randwrite` 功能不受此限制。

## 验收和解释

逐笔核对 CSV 与计划的操作、偏移和长度，要求全部 `status=0`、`mismatches=0`，
实际完成计数与计划一致。保存计划、iolog、CSV、fio JSON、源码版本和设备身份。
初始化参考预读及每笔写后的验证读不属于 fio 计时请求计数。

比较结果时保持 seed、容量、块大小、GPU、队列深度及 SSD 预处理状态一致。
参考预读会影响缓存；低队深且插入校验读的写入不能外推为连续纯写性能。
固定 LBA 与随机地址、不同设备之间的延迟不应直接相减解释为某个单独组件的开销。
测试结束检查控制器驱动绑定、GPU 进程以及 SSD 健康状态，见 [RECOVERY.md](RECOVERY.md)。
