# GiDS 验证方法与通过标准

先执行 [BUILD.md](BUILD.md) 的编译、单元测试、vendor 校验和插件加载检查。
这些检查不产生存储 I/O。硬件验收需另行准备设备并保存运行记录。

`scheduler_test` 直接调用 fio 的 queue/commit/getevents/event 回调，用可控的
adapter 完成检查部分回收、非阻塞轮询、最小完成数、提交时间戳及错误排空。
覆盖 PCIe QD1/31/32/33/64/128/256、每 job 一个 adapter 和初始化失败的清理。
`pcie_batch_test` 用延迟、乱序完成及独立容量限制模拟设备，验证生产代码使用的
批次调度算法：在途上限、每请求恰好一次完成、深度 65536、提交失败和轮询超时。
模拟测试不验证实际 GPU 的 DMA 可见性、MMIO 或硬件环回绕。
`rdma_batch_test` 检查 SEND 与 NVMe 响应两种完成顺序、响应乱序、16 位 WQE
计数器回绕、重复完成和无效 CID；仅两种完成均到达才允许回收请求。

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

PCIe 单 QP 重构还需分别验收 QD1/32/64/128/256、超过硬件环容量的批次及重复
批次回绕。使用接管前独立读取的参考数据检查每条请求，不能仅以 fio `error=0`
判定正确。再按 [README](README.md#单-gpu-多-qp--多-ssd) 测单 GPU 同盘多 QP
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
