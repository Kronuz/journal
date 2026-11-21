#!/usr/bin/env bash
# turn_log_power_vm.sh linux|freebsd SEEDS [REF]: runs journal_turn_log_power_test at SEEDS seeds
# inside a local VM driven by $VM_SH (vm.sh VM start|stop|status|ssh CMD), from this repository's
# REF (HEAD). Waits up to an hour while another run holds the VM; leaves the VM stopped.
set -euo pipefail
v=$1; seeds=$2; ref=${3:-HEAD}
V=${VM_SH:-$HOME/.scratch/vm-document-gate/vm.sh}
repo=$(cd "$(dirname "$0")/.." && pwd)
for _ in $(seq 360); do [ "$("$V" "$v" status)" = stopped ] && break; sleep 10; done
if [ "$("$V" "$v" status)" != stopped ]; then echo "VM $v is held by another run" >&2; exit 75; fi
CPUS=4 MEM=6144 "$V" "$v" start
trap '"$V" "$v" ssh sync || true; "$V" "$v" stop' EXIT
"$V" "$v" ssh 'rm -rf ~/journal-power && mkdir -p ~/journal-power'
git -c tar.umask=022 -C "$repo" archive "$ref" | "$V" "$v" ssh 'tar -xf - -C ~/journal-power'
"$V" "$v" ssh "sh -c 'export PATH=\$PATH:/usr/local/bin; cd ~/journal-power && uname -srm && c++ --version | head -1 &&
	{ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >build.log 2>&1 || { tail -30 build.log; exit 1; }; } &&
	ninja -C build -j4 journal_turn_log_power_test | tail -2 && build/journal_turn_log_power_test $seeds' </dev/null"
