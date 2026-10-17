# GiDS 验证方法与通过标准

先执行 [BUILD.md](BUILD.md) 的编译、单元测试、vendor 校验和插件加载检查。
这些检查不产生存储 I/O。硬件验收需另行准备设备并保存运行记录。

`scheduler_test` 直接调用 fio 的 queue/commit/getevents/event 回调，用可控的
adapter 完成检查部分回收、非阻塞轮询、最小完成数、提交时间戳及错误排空。
它也检查 `sync`、显式 flush 和 `write_barrier` 配置在创建 adapter 前被拒绝。
`completion_test` 运行 adapter 使用的两阶段完成状态机，检查 kernel 未完成时
没有 D2H、复制未完成时不回收、重复轮询只复制和回收一次、快慢 job 独立、
批次复用，以及两次 event 查询和部分复制失败后的禁止重试。
覆盖 PCIe QD1/31/32/33/64/128/256、每 job 一个 adapter、多文件 geometry/请求路由、参数隔离和初始化失败的清理。
`pcie_batch_test` 用延迟、乱序完成及独立容量限制模拟设备，验证生产代码使用的
批次调度算法：在途上限、每请求恰好一次完成、深度 65536、提交失败和轮询超时。
模拟测试不验证实际 GPU 的 DMA 可见性、MMIO 或硬件环回绕。
`pcie_plan_test` 覆盖多盘稳定分组、QP 负载分配、部分批次、全批集中一盘、
65536 个 slot 和每 QP 容量约束。`worker_pool_test` 用真正并发的主机线程运行
生产 task cursor 算法，验证逐请求恰好一次完成、延迟/失败请求和多批次游标复用。
它不模拟 GPU warp 调度或共享 SQ/CQ 锁；这些必须由新模式的硬件验收覆盖。
`shared_queue_test` 用主机原子操作和 SDK 替身直接编译、执行生产共享队列及
`submit_read_one`/`submit_write_one` 代码，检查等待预算耗尽、迟到 CQE 留下的
回收空洞、CID/槽位保留，以及接近 32 位计数回绕的队列推进。
这些测试不验证 GPU 的 warp 调度、DMA 可见性或 MMIO；修改后的共享队列仍需
重新完成 MACA 构建和硬件验收，不能沿用此前二进制的只读结果。
`rdma_batch_test` 检查 SEND 与 NVMe 响应两种完成顺序、响应乱序、16 位 WQE
计数器回绕、重复完成和无效 CID；仅两种完成均到达才允许回收请求。

Python 回归另检查 AP 活动范围的边界、默认随机序列兼容、plan/iolog 一致，
以及基线脚本使用选定 SDK 的运行库路径；这些测试不运行 fio 或访问设备。

## 硬件验证记录及当前覆盖范围

2026-10-15 的 143 / C500 GPU0 / MACA 3.9.0.18 验证使用真实编译器和运行库，
对比提前排入最终 D2H 的版本与等待 kernel 完成后再排入 D2H 的版本。
两者均完成 PCIe-only 构建、插件加载及 `make test`；新版本的 6 组 adapter
只读校验合计 4590 请求通过，覆盖默认单 QP、batched 多盘和 workers 多盘、
4 KiB/128 KiB、重复与部分批次。QD1 AP 另有 8 个读样本通过。

12 组无追踪性能测试（两种盘组 × 两版本 × 三次重复）及 2 组时间线采集
共 14 组、34 个 fio job 均无 I/O 报错，写入和 trim 字节数为零。
性能配置及统计口径见 [143 完成回传对照](PARALLEL_IO.md#143-完成回传对照)。
性能运行关闭 payload H2D/D2H，其 `error=0` 不能代替上述独立数据校验。
测试源码与对应提交按 LF 归一化核对；截至 `afcaf6c` 的提交说明和文档整理
不改变当时执行的代码。后续共享队列修复仅有新增的主机回归，尚未完成新的
MACA 插件构建、GPU/NVMe 真机执行或长期稳定性验收。

仍未覆盖：当前单数据 QP RDMA 的真机数据通路（包括 InfiniBand 链路兼容性）、
新并行与完成路径的写入回归、系统性的硬件故障注入、长期稳定性、与 GPU 计算
并行，以及所有 GPU/SSD 拓扑。
早期不同实现或不同环境的结果不能替代这些验收。下面给出新增环境或变更后的
验证流程，并不表示其中每一项都已在当前版本完成。

## 硬件验收顺序

1. 按 [NEW_HOST.md](NEW_HOST.md) 记录 GPU ordinal/BDF、SSD BDF/序列号/NSID、
   SDK、驱动、模块及插件身份，检查设备可独占。
2. 执行 [固定块只读校验](AP_LATENCY.md)，从单 GPU 单盘 4 KiB 开始，
   再测 128 KiB 和其他盘；RDMA 单独验证目标身份及 host ACL。
3. 按 [多卡多盘指南](MULTIGPU_MULTISSD.md) 验证明确的 GPU/SSD 映射及实际时间重叠。
   单盘逐次运行不代表多盘并发通过。
4. 对允许覆盖的专用测试范围验证写后读回；需要随机地址时使用
   [计划重放](RANDOM_LATENCY.md)。固定块通过不能外推到随机读写或高队深。
5. 在独占测试设备上验证超时和初始化失败后的资源回收，并检查
   [恢复条件](RECOVERY.md)。

PCIe 单 QP 路径应分别验收 QD1/32/64/128/256、超过硬件环容量的批次及重复
批次回绕。使用接管前独立读取的参考数据检查每条请求，不能仅以 fio `error=0`
判定正确。再按 [并行 I/O](PARALLEL_IO.md) 测单 GPU 同盘多 QP
与多盘并发，核对每 job 的 QP/环容量日志、内容与设备恢复。性能测试关闭载荷
H2D/D2H；记录 GPU 占用，计算干扰需要另外加入计算负载测量。
RDMA 另验收单数据 QP 的 QD1/32/64/128/256，以及超过有效窗口的批次；检查
发送/接收队列反复回绕、多 job、QP 错误和 QD1 AP。PCIe 通过不能代替 RDMA。

## 有界写后读回

下面的 fio CRC32C 示例会覆盖指定区域，包含校验所需的 H2D/D2H 复制。
只有在确认该范围允许覆盖后执行；它验证数据正确性，不测纯 GPU 直达性能。
`TEST_OFFSET` 和 `TEST_SIZE` 必须按实际容量和 LBA 对齐填写。

```bash
./fio --name=pcie-verify --ioengine="$PWD/engines/gids/build/gids.so" \
  --thread=1 --filename="${NVME_NS:?}" --gids_gpu="${GIDS_GPU:?}" \
  --rw=write --verify=crc32c --verify_fatal=1 --bs=4k \
  --offset="${TEST_OFFSET:?}" --size="${TEST_SIZE:?}" \
  --iodepth=8 --iodepth_batch_submit=8 \
  --eta=never --output-format=json --output="${NEW_VERIFY_JSON:?}"
```

RDMA 写验证应使用允许覆盖的专用 namespace，并按 README 填写该目标的连接参数。
`gids_poll_budget=1` 可用于独立超时用例；预期错误不能计为成功的 payload 校验。
超时后检查资源状态，再创建新 job；当前插件不在原上下文内自动重连。

## 留存内容

记录实际源码 revision、构建配置、SDK/驱动、设备身份、命令、完整 fio JSON、
CSV、错误信息和恢复检查。核对请求数、字节数、逐请求状态、数据校验及测试后绑定。
原始数据和哈希单独归档；结果摘要注明只读/写入、块大小、QD、并发方式及未覆盖项。
单元测试、构建成功、只读一致性和性能测量分别报告。
