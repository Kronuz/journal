#!/bin/sh
# Applies one guard's mutant to this checkout: `sh test/turn_seal_mutants.sh NAME`. Fails unless
# the guarded text occurs exactly once, so a stale mutant never passes as killed. A guard becomes
# `if (false) {`; a statement mutant replaces its text with `new`.
#   sequence  TurnDriver drops the sealed record's sequence check (A2)
#   covers    TurnLog drops the checkpoint guard (A7)
#   close-behind      Host::close_group drops its acknowledged-record guard (journal-host-guards A1)
#   run-any-scheduler TurnDriver::run drops its post_call check (journal-host-guards A2)
#   keep-waiters      TurnDriver::forget leaves the group's durable() waiters linked (review F1)
#   reuse-sequence    TurnDriver::forget retires no sequence, so a reopen reuses them (review F1)
set -eu
new='if (false) {'
case "$1" in
	sequence) file=include/journal/turn_driver.h; old='if (record->sequence != staged + 1) {' ;;
	covers) file=include/journal/turn_log.h; old='if (record.kind == TurnRecordKind::Checkpoint && record.covers < last) {' ;;
	close-behind) file=include/journal/host.h; old='if (sequence < std::max(base, turns.sequence(group))) {' ;;
	run-any-scheduler) file=include/journal/turn_driver.h; old='if (!task::current().post_call([](void*) {}, nullptr)) {' ;;
	keep-waiters) file=include/journal/turn_driver.h; old='resume([group](const detail::TurnWaiter& waiter) { return waiter.group == group; }, detail::TurnWaiter::closed);'; new='' ;;
	reuse-sequence) file=include/journal/turn_driver.h; old='return found == retired_.end() ? 0 : found->second;'; new='return 0;' ;;
	*) echo "unknown mutant: $1" >&2; exit 2 ;;
esac
git checkout -- "$file" 2>/dev/null || true
python3 - "$file" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"mutant: the guarded text occurs {text.count(old)} times in {path}")
open(path, "w").write(text.replace(old, new))
PY
echo "mutant $1 applied to $file"
