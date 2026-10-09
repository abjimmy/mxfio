#!/usr/bin/env bash
# Build ONLY NVMe target modules against the installed MLNX/DOCA OFED ABI.
# Does not install, unload or replace any existing modules.
set -euo pipefail
: "${OFED_SOURCE:?set OFED_SOURCE to the installed mlnx-ofed-kernel source directory}"
OFED_HEADERS=${OFED_HEADERS:-/usr/src/ofa_kernel/default}
OUT=${OUT:-/tmp/mxfio-gids-ofed-target}
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}
if [[ -e "$OUT" ]]; then echo "OUT must be a new directory: $OUT" >&2; exit 1; fi
cp -a -- "$OFED_SOURCE" "$OUT"
cd -- "$OUT"
for patchfile in backports/*drivers-nvme-target*; do patch -p1 --forward < "$patchfile"; done
cp -a -- "$OFED_HEADERS/include/." include/
cp -- "$OFED_HEADERS/compat/config.h" compat/config.h
cat > gids-target-config.h <<'EOF'
#include <linux/compat-2.6.h>
#undef CONFIG_NVME_TARGET_PASSTHRU
#undef CONFIG_NVME_TARGET_AUTH
#include <linux/nvme-peer.h>
/* OFED 25.10's nvme-peer.h defines PERR_H instead of its PEER_H guard. */
#define _LINUX_NVME_PEER_H
/* DOCA target hardware offload requires a patched NVMe PCI host driver.
 * This build deliberately supports the normal software NVMe/RDMA target
 * only. Offload resource acquisition fails; it cannot silently activate. */
#define nvme_peer_get_resource(...) NULL
#define nvme_peer_put_resource(...) do { } while (0)
#define nvme_peer_flush_resource(...) (-EOPNOTSUPP)
#define nvme_find_pdev_from_bdev(...) NULL
#define nvme_find_ns_id_from_bdev(...) 0
#define nvme_find_noiob_from_bdev(...) 0
#define nvme_is_bdev_support_write_zero_cmd(...) false
EOF
make -C "$KDIR" M="$OUT/drivers/nvme/target" \
    CONFIG_NVME_TARGET=m CONFIG_NVME_TARGET_RDMA=m CONFIG_NVME_TARGET_TCP=m \
    CONFIG_NVME_TARGET_FC= CONFIG_NVME_TARGET_LOOP= CONFIG_NVME_TARGET_FCLOOP= \
    CONFIG_NVME_TARGET_DUMMY= CONFIG_NVME_HOST_WITHOUT_FC= \
    CONFIG_NVME_TARGET_AUTH= CONFIG_NVME_TARGET_PASSTHRU= \
    NOSTDINC_FLAGS="-I$OUT/include -include $OUT/gids-target-config.h" \
    KBUILD_EXTRA_SYMBOLS="$OFED_HEADERS/Module.symvers" -j"${JOBS:-8}" modules
printf 'Built target modules in %s/drivers/nvme/target; nothing installed.\n' "$OUT"
