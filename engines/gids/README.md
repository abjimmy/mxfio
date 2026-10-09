# MetaX GiDS fio engine

`gids.so` 是 mxFIO 的外部异步 ioengine，支持 GPU 发起本地 NVMe 和 NVMe/RDMA 读写。装载方式参考 [SPDK fio plugin](https://github.com/spdk/spdk/blob/master/app/fio/bdev/README.md)。不依赖 SPDK，也不修改现有 libmcfile/libmcfile_async 引擎。

## 新服务器测试入口

按[独立部署准备](NEW_HOST.md) 配置 N300 / C500 的 SDK 环境并核对 GPU BDF。
GiDS 测试无需模型、torch/vLLM 或 Tutti Python 包，本地测试盘无需格式化。

插件独立构建为 `gids.so`，由 fio 动态加载；未加入 fio 主程序的引擎编译清单，也未修改其 ioengine ABI。必须针对要运行的 fio 版本及其 `config-host.h` 重新构建。

可选 Tutti Runtime 后端的扩展计划见 [TODO](TODO.md)，包括多盘融合提交、worker 配置、层流水线和计算干扰测试；这些能力尚未实现。

建议顺序：编译并通过 `make -C engines/gids test` → 核对空闲 GPU 的可见编号/BDF → 验收所需的 PCIe 或 RDMA 路径。PCIe fio 会接管整个 NVMe 控制器，必须在同盘 daemon 已退出、文件系统卸载、其他使用者释放后运行。不要同时让两套程序接管同盘。

本页与 [AP_LATENCY.md](AP_LATENCY.md) 提供参数化命令；运行 `examples/*.fio` 前，须按实际环境替换地址、NQN、盘名和 GPU。

多卡多盘请接续 [2 GPU + 2 SSD 同时只读验收](MULTIGPU_MULTISSD.md)，使用两个显式映射的
job、独立 CSV 和逐 job 结果检查；包含参数化 fio 配置及扩展约束。
原生 fio/AP 指标与 KV retrieve/store、TTFT、ITL 分开报告；vLLM 集成需另做 E2E 验收。


## 数据通路

* **pcie**：CPU 建立控制器、队列及 PRP 映射，每个 fio job 独占一个 NVMe QP。GPU 批次 kernel 复用 Tutti 的 SQ/CQ 原语，先提交多条命令，再按 CQ 到达顺序和 CID 回收结果；SSD DMA 直接访问 GPU 显存。
* **rdma**：CPU 用 RDMA CM/verbs 完成连接、Fabrics Connect、Identify 和显存 MR 注册。每个 fio job 使用一个数据 RC QP，以及独立的管理连接。GPU 构造多条 NVMe keyed-SGL 命令、发布 mlx5 RQ/SQ、写 NIC UAR 门铃并轮询 CQ；按 CID 匹配响应，等待 SEND 与响应接收均完成后回收请求。目标端通过 RDMA READ/WRITE 直接访问 GPU 显存。每次数据 I/O 没有 CPU `ibv_post_send`/`ibv_poll_cq` 代理，没有 TCP 或主机数据中转回退。

CPU 仍负责 fio 调度、批次 kernel launch 和状态回收。RDMA 队列页映射给 GPU，命令、响应及数据缓冲区位于 GPU 显存；显式关闭 CQE scatter，避免响应内联到主机 CQ。共享 UAR 使用独立 warp 发布门铃，避免同一 warp 的存储合并丢失门铃。相邻主机页在 MACA 上可能映射到不连续 GPU 地址，因此 SQ/RQ/CQ 按页寻址。

使用 **MACA 3.9.0 或更新版本编译并运行**，并对目标 GPU 架构重新构建；只更换运行库不够。GiDS 使用原始 `cudaMalloc` 分配，不包含旧版 SDK 的 SQ/CQ 可见性 workaround。MACA 3.7.0 和 3.8.2 不在支持范围内。

默认 `gids_host_buffers=0`：写数据在 GPU 初始化，读数据留在 GPU，仅请求和完成状态经过 CPU。fio 的 buffer_pattern、refill_buffers 等主机缓冲区设置此时不控制实际写数据。启用 `verify=crc32c` 等 fio 校验或显式设置 `gids_host_buffers=1` 时，会在存储传输之前/之后额外复制 fio 校验数据 H2D/D2H；存储传输仍直接访问显存，但测试时间包含这些复制，不能作为纯 GiDS 性能结果。

## 编译

最小构建与动态加载检查见 [BUILD.md](BUILD.md)。

在 Linux 上使用与 fio 主程序相同的源码/config-host.h。需要 GCC/G++、MACA cu-bridge 以及 Python 3（构建计时头文件、源文件校验与结果统计）。默认 `GIDS_RDMA=off` 只构建 PCIe 后端，不需要 RDMA 开发库或网卡；启用 `GIDS_RDMA=mlx5` 后同时支持 PCIe 和 mlx5 RDMA。测试方法与通过标准见 [TESTING.md](TESTING.md)。

Ubuntu 22.04 的通用构建依赖可安装 `build-essential python3 pkg-config libaio-dev`。仅在启用 mlx5 后端时需要 libibverbs/librdmacm/libmlx5 开发文件，版本须匹配本机已有软件栈。使用发行版 RDMA 时安装 `libibverbs-dev librdmacm-dev`；已有 OFED 时使用其配套开发包，先核对 `infiniband/mlx5dv.h`、`rdma/rdma_cma.h` 和对应库，避免覆盖驱动栈。

```bash
# 新服务器解包目录；每台新主机重新 configure，不复制旧 config-host.h。
cd /srv/tutti/mxFIO
source /srv/tutti/config/gpu-env.sh  # 新机指南保存的空闲 GPU 和 SDK 环境
set -o pipefail
"$CUCC_PATH/bin/cucc" --version  # 核对实际编译器属于 gpu-env.sh 选定的 SDK
./configure
make -j8 2>&1 | tee /srv/tutti/logs/build-fio.log
export GIDS_RDMA=off  # 需要 RDMA 时改为 mlx5；每次构建沿用此选择
make -C engines/gids -j8 2>&1 | tee /srv/tutti/logs/build-gids.log
make -C engines/gids test 2>&1 | tee /srv/tutti/logs/test-gids.log
ldd engines/gids/build/gids.so
```

任一步失败就停止；`ldd` 不得出现 `not found`。产物为 `engines/gids/build/gids.so`。
`./fio --enghelp="$PWD/engines/gids/build/gids.so"` 显示编译时启用的后端；
切换 `GIDS_RDMA` 后重新运行 make 即可，无需 clean。RDMA 运行要求 mlx5 网卡和可用的
GPU peer-memory；其他品牌网卡不支持当前 GPU 直接操作队列的实现。构建本身无需网卡。
下面的 job 文件通过路径动态加载插件，须先按新机参数修改；推荐直接用下一节的参数化验收：

```bash
./fio --readonly engines/gids/examples/pcie-read.fio
./fio --readonly engines/gids/examples/rdma-read.fio
# 命令行也可直接指定绝对路径，余下 job 参数相同
./fio --ioengine=$PWD/engines/gids/build/gids.so --thread=1 ...
```

仍兼容历史 `LD_PRELOAD` 加 `ioengine=gids` 的外部插件装载方式，常规使用推荐上面的路径加载。

`filename` 在 PCIe 模式必须为完整、空闲的 NVMe namespace 块设备；Tutti 的设备信息 ABI 固定查询 nsid=1，因此只接受单 namespace 控制器及 nsid=1，不支持分区或文件系统路径。RDMA 模式是 fio 的逻辑名称，不需要先运行内核 `nvme connect`。

## 新机只读验收

在已暴露 GPU、PCI sysfs、snvme 字符设备的容器内执行 PCIe 测试；先按下一节在宿主机加载模块。RDMA 需可用的 mlx5/peer-memory 及已有 target，不依赖 snvme 或本地 SSD。目标端需确认本机 host NQN 获得对应 namespace 的访问权限。

每个新终端先加载 GPU 环境，然后填写本机参数；BDF 必须来自新机 `mx-smi` / `nvme list -v`，不能沿用旧机编号。以下只定义参数，不执行 I/O：

```bash
cd /srv/tutti/mxFIO
source /srv/tutti/config/gpu-env.sh
/srv/tutti/config/gpu-probe "$GPU_BDF" || exit 1
export GIDS_GPU=0  # 探针已确认单卡可见及其 BDF
export NVME_NS='/dev/REPLACE_NAMESPACE'
export NVME_BDF='REPLACE_NVME_BDF'
export RDMA_ADDR='REPLACE_TARGET_IPV4'
export RDMA_PORT='REPLACE_RDMA_PORT'
export RDMA_SUBNQN='REPLACE_SUBSYSTEM_NQN'
export RDMA_NSID=1  # 按 target 导出的实际 NSID 修改
export RDMA_HOSTNQN='REPLACE_THIS_HOST_NQN'
```

`RDMA_HOSTNQN` 使用发起端宿主机 `/etc/nvme/hostnqn` 的内容；容器未挂载该文件时显式填写。若新主机尚无身份，先由管理员分配持久身份并配置 target ACL，不复制旧测试机的 NQN。

PCIe 测试前在宿主机核对 `nvme id-ns "$NVME_NS" -H`、`lsblk -f "$NVME_NS"`、挂载、swap、holders 及控制器 BDF，确认整个控制器可独占、单 namespace 且 NSID=1。实际设备名后缀不等于 NSID。读测试区间需至少覆盖 offset=1 GiB 加一个测试块，RDMA 同样要求此范围可读且内容在测试期间稳定。

然后执行 [AP_LATENCY.md 的“复现”命令](AP_LATENCY.md#复现)。可以只执行已具备条件的 PCIe 或 RDMA 一段，并只向统计脚本传入实际生成的 CSV。先跑 4 KiB，再用新的输出目录跑 128 KiB。

通过标准：fio job `error=0`；每组 5,200 条请求完成且逐字显存校验通过；`summarize_ap.py --warmup=200` 成功并剩余 5,000 个正式样本；CSV 的 GPU BDF 与选择一致。PCIe 结束后核对 `readlink -f "/sys/bus/pci/devices/$NVME_BDF/driver"` 已恢复 `nvme`。保存源码 revision、SDK/驱动版本、GPU/NVMe 身份、fio JSON、CSV 和统计结果，再与历史相同口径比较。这只证明重复只读一致性；写入和随机范围验收另见 [RANDOM_LATENCY.md](RANDOM_LATENCY.md)。

## 本地 NVMe 模块

在具有当前宿主机内核头文件、MetaX 驱动符号的环境中编译 Tutti snvme 模块：

```bash
bash engines/gids/build-module.sh
sudo insmod engines/gids/build/module/snvme-core.ko
sudo insmod engines/gids/build/module/snvme.ko
```

同一宿主机使用一套兼容的 snvme 模块；若已为 Tutti 加载兼容版本，先确认其使用者状态，避免重复 `insmod`。Tutti daemon 此时不能接管测试盘。`build-module.sh` 在宿主机的 mxFIO 根目录运行；`insmod` 也在宿主机执行。

编译脚本只生成模块，不自动加载或替换驱动。测试进程需要访问 `/dev/snvm_control`、`/dev/ssnvme*`、GPU 和 PCI sysfs。模块加载后才启动的 privileged 容器一般可见这些设备；已运行容器可能需要按宿主机设备的实际 major/minor 创建对应字符节点。不要固定设备号。模块须针对宿主机正在运行的内核编译；用户态插件可在具备 MACA SDK 的容器中编译。

插件在开始时临时将指定控制器从 `nvme` 绑定到 `snvme`，结束及初始化失败路径会释放队列并恢复该 PCI BDF 的 `nvme` 绑定。用同进程共享控制器支持多 job；同一控制器的并发 job 必须使用同一 GPU。仅对可独占的测试盘使用；切换作用于整个 PCI 控制器。进程被 SIGKILL、GPU/驱动失去响应或系统故障时无法依赖用户态析构恢复，需对记录的 BDF 检查当前绑定并人工恢复。

## NVMe/RDMA 环境

需要 mlx5 RC/RoCE（IPv4）及能够为 MetaX 显存注册 MR 的 peer-memory 驱动。主机队列与 NIC UAR 必须能映射到 GPU；不满足时初始化报错。此实现不支持 TCP、iWARP、FC、TLS、DH-HMAC-CHAP 或目标硬件卸载。管理连接使用 KATO=0；要求强制 keepalive 的目标会拒绝连接。

目标机若已有兼容的 `nvmet-rdma`，直接建立 RDMA 监听即可。若系统使用 MLNX/DOCA OFED，而发行版 `nvmet-rdma` 报 RDMA 符号版本不匹配，可使用已安装的 OFED 源码额外构建目标模块：

```bash
OFED_SOURCE=/usr/src/mlnx-ofed-kernel-25.10.OFED.25.10.1.7.1.1 \
  bash engines/gids/scripts/build-ofed-target.sh
```

脚本只生成 `nvmet.ko`、`nvmet-tcp.ko`、`nvmet-rdma.ko`，不会安装或替换 `ib_core`、`rdma_cm`、mlx5、主机 nvme 驱动。禁用需要更换 NVMe PCI 驱动的可选 target offload、passthru 和认证组件。该辅助脚本针对已验证的 OFED 25.10 布局，其他版本需检查头文件、backport 和符号兼容性。

目标模块组切换会中断使用该目标的连接。`scripts/nvmet_config.py snapshot BACKUP.json` 保存 configfs 配置；`clear`/`restore` 默认只打印计划，需要 `--execute` 才执行。先保留原模块和配置，再清理 configfs、卸载目标模块组、加载一致版本的新模块并 restore。若失败，恢复之前记录的目标模块组和 configfs 配置。同名备份禁止覆盖。该辅助工具的已验证范围为 namespace、host ACL 和单 ANA group 配置，使用其他配置前需单独验证恢复流程。

RDMA 监听地址、端口、NQN 和 host ACL 必须按实际目标配置填写。保留配置快照，并明确运行时配置与启动服务配置的区别。

## 参数与限制

延迟测量与对比：见 [AP 内部计时](AP_LATENCY.md) 和 [AP / CPU / SPDK / mcFile 对比方法](COMPARISON.md)。

全 namespace 范围随机读、随机写的 AP 延迟测量：见 [随机读写测试](RANDOM_LATENCY.md)。

| 参数 | 默认值 | 含义 |
|---|---|---|
| gids_transport | pcie | pcie；以 GIDS_RDMA=mlx5 构建时也支持 rdma |
| gids_gpu | 0 | 当前可见 GPU ordinal |
| gids_nsid | 1 | namespace ID |
| gids_traddr | 无 | RDMA 目标 IPv4 |
| gids_trsvcid | 4420 | RDMA 数字端口 |
| gids_subnqn | 无 | RDMA 目标 NQN |
| gids_hostnqn | /etc/nvme/hostnqn 内容 | RDMA host NQN |
| gids_timeout_ms | 30000 | RDMA 建连与管理命令的毫秒超时 |
| gids_poll_budget | 1000000 | GPU 无进展轮询的迭代上限；不是毫秒。批次每次成功提交或处理完成记录都会重置计数；QD1 AP 按单请求计数 |
| gids_host_buffers | 0 | 复制 fio 缓冲区供校验/指定写模式；verify 自动启用 |
| gids_ap_latency | 无 | 新 CSV 路径；启用 QD1 AP 计时及显存校验，默认固定块只读 |
| gids_ap_plan | 无 | 随机读写地址计划；与 `gids_ap_latency`、匹配的 fio `read_iolog` 一起使用，详见 [RANDOM_LATENCY.md](RANDOM_LATENCY.md) |

* 必须 `thread=1`，每 job 一个 namespace。PCIe 每 job 一个 QP、一个 stream；`numjobs` 增加独立 job/QP，`iodepth=1..65536` 控制每个 job 的请求上限，65536 来自批次中使用的 16 位 CID 空间。硬件环大小由控制器/驱动确定，初始化日志给出 `ring_entries` 和 `outstanding_limit=min(iodepth, ring_entries-1)`；超过环容量的批次在 GPU 内分段推进。实际可用 QP 数、显存和映射资源也限制配置规模。
* RDMA 同样每 job 一个数据 QP、`iodepth=1..65536`。物理在途窗口受 target、NIC 和 GPU 队列页表容量限制，当前最多 256 个请求；更大的 fio 批次在同一 QP 内分段推进。初始化日志打印实际 SQ/RQ/CQ 容量和 `outstanding_limit`，不因提高 `iodepth` 而创建更多 QP。
* 每个 job 只允许一个在途批次，整批完成并被 fio 回收后才复用缓冲区。多个 job 可独立推进。这个完成模型不同于逐请求完成后立即从 fio 补入新请求的模型。
* fio `slat` 结束于主机提交函数返回，`clat` 覆盖其后的 GPU 工作及主机观察到完成的时间；两者都不是 GPU 内部 SQ→CQ 时间。设备内部时间使用 QD1 AP 测量。吞吐/延迟包含 fio 调度、kernel launch 和完成回收开销。
* 支持顺序/随机 read、write、randrw 等数据 I/O，拒绝 trim、flush/fsync、ZBD、verify_async。不支持自动重连；GPU 轮询超时/传输错误后停止该上下文，清理资源，再启动新 job。
* RDMA 必须显式给出 `size`，实际 offset、长度按 Identify 返回的容量及 LBA 对齐检查。最大 bs 同时受 MDTS、PRP/SGL、NVMe NLB 字段约束；RDMA 不支持 metadata/protection information。
* 默认 GPU 写缓冲区仅用于测试，不具有应用数据语义。示例只读；需要写入时应明确选择已允许覆盖的测试范围。

## PCIe 吞吐配置

单 job、单 QP、QD128 的吞吐测试可使用以下 fio 设置：

```ini
iodepth=128
iodepth_batch_submit=128
iodepth_batch_complete_min=1
iodepth_batch_complete_max=128
```

`iodepth_batch_submit` 控制每次 GPU 批次的请求数。fio 默认值为 1；在当前整批
回收模型下，这会使单 job 每次只提交一个请求，单独提高 `iodepth` 不会增加设备并发。
测吞吐时将两者设为相同值。`iodepth_batch_complete_min/max` 只控制 fio 一次取回
多少个完成，不能提前取回尚未结束的 GPU 批次。比较时同时记录这些参数。

## 单 GPU 多 QP / 多 SSD

同一 SSD 上设置 `numjobs=4`，得到 4 个独立 job/QP；所有 job 指定相同的 `gids_gpu`。
多 SSD 则使用独立 job 段，每段一个 `filename`。例如下面是同一 GPU、两块 SSD、
每块两个 QP；测试区间要求每盘至少 2 GiB。先按上文核对并独占两块盘，再设置
`GIDS_ENGINE`、`GIDS_GPU`、`GIDS_NS0`、`GIDS_NS1` 环境变量：

```ini
[global]
ioengine=${GIDS_ENGINE}
thread=1
gids_gpu=${GIDS_GPU}
rw=read
bs=128k
offset=1g
size=1g
iodepth=128
iodepth_batch_submit=128
iodepth_batch_complete_min=1
iodepth_batch_complete_max=128
numjobs=2

[ssd0]
filename=${GIDS_NS0}

[ssd1]
filename=${GIDS_NS1}
```

用 `fio --readonly --output-format=json JOB.fio` 运行，逐 job 检查结果和时间重叠。
这表示 4 个 QP、合计最多 512 个 fio 请求；有效硬件在途上限按各 job 的初始化日志
求和。各 job 使用独立 stream/kernel，不是一个 kernel 跨盘融合提交，也不包含
Tutti Runtime 的 worker 池、逐层 event 或 GPU 计算负载，相关扩展见 [TODO](TODO.md)。
一块盘的多个并发 job 必须在同一 fio 进程内，不能用多个独立 fio 进程同时接管它。

调度回归与设备验收方法见 [TESTING.md](TESTING.md)。

## 源文件追踪

`vendor/Tutti` 包含 GiDS 所需的 libnvm、snvme、公共头文件和 local_nvme/io 源码，
公开基线为 xPU-IO/Tutti 的 `c4f19b6fca524e020c1233e817dabb89bb35d3a3`。
Tutti 的 LICENSE 随源码保留。

`vendor/manifest.json` 的 `local_changes` 记录 MACA 主机编译别名和计时单位转换；
`sha256` 覆盖全部 vendored 文件。
`scripts/check_vendor.py` 校验文件集合和内容，`import_provenance` 保留原始导入记录。
依赖构建见 [vendor/README.md](vendor/README.md)，计时语义见 [AP_LATENCY.md](AP_LATENCY.md)。
