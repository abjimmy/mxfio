# Build and validate the external GiDS engine

Use a Linux checkout, GCC/G++, make, Python 3, MACA 3.9.0 or later with
cu-bridge, and matching runtime/driver libraries. Rebuild with the required
SDK for the target GPU architecture; upgrading only the runtime is insufficient.
The default `GIDS_RDMA=off` build supports PCIe
without RDMA headers, libraries or a network adapter.

From the repository root:

```bash
export MACA_PATH=/opt/maca
export MACA_CLANG_PATH="$MACA_PATH/mxgpu_llvm/bin"
export MXCC_PATH="$MACA_CLANG_PATH/mxcc"
export CUDA_PATH="$MACA_PATH/tools/cu-bridge"
export CUDA_HOME="$CUDA_PATH"
export CUCC_PATH="$CUDA_PATH"
export PATH="$MACA_PATH/tools/cu-bridge/bin:$PATH"
export LD_LIBRARY_PATH="$MACA_PATH/lib:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset LD_PRELOAD
"$CUCC_PATH/bin/cucc" --version
./configure
make -j8
make -C engines/gids -j8
make -C engines/gids test
ldd engines/gids/build/gids.so
./fio --enghelp="$PWD/engines/gids/build/gids.so"
```

Check that `cucc --version` reports the compiler from the selected SDK.
The wrapper prefers `MXCC_PATH`, then `MACA_CLANG_PATH`, over `MACA_PATH`;
containers may predefine either variable for an older installation.
Use a fresh `BUILD` directory when changing SDKs, such as
`make -C engines/gids BUILD=build-sdk390 -j8`, and inspect/load that directory's
`gids.so`. Changing library paths alone does not rebuild existing objects.

Each command must succeed; `ldd` must show no `not found` entries. The `test`
target runs the checks listed in the Makefile, including the fio scheduler
callbacks, batch scheduling, RDMA completion tracking, wire, AP-plan and PCIe
owner tests and vendor hashes. The owner test covers delayed
reuse after namespace removal, path replacement (including dangling links),
and final release.
`--enghelp` loads the shared object without starting a storage job. The
ordinary `./fio --enghelp` list must not contain a built-in `gids` engine.
The shared object must be rebuilt against the running fio version and its
generated `config-host.h`; do not reuse a binary built for a different fio.

For optional existing libmcfile baselines, configure fio with
`./configure --enable-maca --enable-libmcfile` and the mcFile development
environment documented in the root README. GiDS itself does not require
libmcfile. Standalone dependency validation remains available in
[vendor/README.md](vendor/README.md).

## Optional mlx5 RDMA backend

Use `GIDS_RDMA=mlx5` to build both PCIe and GPU-initiated NVMe-oF/RDMA.
This requires the development headers/libraries for libibverbs, librdmacm
and libmlx5, including `infiniband/mlx5dv.h`. Use packages matching the
installed RDMA stack; an existing OFED installation should use its
accompanying development packages. Compilation does not require a NIC;
RDMA I/O requires an mlx5 device and GPU peer-memory registration support.
Other vendors' NICs are not supported by this GPU queue implementation.

```bash
make -C engines/gids GIDS_RDMA=mlx5 -j8
./fio --enghelp="$PWD/engines/gids/build/gids.so"
ldd engines/gids/build/gids.so
```

`gids_transport` help identifies the compiled backend; RDMA connection
options appear only in the mlx5 build. A PCIe-only build rejects
`gids_transport=rdma` before initializing devices.

Pass the selection on every build invocation, or export `GIDS_RDMA=mlx5`
in the build shell. Omitting it uses `off`. Switching modes in the same
`BUILD` directory rebuilds the objects and relinks the plugin automatically;
do not build two configurations concurrently in the same directory.
Use separate `BUILD` directories to retain both binaries.

To return to PCIe-only and check that RDMA libraries are absent:

```bash
make -C engines/gids GIDS_RDMA=off -j8
./fio --enghelp="$PWD/engines/gids/build/gids.so"
readelf -d engines/gids/build/gids.so
```

The `NEEDED` entries must not contain `libibverbs`, `librdmacm` or `libmlx5`.
The `off`/`mlx5` selection is explicit; it does not auto-detect the installed
NIC or silently switch transports.

## Device checks

Local NVMe modules can be compiled on the host with its running-kernel
headers using `make -C engines/gids/vendor module`. Compilation and the
checks above issue no storage I/O or controller-binding changes.

Device selection, read-only commands and result checks are in
[SMOKE.md](SMOKE.md).
