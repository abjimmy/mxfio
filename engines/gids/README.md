# MetaX GiDS external fio engine

GiDS provides GPU-initiated local NVMe and NVMe/RDMA I/O as a separately
built `gids.so`. Start with [BUILD.md](BUILD.md) to build fio and the plugin,
run the tests and check path-based dynamic loading.
The normal fio engine list and ABI are unchanged.
Build and run with MACA 3.9.0 or later for the target GPU architecture.

The default build supports PCIe without RDMA dependencies. Use
`GIDS_RDMA=mlx5` to also enable NVMe-oF/RDMA on mlx5 devices; see
[BUILD.md](BUILD.md) for dependencies and build selection.

Additional validation:

- [Vendor compilation and linking](vendor/README.md)

The build and engine-help checks do not submit I/O. Hardware jobs require
an explicitly selected idle GPU and test device/target.

Use [SMOKE.md](SMOKE.md) for PCIe and RDMA read-only verification.

## Queue and batch controls

Each fio job owns one NVMe queue pair and one GPU stream. Use `thread=1`
and one namespace per job. `numjobs` creates independent jobs/QPs;
`iodepth=1..65536` bounds requests in each job. The hardware ring has its
own capacity: the GPU refills a window of at most `ring_entries-1` requests
as completions arrive. Startup logs report both limits.

For throughput, set `iodepth_batch_submit=iodepth`; fio's default of 1
submits only one request per GPU batch. A job reuses buffers after its
whole batch completes and fio consumes every event. Jobs run independently.
The fio `slat`/`clat` measurements include host submission/completion work;
use QD1 AP mode for GPU SQ-to-CQ timing.

`make -C engines/gids test` includes scheduler callback tests and delayed,
out-of-order batch scheduling tests. Device checks start with SMOKE.md.
For higher depths, compare read payloads with an independent reference,
exercise batches larger than the ring, then check multiple jobs and SSDs.
Host simulations do not validate GPU DMA visibility or MMIO.


RDMA also uses one data QP per job, plus an admin connection. Its outstanding
window is bounded by target/NIC capacity and the GPU queue-page table,
currently at most 256 requests. Larger batches refill that same QP. SEND
completion and the matching NVMe response must both arrive before a slot
is released. The host RDMA tracker test covers both completion orders,
counter wraparound and invalid/duplicate completions. Hardware acceptance
must additionally exercise queue wraparound, multiple jobs and QP errors;
PCIe results do not establish RDMA correctness.
