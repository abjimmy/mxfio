# Build and validate the external GiDS engine

Use a Linux checkout, GCC/G++, make, Python 3, MACA 3.9.0 or later with
cu-bridge, and matching runtime/driver libraries. Rebuild with the required
SDK for the target GPU architecture; upgrading only the runtime is insufficient.

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
callbacks, batch scheduling, AP-plan and PCIe
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

## Device checks

Local NVMe modules can be compiled on the host with its running-kernel
headers using `make -C engines/gids/vendor module`. Compilation and the
checks above issue no storage I/O or controller-binding changes.

Device selection, read-only commands and result checks are in
[SMOKE.md](SMOKE.md).
