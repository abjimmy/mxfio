# Read-only GiDS smoke checks

Build fio and the plugin using [BUILD.md](BUILD.md). These checks use QD1
and AP mode to compare every read against a reference retained on the GPU.
They check repeated-read consistency, not correctness against an external
reference, write behavior, high queue depths or throughput.

## Select the GPU and output directory

Run from the repository root on Linux with the same MACA libraries used
at build time. Use an idle GPU and confirm its logical ordinal and PCI BDF
after applying any visibility settings. Set `GPU` and `GPU_BDF` accordingly.
Set `OUT` to a new output directory; all result files must be new.

```bash
: "${GPU:?set the logical GPU ordinal}"
: "${GPU_BDF:?set the confirmed GPU PCI BDF}"
: "${OUT:?set a new absolute output directory}"
export GPU GPU_BDF OUT
export LOOPS=${LOOPS:-256}
: "${MACA_PATH:?set the MACA SDK used to build the plugin}"
export LD_LIBRARY_PATH="$MACA_PATH/lib:/opt/mxdriver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset LD_PRELOAD
mkdir "$OUT"
./fio --enghelp="$PWD/engines/gids/build/gids.so"
```

## PCIe

Set `DEVICE` to a whole NVMe namespace and `NVME_BDF` to its controller.
Record its serial, capacity, NSID and driver before testing. The controller
must have one namespace, with NSID 1, and initially use the `nvme` driver.
GiDS temporarily takes over the entire controller. Confirm it has no users,
mounted filesystems (including other mount namespaces), swap, device-mapper
holders or daemon ownership. The namespace must be large enough for the
test range starting at 1 GiB. No formatting is required.

Build the matching snvme modules against the host's running kernel, then
load them on the host if compatible modules are not already loaded:

```bash
make -C engines/gids module
sudo insmod engines/gids/build/module/snvme-core.ko
sudo insmod engines/gids/build/module/snvme.ko
```

The fio process needs access to the GPU, PCI sysfs, `/dev/snvm_control`,
the snvme controller nodes and the selected block device. In a container,
verify these nodes against current sysfs major/minor values. Preserve the
real `nvmeXnY` basename when creating a temporary block node. Rebinding may
change namespace names or device numbers; resolve `DEVICE` again by the
recorded physical identity before each invocation.

Run once with `BS=4096`, then with `BS=131072`, rechecking `DEVICE` between
runs. Set `LABEL` to a unique result name each time:

```bash
: "${DEVICE:?set the current namespace path}"
: "${NVME_BDF:?set its confirmed controller PCI BDF}"
: "${BS:?set 4096 or 131072}"
: "${LABEL:?set a unique result name}"
export TRANSPORT=pcie
./fio --readonly --name="$LABEL" --thread=1 \
  --ioengine="$PWD/engines/gids/build/gids.so" \
  --gids_transport=pcie --gids_gpu="$GPU" --gids_nsid=1 \
  --filename="$DEVICE" --rw=read --bs="$BS" --size="$BS" \
  --offset=1G --loops="$LOOPS" --iodepth=1 \
  --gids_ap_latency="$OUT/$LABEL.csv" --eta=never \
  --output-format=json --output="$OUT/$LABEL.json"
readlink -f "/sys/bus/pci/devices/$NVME_BDF/driver"
```

Require fio exit 0 and the controller restored to `nvme` after each run.
Check namespace identity and capacity again. Remove only modules and
temporary nodes created for this test, after their users have exited.
If a run fails to restore the controller, preserve the logs and inspect
outstanding I/O before attempting recovery. SIGKILL or a device/driver
failure can prevent normal cleanup.

## NVMe-oF/RDMA

Build the plugin with `make -C engines/gids GIDS_RDMA=mlx5 -j8` as described
in [BUILD.md](BUILD.md); the default build supports PCIe only.

Use an existing test target with a known backing namespace. Confirm its
identity, capacity and permitted read range with the target owner. The
initiator needs an mlx5 NIC, working RDMA routing and a peer-memory driver
that allows the NIC to register GPU memory. The target must accept the
chosen host NQN. This test connects directly from the plugin and does not
require a kernel NVMe-oF connection or local snvme modules.

Set `TRADDR`, `TRSVCID`, `SUBNQN`, `HOSTNQN` and `NSID` from the target
configuration. Use its IPv4 address and numeric port. Run once with
`BS=4096`, then with `BS=131072`, using a new `LABEL` for each run:

```bash
: "${TRADDR:?set the target IPv4 address}"
: "${TRSVCID:?set its RDMA port}"
: "${SUBNQN:?set the subsystem NQN}"
: "${HOSTNQN:?set an allowed host NQN}"
: "${NSID:?set the namespace ID}"
: "${BS:?set 4096 or 131072}"
: "${LABEL:?set a unique result name}"
export TRANSPORT=rdma
./fio --readonly --name="$LABEL" --thread=1 \
  --ioengine="$PWD/engines/gids/build/gids.so" \
  --gids_transport=rdma --gids_gpu="$GPU" --filename=rdma \
  --gids_traddr="$TRADDR" --gids_trsvcid="$TRSVCID" \
  --gids_subnqn="$SUBNQN" --gids_hostnqn="$HOSTNQN" --gids_nsid="$NSID" \
  --rw=read --bs="$BS" --size="$BS" --offset=1G --loops="$LOOPS" \
  --iodepth=1 --gids_ap_latency="$OUT/$LABEL.csv" --eta=never \
  --output-format=json --output="$OUT/$LABEL.json"
```

Require fio exit 0 and validate the result below. Confirm the test process
has exited and the original target configuration and connections remain
intact. No target reconfiguration is needed for this check.

## Check each result

The following check uses Python's standard library and the recorded JSON
and CSV files. It requires every sample, including warmup, to report a
successful completion and full payload match. It also checks the GPU BDF,
fixed offset, timestamp ordering and clock drift. Set `LABEL`, `BS` and
`TRANSPORT=pcie` or `rdma` to match the run.

```bash
export LABEL BS TRANSPORT
python3 - <<'PY'
import csv, io, json, math, os
from pathlib import Path

prefix = Path(os.environ['OUT']) / os.environ['LABEL']
count, block = int(os.environ['LOOPS']), int(os.environ['BS'])
transport = os.environ['TRANSPORT']
job, = json.loads(Path(str(prefix) + '.json').read_text())['jobs']
assert job['error'] == 0 and job['read']['total_ios'] == count
assert job['read']['io_bytes'] == count * block
assert job['write']['total_ios'] == job['write']['io_bytes'] == 0
metadata, lines = {}, []
for line in Path(str(prefix) + '.csv').read_text().splitlines():
    if line.startswith('# '):
        key, value = line[2:].split('=', 1)
        metadata[key] = value
    else:
        lines.append(line)
assert 'end_calibration_error' not in metadata
assert metadata['gpu_pci'] == os.environ['GPU_BDF']
assert metadata['transport'] == transport
before, after = (float(metadata['ticks_per_us_' + key])
                 for key in ('before', 'after'))
assert all(math.isfinite(value) and value > 0 for value in (before, after))
assert abs(after / before - 1) <= 0.02
rows = list(csv.DictReader(io.StringIO('\n'.join(lines))))
assert len(rows) == count
for index, row in enumerate(rows):
    row = {key: int(value) for key, value in row.items()}
    assert row['sample'] == index and row['bytes'] == block
    assert row['offset'] == 1073741824
    assert row['status'] == row['mismatches'] == 0
    names = ['sq_begin', 'doorbell', 'cq_seen']
    if transport == 'pcie':
        names += ['cqe_observed', 'slot_reclaimed']
    else:
        assert row['cqe_observed'] == row['slot_reclaimed'] == 0
    stamps = [row[name] for name in names + ['first_read', 'all_read']]
    assert all(value > 0 for value in stamps) and stamps == sorted(stamps)
print(f'PASS: {count} verified {block}-byte reads on {transport}')
PY
```

Retain the source revision, binary hashes, SDK/driver versions, device
identities, fio JSON, CSV and stderr. Successful smoke checks establish
the stated read-correctness coverage; fio timing includes work outside
the GPU AP timestamp interval.
