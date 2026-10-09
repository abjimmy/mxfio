#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
root=$(pwd)
module="$root/vendor/Tutti/csrc/device_manager/nvme/kernel_modules/snvme"
out="$root/build/module"
mkdir -p "$out"
# Keep the vendored source byte-identical. Kbuild generates its objects here.
sed -e "s|@module_root@|$module|g" \
    -e "s|@module_ccflags@|-I$root/vendor/Tutti/csrc/include/uapi -I$root/vendor/Tutti/csrc/device_manager/nvme/libnvm/include|g" \
    "$module/Makefile.in" > "$out/Makefile"
make -C "$out" TUTTI_P2P_BACKEND=metax -j"${JOBS:-8}"
