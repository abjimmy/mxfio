# 独立部署准备

GiDS 是外部 fio ioengine；仅需本仓库、MACA 3.9.0 或更新版本和所选传输的系统依赖。
须使用对应 SDK 为目标 GPU 重新编译，并加载匹配的运行库。
默认构建仅支持 PCIe，不依赖 RDMA 库；需要 RDMA 时按 [BUILD.md](BUILD.md)
使用 `GIDS_RDMA=mlx5` 并安装对应开发库。
Tutti 的必要源码已固定在 `vendor/Tutti`。无需安装 Tutti daemon、Python 包或模型。

下文约定本仓库在 `/srv/tutti/mxFIO`，配置与日志分别放在 `/srv/tutti/config`、
`/srv/tutti/logs`；可以替换为本机工作目录。
容器需能访问 GPU、PCI sysfs 和本地测试所需的 snvme 字符设备；RDMA 另需 mlx5、
peer-memory 和可用的目标端。先在宿主机记录 `mx-smi`、`lspci -Dnn`、`nvme list -v`、
`lsblk -f`，选出空闲 GPU 和可独占的测试控制器，再按 README 构建/加载模块。

若从 Windows 打包源码传到 Linux，在仓库根目录使用以下命令保留 LF，避免
`configure` 报 `/bin/sh^M: bad interpreter`；Linux 原生 checkout 不需要此步骤：

```bash
git -c core.autocrlf=false archive --format=tar.gz --output=source.tar.gz HEAD
```

已有容器中的块设备节点可能在控制器重新绑定后过期，应核对当前 sysfs 的 major/minor。
若在独立目录创建临时块设备节点，basename 仍须为实际的 `nvmeXnY`；PCIe 插件按该名称
查找 `/sys/class/block`，不能给节点任意改名。设备身份以 BDF、序列号及 NSID 核对。

## 单卡环境

在容器内准备目录，将占位符改成宿主机核对过的物理 GPU 编号和 PCI BDF：

```bash
mkdir -p /srv/tutti/config /srv/tutti/logs
cat > /srv/tutti/config/gpu-env.sh <<'SH'
export MACA_VISIBLE_DEVICES='REPLACE_PHYSICAL_GPU_ID'
export CUDA_VISIBLE_DEVICES="$MACA_VISIBLE_DEVICES"
export GPU_BDF='REPLACE_GPU_BDF'
export MACA_EXT_NUMA_MEMORY_POLICY=1
export MACA_PATH=/opt/maca
export MACA_CLANG_PATH="$MACA_PATH/mxgpu_llvm/bin"
export MXCC_PATH="$MACA_CLANG_PATH/mxcc"
export CUDA_PATH="$MACA_PATH/tools/cu-bridge"
export CUDA_HOME="$CUDA_PATH"
export CUCC_PATH="$CUDA_PATH"
export PATH="$MACA_PATH/tools/cu-bridge/bin:$MACA_PATH/bin:$PATH"
export LD_LIBRARY_PATH="$MACA_PATH/lib:$MACA_PATH/lib64:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
SH
source /srv/tutti/config/gpu-env.sh
"$CUCC_PATH/bin/cucc" --version
cat > /srv/tutti/config/gpu-probe.cu <<'CPP'
#include <cuda_runtime.h>
#include <cstdio>
#include <strings.h>
int main(int argc, char** argv) {
    int count = -1;
    if (argc < 2 || cudaGetDeviceCount(&count) != cudaSuccess) return 1;
    std::printf("device_count=%d expected=%d\n", count, argc - 1);
    if (count != argc - 1) return 2;
    for (int i = 0; i < count; ++i) {
        char bdf[32]{};
        if (cudaDeviceGetPCIBusId(bdf, sizeof(bdf), i) != cudaSuccess) return 3;
        std::printf("logical_device_%d=%s\n", i, bdf);
        if (strcasecmp(bdf, argv[i + 1]) != 0) return 4;
    }
    return 0;
}
CPP
cucc -std=c++17 -O2 /srv/tutti/config/gpu-probe.cu \
    -L"$MACA_PATH/lib" -lmcruntime -lruntime_cu -lmxc-runtime64 \
    -o /srv/tutti/config/gpu-probe
set -o pipefail
/srv/tutti/config/gpu-probe "$GPU_BDF" | tee /srv/tutti/logs/gpu-map.log
```

`MACA_PATH` 应指向选定的 MACA 3.9.0 或更新 SDK。确认 `cucc --version` 的
`InstalledDir` 属于该 SDK：容器可能预设旧的 `MACA_CLANG_PATH` 或 `MXCC_PATH`，
仅改 `MACA_PATH` 不足以切换实际编译器。更换 SDK 后使用新的 `BUILD` 目录重编译。

探针必须通过，再使用逻辑 ordinal 0。环境变量必须在 fio/SDK 初始化之前生效；每次
新终端重新 source 并核对设备映射。

## 双卡环境

单卡环境的 SDK 路径和探针就绪后，单独保存双卡映射：

```bash
cat > /srv/tutti/config/gpu-env-tp2.sh <<'SH'
source /srv/tutti/config/gpu-env.sh
export MACA_VISIBLE_DEVICES='REPLACE_GPU_A_ID,REPLACE_GPU_B_ID'
export CUDA_VISIBLE_DEVICES="$MACA_VISIBLE_DEVICES"
export GPU_BDF0='REPLACE_GPU_A_BDF'
export GPU_BDF1='REPLACE_GPU_B_BDF'
SH
source /srv/tutti/config/gpu-env-tp2.sh
set -o pipefail
/srv/tutti/config/gpu-probe "$GPU_BDF0" "$GPU_BDF1" \
    | tee /srv/tutti/logs/gpu-map-tp2.log
```

然后按 [多 GPU / SSD 验收](MULTIGPU_MULTISSD.md) 显式映射每个 job 的 GPU 与 SSD。
AP 原生 I/O 指标与 KV retrieve/store、TTFT、ITL 分开报告；本指南不验收 vLLM E2E。

## CPU / mcFile 对照

GiDS 自身单独构建为 `gids.so`。如还要运行 `scripts/run_baseline.sh` 的 `posix` / `mcfile`
对照，需要按仓库根 README 配置 `--enable-maca --enable-libmcfile`，使 fio 带有已有的
`libmcfile` 对照引擎。脚本使用 `maca_io=posix|mcfile`；兼容旧 `cufile` 模式名作为
`mcfile` 的别名。比较历史结果时，应核对实际使用的引擎实现和配置。
