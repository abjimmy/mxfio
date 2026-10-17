# AP、CPU、SPDK 与 mcFile 的对比方法

先按 [BUILD.md](BUILD.md) 构建 fio 和 GiDS，并按 [新机指南](NEW_HOST.md)
核对空闲 GPU、可独占的 SSD、RDMA 目标和驱动。每条路径单独运行，记录实际源码、
SDK、驱动、设备拓扑、块大小、队列深度、请求数和缓存状态。

## 保持可比的工作负载

建议从 QD1、固定 offset=1 GiB、4 KiB 和 128 KiB 只读开始，每组 5200 次请求，
剔除前 200 次预热。检查每笔请求的偏移和长度一致；不要用每读一块就重开文件的配置，
也不要将不同路径的冷缓存、预读、数据校验或主机复制开销混在一个指标里。

- GiDS AP 指标在 GPU 内计时，包含协议提交至完成后消费数据；见 [AP_LATENCY.md](AP_LATENCY.md)。
- fio `slat/clat/lat` 在主机侧计时。GiDS 的 fio 延迟还包含显存准备、kernel 调度、
  完整 payload 校验及回收，不能与 AP 内部延迟互换。
- CPU 内核路径读取到主机内存；mcFile POSIX 模式还涉及 GPU 数据传输。
  目标内存位置和实际数据路径必须随结果标明。
- SPDK 使用其用户态队列路径；须单独准备与该 fio 构建匹配的插件及驱动绑定。

比较版本时在同机、同设备和同配置上交替运行旧/新版本，保存每次结果并报告波动。
历史机器的单次结果不构成当前版本的性能基线。

## 固定地址只读基线

`scripts/run_baseline.sh` 支持 `kernel`、`posix`、`mcfile`、`spdk`，
`cufile` 是 `mcfile` 的兼容名称。脚本使用 `rw=randread:5201`、
`percentage_random=0`、`rw_sequencer=identical`，在一次打开期间重复访问同一个偏移。
它固定使用 CPU 0；先确认该 CPU 可用，并检查脚本内的实际配置。

脚本优先将 `${MACA_PATH}/lib` 放入 `LD_LIBRARY_PATH`，随后是 `/opt/mxdriver/lib`
和已有的库路径；`MACA_PATH` 未设置或为空时默认使用 `/opt/maca/lib`。
选择非默认 SDK 时，运行前导出与构建 fio/mcFile 引擎相同的 `MACA_PATH`，
避免运行时加载其他 SDK 的库，并随结果保存 SDK 路径及版本。

以下命令从仓库根目录执行，设备已恢复内核 `nvme` 绑定且没有 GiDS/daemon 接管：

```bash
export FIO_ROOT="$PWD"
export BASELINE_GPU="${GIDS_GPU:?}"
export BASELINE_OUT="$(mktemp -d /srv/tutti/logs/baseline-XXXXXX)"
bash engines/gids/scripts/run_baseline.sh kernel "${NVME_NS:?}" 4k "$BASELINE_OUT/kernel-4k"
```

128 KiB 使用 `128k` 和新的输出前缀。POSIX/mcFile 模式使用本仓库的
`libmcfile` 引擎及 `maca_io` 参数；先按 BUILD.md 启用已有 mcFile 引擎、
配置对应 SDK/驱动，再将模式换为 `posix` 或 `mcfile`。
对于 RDMA 的内核/mcFile 基线，先确认存在可用的内核 NVMe/RDMA 块设备；
GiDS RDMA 或 SPDK RDMA 成功不证明该内核路径可用。

SPDK 模式需要设置 `SPDK_ROOT`，确认插件兼容性、hugepages、设备节点和控制器归属。
该脚本的 SPDK 分支含示例 host NQN，运行前必须改成目标 ACL 允许的本机身份，
并填写 SPDK transport ID。NQN 中的冒号按 fio filename 规则转义。
插件的 `LD_PRELOAD` 应只应用于 fio。结束后恢复记录的驱动绑定及临时资源。

## 核实 GPU 直达路径

mcFile 调用返回成功或注册 GPU buffer 成功，并不能单独证明没有 CPU fallback。
在当前 SDK 支持的诊断方式下，保存实际环境、direct/fallback 计数或调用跟踪；
报告中写明路径已确认、允许回退或未确认。不能将 MetaX mcFile 的结果标为 NVIDIA
cuFile 的实测结果。

某条路径失败或缺少可用模块时，报告错误和 N/A；不以另一种传输或 fallback 的
数字代替。驱动符号版本不匹配时应先解决环境一致性，编译成功不等于模块可安全运行。

## 验收和留存

`summarize_baseline.py` 检查完整 fio JSON 和逐请求延迟日志，拒绝错误、缺样本、
非固定偏移及无效延迟。手动重新统计示例：

```bash
python3 engines/gids/scripts/summarize_baseline.py --warmup=200 \
  "$BASELINE_OUT/kernel-4k-fio.json" --output="$BASELINE_OUT/rechecked.json"
```

保留原始 JSON、lat/clat/slat 日志、运行命令、软件和设备身份、统计结果及恢复检查。
计数必须与预期一致，不能仅检查 fio `error=0`。完整运行产物独立归档，结果摘要给出
环境、方法、结果摘要和覆盖限制。故障恢复检查见 [RECOVERY.md](RECOVERY.md)。
