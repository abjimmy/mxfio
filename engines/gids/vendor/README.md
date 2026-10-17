# Standalone Tutti vendor build

Build and check the Tutti dependencies used by GiDS. `Tutti/` contains
complete upstream files with the MACA adaptations and shared NVMe queue
fixes recorded in `manifest.json`, together with the file set and normalized
SHA-256 hashes.

The public source base is xPU-IO/Tutti
`c4f19b6fca524e020c1233e817dabb89bb35d3a3`. This is not a full Tutti checkout:
it does not build the daemon, Python package or vLLM integration.

## Build and verify user-space dependencies

Use Linux, GCC/G++, make, ar, Python 3, and MACA 3.9.0 or later with
cu-bridge. Build for the target GPU architecture and use the matching SDK
libraries at run time. Older SDK builds are unsupported: replacing only
the runtime does not fix SQ/CQ visibility. Run from the repository root:

```bash
export MACA_PATH=/opt/maca
export MACA_CLANG_PATH="$MACA_PATH/mxgpu_llvm/bin"
export MXCC_PATH="$MACA_CLANG_PATH/mxcc"
export CUDA_PATH="$MACA_PATH/tools/cu-bridge"
export CUDA_HOME="$CUDA_PATH"
export CUCC_PATH="$CUDA_PATH"
export LD_LIBRARY_PATH="$MACA_PATH/lib:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$CUCC_PATH/bin/cucc" --version
make -C engines/gids/vendor MACA_PATH="$MACA_PATH" -j8
make -C engines/gids/vendor MACA_PATH="$MACA_PATH" test
```

Confirm that the reported compiler `InstalledDir` belongs to the selected
SDK. cucc prefers `MXCC_PATH` and `MACA_CLANG_PATH` over `MACA_PATH`; inherited
container settings can otherwise select an older compiler. When changing
SDKs, use a fresh `BUILD` directory for both commands above.

The build compiles every libnvm host source and the three local-NVMe GPU
translation units (`submit_one`, `device_target`, `nvme_queue_group`). It
links all their objects into `build/vendor_link_check`, so unresolved
dependencies cannot hide in unused archive members. It also compiles the
MACA timer builtin as device code and consumes public runtime headers with
the ordinary host compiler.

`test` checks every source hash, runs the link-check program (no SDK calls),
and checks the timer's nanosecond contract, 64-bit conversion and single
counter evaluation with a deterministic counter. No GPU kernel or storage
command runs in these checks. A successful result proves the dependency
subset compiles and links; it is not device-I/O acceptance.

## Build the kernel modules

On the Linux host with headers matching the running kernel:

```bash
test -d "/lib/modules/$(uname -r)/build"
make -C engines/gids/vendor module
```

Outputs are `build/module/snvme-core.ko` and `build/module/snvme.ko` under
this directory. The target generates Kbuild files outside `Tutti/` and only
compiles; it never loads modules or changes controller bindings. Use paths
without spaces for the source checkout and module output.
