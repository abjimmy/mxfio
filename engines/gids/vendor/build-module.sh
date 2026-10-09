#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
vendor_root="$PWD/Tutti"
module_root="$vendor_root/csrc/device_manager/nvme/kernel_modules/snvme"
module_out="${1:-$PWD/build/module}"
mkdir -p "$module_out"
module_out=$(cd "$module_out" && pwd)
sed -e "s|@module_root@|$module_root|g" \
    -e "s|@module_ccflags@|-I$vendor_root/csrc/include/uapi -I$vendor_root/csrc/device_manager/nvme/libnvm/include|g" \
    "$module_root/Makefile.in" > "$module_out/Makefile"
make -C "$module_out" TUTTI_P2P_BACKEND=metax -j"${JOBS:-8}"
