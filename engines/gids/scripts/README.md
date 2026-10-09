# Profiling and baseline tools

The tools generate address plans, analyze AP and baseline measurements, and
prepare NVMe target configuration changes. From the repository root,
`make -C engines/gids test` runs the C++ and Python tests and vendor checks
listed in the Makefile. Use Python 3, G++, make and bash. No storage job is
executed.

The tests cover configfs snapshot guards and restore-link conflicts, malformed/incomplete/non-monotonic
AP records, baseline summaries, and generated fio 3.42 mcFile commands.
The command tests intercept `taskset` and the analyzer, so their fake device
path cannot be submitted to fio. Tests use temporary directories.

The scripts provide their CLI with `--help`: `make_ap_plan.py`,
`summarize_ap.py`, `summarize_baseline.py`, and `nvmet_config.py`.
`run_baseline.sh MODE DEVICE BLOCK_SIZE NEW_OUTPUT_PREFIX` supports `kernel`,
`posix`, `mcfile`, `spdk`, and the legacy `cufile` alias for `mcfile`. Read the script
and set its target parameters before running a real job. The mcFile modes
use this repository's existing libmcfile engine and `maca_io` options;
build prerequisites are in [../BUILD.md](../BUILD.md).

`nvmet_config.py` defaults to describing changes; configfs mutation requires
its explicit execution option and the saved-state preconditions. Restore
accepts matching existing links and rejects conflicting links before changing
attributes or enabling namespaces. Building
OFED target modules with `build-ofed-target.sh` does not load them. The unit
tests validate guards and command construction, not a live target setup.
