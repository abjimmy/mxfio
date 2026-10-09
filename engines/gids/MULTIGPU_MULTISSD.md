# 2 GPU + 2 SSD 同时只读验收

接续 [README 新机入口](README.md#新机只读验收)。使用两个 fio job、同一个 fio 进程，
每个 job 一张可见 GPU 和一块独立 SSD。两个 job 不设 `stonewall`，同时运行；
`numjobs=2` 复制相同参数不会自动选择不同 GPU/SSD。

本流程不覆盖多 GPU RDMA 或多个 GPU 共用同一控制器。

## 前提与映射

按本仓库的[独立部署准备](NEW_HOST.md#双卡环境)
保存 `/srv/tutti/config/gpu-env-tp2.sh` 并核对双卡探针。
下文沿用其逻辑 ordinal 0/1。两盘须为不同 PCI BDF、可独占、单 namespace、NSID=1；
分别完成 README 中的序列号、BDF、挂载、swap、holders、fuser 检查。

在 Tutti daemon 运行前执行本测试。按 README 加载同版本 snvme 模块；fio 负责临时
接管和恢复两个控制器，不要另外启动第二个程序接管同盘。新机仍需根据实际设备名填写变量：

```bash
set -euo pipefail
cd /srv/tutti/mxFIO
source /srv/tutti/config/gpu-env-tp2.sh
/srv/tutti/config/gpu-probe "$GPU_BDF0" "$GPU_BDF1"
export GIDS_NS0='/dev/REPLACE_NAMESPACE_0'
export GIDS_NS1='/dev/REPLACE_NAMESPACE_1'
export GIDS_BDF0='REPLACE_SSD_0_BDF'
export GIDS_BDF1='REPLACE_SSD_1_BDF'
test "$GIDS_BDF0" != "$GIDS_BDF1"
test "$GIDS_NS0" != "$GIDS_NS1"
export GIDS_BS=4k
export AP_OUT="$(mktemp -d /srv/tutti/logs/ap-2gpu-2ssd-XXXXXX)"
```

单个 job 测量固定 offset=1 GiB 的一块数据，不写盘。每次读取前填充反码哨兵，读取后
在 GPU 上比较全部字。两张卡不共享控制器：**当前 PCIe 实现中，同一控制器的并发 job
必须使用同一 GPU**。这与 Tutti daemon 管理多 rank 共享盘组的部署不能混为一谈。

## 两个 job 同时运行

```bash
./fio --readonly \
    --eta=never --output-format=json --output="$AP_OUT/fio.json" \
    engines/gids/examples/pcie-2gpu-2ssd-read.fio \
    > "$AP_OUT/stdout.log" 2> "$AP_OUT/stderr.log"
python3 engines/gids/scripts/summarize_ap.py --warmup=200 \
    "$AP_OUT/gpu0-ssd0.csv" "$AP_OUT/gpu1-ssd1.csv" --output="$AP_OUT/summary.json"
python3 - <<'PY'
import json, os
from pathlib import Path
root = Path(os.environ['AP_OUT'])
jobs = json.loads((root/'fio.json').read_text())['jobs']
assert len(jobs) == 2
expected = {'gpu0-ssd0', 'gpu1-ssd1'}
assert {j['jobname'] for j in jobs} == expected
for j in jobs:
    assert j['error'] == 0 and j['read']['total_ios'] == 5200
    assert j['write']['io_bytes'] == 0 and j['iodepth_level']['1'] == 100.0
# fio 的 job_start 是 epoch 毫秒，read.runtime 是毫秒；要求运行区间实际重叠。
assert max(j['job_start'] for j in jobs) < min(j['job_start'] + j['read']['runtime'] for j in jobs)
rows = json.loads((root/'summary.json').read_text())
assert len(rows) == 2 and all(r['samples'] == 5000 for r in rows)
for r, bdf in zip(rows, (os.environ['GPU_BDF0'], os.environ['GPU_BDF1'])):
    assert r['metadata']['gpu_pci'].lower() == bdf.lower(), r['metadata']
for key in ('GIDS_BDF0', 'GIDS_BDF1'):
    assert (Path('/sys/bus/pci/devices')/os.environ[key]/'driver').resolve().name == 'nvme'
print('TWO_GPU_TWO_SSD_READ_PASS')
PY
```

须同时满足 fio 返回 0、上述检查通过、两个 CSV 都完整；只看到汇总带宽不算通过。
保存两个 namespace→BDF→序列号的前后记录，确认 CSV 对应实际 job 的 GPU。
随后设置 `GIDS_BS=128k`、创建新的 `AP_OUT`，完整重复本节。两种块大小合计
20,800 条计量请求（另有不计入样本的预读）；每组剔除 200 条 warmup 后保留 5,000 条。
启用 `group_reporting=0` 保留每个 job 的独立状态，不合并成单行。

此结果只覆盖 QD1 固定 LBA 的同时读取及数据一致性，不代表随机写、饱和带宽或掉电恢复。
AP 指标的定义见 [AP_LATENCY.md](AP_LATENCY.md)；性能对比需使用相同块大小、拓扑、
GPU 时钟状态和并发配置，不能直接把双卡并发结果与单卡历史延迟判断为回归。

## 更多卡/盘与 RDMA

更多独立 GPU/SSD 配对：为每个配对新增一个 job，显式指定 `gids_gpu`、`filename` 和
独立 CSV；扩大可见 GPU 列表并核对全部 BDF。一个 GPU 驱动多个不同控制器可增加同 GPU
的 job，但同一控制器不得分给不同 GPU。不要用多个 fio 进程绕过控制器共享约束。

双卡 RDMA 需要另建两个 job，分别指定 `gids_gpu=0/1` 及各自的
`gids_traddr`、`gids_trsvcid`、`gids_subnqn`、`gids_nsid`、本机 `gids_hostnqn`、
逻辑 `filename` 和 CSV；每个目标必须已有正确的 ACL 和 peer-memory/RDMA 通路。
本地双 SSD 通过不能证明双卡 RDMA 通过，现有单卡 RDMA 对比也不覆盖双卡同时运行。
本页不自动修改目标端配置。写入验收需另外选定可覆盖范围并使用
[随机读写流程](RANDOM_LATENCY.md)，不能在已有 Tutti 文件系统上运行裸盘写入。

退出后确认两个 BDF 均恢复 `nvme`、GPU 无残留任务，再启动 Tutti，或仅在没有其他使用者
时卸载本轮加载的模块。若 fio 异常中断未恢复，先按已记录 BDF 处理恢复，不继续跑下一项。
