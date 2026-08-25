 *Copyright (c) 2026 MetaX Integrated Circuits (Shanghai) Co., Ltd. All Rights Reserved.*
# mxfio
fio（Flexible I/O Tester）是一款功能强大且灵活的开源磁盘 I/O 性能测试工具。它可以对块设备（比如磁盘、固态硬盘等）以及文件系统进行多种类型的 I/O 性能测试，在存储系统的性能评估、优化等方面有着广泛应用。

fio支持多种I/O引擎，其中包括libcufile和psync。我们参照libcufile engine的实现在fio中增加了MACA libmcfile engine（可执行文件仍名为fio，仅在version中通过mxfio进行区别），使用libmcfile引擎测试MACA MAS特性。
在使用libmcfile I/O引擎时，通过maca_io指定往显存中读写使用的I/O引擎。

- maca_io=mcfile时，存储storage和显存之间直接使用libmcfile 的API进行读写。
- maca_io=posix时，存储storage先经过pwrite/pread 至内存，然后再通过mcMemcpy API拷贝至显存。

通过灵活使用不同的I/O引擎，可以对比Storage  <->  CPU，Storage <-> GPU, Storage <-- CPU --> GPU 三种不同数据通路下的各种IO的性能。

#### 编译

**mxfio编译安装**

首先checkout mx/dev分支
```
git checkout origin/mx/dev-3.42
```

然后设置环境变量如下：
```
export MACA_PATH=/opt/maca
```

接着执行如下指令：

```bash
../configure --enable-maca --enable-libmcfile
# 执行编译前的配置,自定义一部分功能是否开启并且检测当前平台是否有其余功能的对应支持，结果打印到终端并存储到同一目录下的config-host.h
# 如需启用Huawei NDS file system，将nds_api.h（其中需注释掉enum nds_file_op_err_s定义，因其在cudatocann.h中重新封装）和nds_file.h拷贝至mxFIO目录，并将libndsfs.so拷贝至/opt/maca/lib, 执行../configure --enable-cuda --enable-libcufile --enable-nds

make -j 16

sudo make install
```

**如果需要完整执行预期的所有UT case，请在编译前安装aio以及CUnit**

```bash
sudo apt install libaio1 libaio-dev libcunit1 libcuit1-dev
```



#### 运行

一般情况下我们使用*.fio文件指定mxfio的各项运行参数

```shell
fio example.fio
```


以下配置参数可供参考：
```
[test]
ioengine=libmcfile
bs=1M
size=4G
direct=1
gpu_dev_ids=0
maca_io=mcfile
readwrite=write
numjobs=1
end_fsync=1
group_reporting
```

mxfio支持的其他参数配置，参考HOWTO.rst或者 [Welcome to FIO’s documentation!](https://fio.readthedocs.io/en/latest/)。

mxfio 测试完成后会输出详细的测试结果报告，常见的指标有：

+ **带宽（Throughput）**：表示单位时间内读写的数据量，通常以 MB/s（兆字节每秒）或者 GB/s（吉字节每秒）为单位，它直观体现了存储设备的数据传输速度快慢。
+ **IOPS（Input/Output Operations Per Second）**：即每秒输入输出操作次数，对于一些频繁进行小数据量读写的场景（像数据库中大量的事务操作，每次读写的数据量不大但操作很频繁），IOPS 是个很重要的性能衡量指标。
+ **平均响应时间（Average Latency）**：反映了从发出 I/O 请求到得到响应所花费的平均时间，单位一般是毫秒（ms），较短的平均响应时间意味着存储设备能够快速响应读写请求。

当前移植的mxfio基于开源fio-3.42版本[axboe/fio](https://github.com/axboe/fio/tree/fio-3.42)。
