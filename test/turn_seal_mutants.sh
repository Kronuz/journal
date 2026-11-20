#!/bin/sh
# Applies one guard's mutant to this checkout: `sh test/turn_seal_mutants.sh NAME`. Fails unless
# the guarded text occurs exactly once, so a stale mutant never passes as killed. A guard becomes
# `if (false) {`; a statement mutant replaces its text with `new`.
#   sequence  TurnDriver drops the sealed record's sequence check (A2)
#   covers    TurnLog drops the checkpoint guard (A7)
#   close-behind      Host::close_group drops its acknowledged-record guard (journal-host-guards A1)
#   run-any-scheduler TurnDriver::run drops its post_call check (journal-host-guards A2)
#   bare-home TurnDriver::durable links a waiter whose scheduler declares no post_call (review F6)
#   keep-waiters      TurnDriver::forget leaves the group's durable() waiters linked (review F1)
#   reuse-sequence    TurnDriver::forget retires no sequence, so a reopen reuses them (review F1)
#   one-call          seal_turn calls a group once per turn (journal-seal-repeat A1)
#   framing           seal_turn charges a record's payload but not its framing (A1)
#   staged            seal_turn does not advance the group's staged sequence between calls (A1)
#   rotation          seal_turn never moves next_seal_ (A2)
#   unsealed-again    seal_turn calls a group unsealed or replaced during its last call (A3)
#   repeat-throw      a repeat call's throw fails its turn unwritten (A3)
#   deferred-ignored  a repeat call's throw is dropped, never failing the driver (review F1)
set -eu
new='if (false) {'
case "$1" in
	sequence) file=include/journal/turn_driver.h; old='if (record->sequence != staged + 1) {' ;;
	covers) file=include/journal/turn_log.h; old='if (record.kind == TurnRecordKind::Checkpoint && record.covers < last) {' ;;
	close-behind) file=include/journal/host.h; old='if (sequence < std::max(base, turns.sequence(group))) {' ;;
	run-any-scheduler) file=include/journal/turn_driver.h; old='if (!detail::posts_calls(task::current())) {' ;;
	bare-home) file=include/journal/turn_driver.h; old='if (!detail::posts_calls(waiter.home)) {'; new='if (!waiter.home) {' ;;
	keep-waiters) file=include/journal/turn_driver.h; old='resume([group](const detail::TurnWaiter& waiter) { return waiter.group == group; }, detail::TurnWaiter::closed);'; new='' ;;
	reuse-sequence) file=include/journal/turn_driver.h; old='return found == retired_.end() ? 0 : found->second;'; new='return 0;' ;;
	one-call) file=include/journal/turn_driver.h; old='pending_.push_back({std::move(*record), {}, {}});'; new='pending_.push_back({std::move(*record), {}, {}}); break;' ;;
	framing) file=include/journal/turn_driver.h; old='used += turn_log_detail::record_cost(record->payload.size()); first = false;'; new='used += record->payload.size(); first = false;' ;;
	staged) file=include/journal/turn_driver.h; old='if (found != sealers_.end()) { found->second.staged = record->sequence; mark(found->second); }'; new='' ;;
	rotation) file=include/journal/turn_driver.h; old='if (resume_at) { next_seal_ = *resume_at; }'; new='' ;;
	unsealed-again) file=include/journal/turn_driver.h; old='if (found == sealers_.end() || found->second.fn != fn) { break; }'; new='' ;;
	repeat-throw) file=include/journal/turn_driver.h; old='catch (...) { if (!repeat) { throw; } deferred_ = std::current_exception(); return; }'; new='catch (...) { throw; }' ;;
	deferred-ignored) file=include/journal/turn_driver.h; old='catch (...) { if (!repeat) { throw; } deferred_ = std::current_exception(); return; }'; new='catch (...) { if (!repeat) { throw; } return; }' ;;
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
