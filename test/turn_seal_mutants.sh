#!/bin/sh
# Applies one turn-seal mutant to this checkout: `sh test/turn_seal_mutants.sh NAME`. Fails unless
# the guarded line occurs exactly once, so a stale mutant never passes as killed.
#   sequence  TurnDriver drops the sealed record's sequence check (A2)
#   covers    TurnLog drops the checkpoint guard (A7)
set -eu
case "$1" in
	sequence) file=include/journal/turn_driver.h; old='if (record->sequence != staged + 1) {' ;;
	covers) file=include/journal/turn_log.h; old='if (record.kind == TurnRecordKind::Checkpoint && record.covers < last) {' ;;
	*) echo "unknown mutant: $1" >&2; exit 2 ;;
esac
git checkout -- "$file" 2>/dev/null || true
python3 - "$file" "$old" <<'PY'
import sys
path, old = sys.argv[1], sys.argv[2]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"mutant: the guarded line occurs {text.count(old)} times in {path}")
open(path, "w").write(text.replace(old, "if (false) {"))
PY
echo "mutant $1 applied to $file"
