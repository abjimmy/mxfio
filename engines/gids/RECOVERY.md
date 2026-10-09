# GiDS 测试后的资源恢复

测试开始前记录 GPU BDF、NVMe BDF/序列号/NSID、原驱动绑定、加载模块、设备节点、
挂载及现有连接。设备编号可能在重绑定后变化，恢复时以物理身份为准。

正常退出和初始化失败路径会释放队列并恢复 PCIe 控制器的 `nvme` 绑定。
SIGKILL、GPU/驱动失去响应或系统故障时，不能依赖用户态析构完成恢复。

## 退出后的检查

```bash
readlink -f "/sys/bus/pci/devices/${NVME_BDF:?}/driver"
nvme list -v
lsblk -f
mx-smi
```

确认目标控制器绑定 `nvme`，namespace 的序列号、容量和 NSID 与测试前一致，
GPU 没有残留测试进程。等待 namespace 枚举完成后再访问设备；容器内核对节点
major/minor，不能沿用重绑定前的旧节点。参考 [新机指南](NEW_HOST.md)。

只卸载本次加载且已无使用者的模块，只移除本次创建的临时节点和挂载。
多个 GiDS job、Tutti daemon 或其他测试若共享模块，必须先确认其资源已释放。
RDMA 测试后核对原有目标配置和连接；如修改过 configfs，使用事先保存的快照
按 [README](README.md#nvmerdma-环境) 所述流程恢复。

## 异常退出

停止向故障上下文继续提交，保存 fio 日志、设备状态和内核错误，再判断是否仍有
在途 I/O、队列或 DMA 映射。绑定尚未恢复时保留现场，避免一边使用设备一边卸载模块。

GPU 的 Syncflood、不可用或驱动 probe 错误需要驱动/硬件诊断。不要将 reset 命令
返回成功当成设备恢复，也不要假定按单卡编号执行的 reset 只影响一张卡；
先确认实际复位范围及其他使用者。中断命令不保证撤销已经启动的异步复位。

恢复后重新核对 GPU ordinal/BDF 和 SSD 身份，先做小规模只读校验，
再恢复后续测试。保留故障和恢复记录，避免把未完成或错误运行计入性能统计。
