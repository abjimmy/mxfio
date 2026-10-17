#!/usr/bin/env bash
# Read-only, QD1, fixed-LBA baseline. Run inside the prepared test container.
set -euo pipefail
if (( $# != 4 )); then
    echo "usage: $0 kernel|spdk|posix|mcfile DEVICE_OR_SPDK_TRID 4k|128k NEW_OUTPUT_PREFIX" >&2
    exit 2
fi
mode=$1 target=$2 block=$3 prefix=$4
case "$block" in
    4k) bytes=4096 ;;
    128k) bytes=131072 ;;
    *) echo 'block must be 4k or 128k' >&2; exit 2 ;;
esac
for output in "$prefix-fio.json" "$prefix-summary.json" "$prefix"_*.log; do
    [[ ! -e "$output" ]] || { echo "output already exists: $output" >&2; exit 2; }
done
script_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
fio_root=${FIO_ROOT:-$script_root}
spdk_root=${SPDK_ROOT:-/workspace/spdk-ap-baseline}
gpu=${BASELINE_GPU:-0}  # Logical ordinal; confirm the selected physical PCI BDF.
maca_path=${MACA_PATH:-/opt/maca}
export LD_LIBRARY_PATH="$maca_path/lib:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
extra=()
preload=()
case "$mode" in
    kernel) engine=io_uring ;;
    spdk)
        engine=spdk
        preload=("LD_PRELOAD=$spdk_root/build/fio/spdk_nvme")
        extra=(--mem_size_mb=256 --print_qid_mappings=1
          --hostnqn=nqn.2014-08.org.nvmexpress:uuid:cc072fcf-2efd-4582-84e0-687520dcfb39) ;;
    posix|mcfile|cufile)
        # Use the MetaX libmcfile engine; cufile is a compatibility mode alias.
        engine=libmcfile
        maca_mode=$mode
        [[ $mode != cufile ]] || maca_mode=mcfile
        extra=("--maca_io=$maca_mode" "--gpu_dev_ids=$gpu") ;;
    *) echo 'unknown mode' >&2; exit 2 ;;
esac
# First offset is sequential (percentage_random=0), then repeated identically
# for 5200 requests (5201 avoids advancing on the last request). A 5200-block
# logical size avoids per-block file reopening.
# Verify every logged offset after the run; no payload writes are permitted.
taskset -c 0 env "${preload[@]}" "$fio_root/fio" --readonly \
    --name="${prefix##*/}" --ioengine="$engine" --thread=1 --cpus_allowed=0 \
    --filename="$target" --rw=randread:5201 --percentage_random=0 \
    --rw_sequencer=identical --norandommap=1 --direct=1 --bs="$block" \
    --size="$((5200 * bytes))" --offset=1G --number_ios=5200 --iodepth=1 \
    --clocksource=clock_gettime --write_lat_log="$prefix" --log_avg_msec=0 \
    --log_offset=1 --log_entries=6000 --per_job_logs=0 --lat_percentiles=1 \
    --eta=never --output-format=json --output="$prefix-fio.json" "${extra[@]}"
python3 "$fio_root/engines/gids/scripts/summarize_baseline.py" \
    "$prefix-fio.json" --output="$prefix-summary.json"
