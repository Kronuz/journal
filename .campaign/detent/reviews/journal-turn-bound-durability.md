# journal-turn-bound review: durability

Commit: journal:c379267bdb5ec06d3418fbba7cc75f8fda04f7cf
Verdict: APPROVE
Date: 2026-11-17

APPROVE: lands as is, at this commit's code, with no blocking finding. CHANGES: lands once each
blocking finding's fix and its test pass the suite at the fixed commit; with only nits, an APPROVE.
REJECT: the approach is wrong; the quest is re-planned.

This is the fix-round review. The first round gave CHANGES at journal:7990fc66 (F1 owed sealer
spin, F2 forget floor one short).

## Evidence I relied on

- Suite 261117-224244-e38ae9 at c379267: exit 0, including `journal_turn_bound_bound`, `_owed`,
  `_forget`, `_oversize`, `_wave`, `_wave_run` and `_crash` (A1, A2, A3, A5). CI green per the lead.
- Fails-without at c379267, each reverting only the fixed header, failing on assertions:
  70d0b4 (`owed` at 7990fc6's driver: "after the refusal the driver goes idle" and the two
  re-link/re-register idle checks), 24e131 (`forget`: "retired ... 2, 2", the old open-record wait
  not failing `group_closed`, the new incarnation acknowledging it), a97f37 (`bound` at 68271af:
  "main's most records commit in one turn: 2 turns" for payloads 10, 100 and 1000, and main's
  largest single record refused).
- 1d5f49: main's one-turn limit against the branch, passed.

## What I checked myself

Probe job 261117-235207-1dff03 at c379267, exit 0. Scripts:
`.scratch/review-journal-turn-bound-durability/r2/` in the Detent home (`run.sh`, `fit.cc`,
`CMakeLists.txt`); rerun with
`campaign job submit --quest journal-turn-bound -- sh <dir>/run.sh <dir>`.

- `oracle`: 2000 trials over five geometries (blocks of 256, 300, 512, 1024 and 4096 bytes), each
  from a log replayed into a random state (up to 40 turns of up to 40 groups, so the offset is mid
  segment and the carried manifest is full), with candidates of tiny, mixed, multi-block and
  near-segment records, of warm and fresh groups. For every prefix length n, `fitting(first n)`
  equals `min(n, k)` (longest and monotone); `stage(first k)` never throws; `stage(first k + 1)`
  threw `length_error` in all 1666 split trials, and was taken neither fresh nor at the log's
  current offset in any. 426 single near-segment records were refused, each one encode refuses.
- `drive`: 180 runs of six random waves through `TurnDriver` (3 geometries, 50 groups, records up
  to a segment): 64347 operations, 4048 turns, 921 groups refused. The driver never failed; every
  operation completed; every error was `length_error`; per group the acknowledged records are a
  prefix and the refused ones the rest; no turn was written that acknowledged nothing; recovery
  in a new driver equals the acknowledged records exactly.

Answers to the lead's questions:

1. `fitting()` agrees with encode by construction, and the probe found no case against it.
   `fits()` (turn_log.h:342-345) is the same emitter encode's fresh path builds
   (turn_log.h:410-412): offset 0, the current segment, epoch and carried manifest, so block
   headers, manifests, padding, fragment headers and checksums are counted by the same code.
   `fitting` emits what `stage` emits without copies: the same `logical` bytes, turn `turn_ + 1`,
   group and sequence for the manifest, and the end-of-turn tally of `count`; `stage` keeps copies
   only where they fit (turn_log.h:272), so the turn `fitting` sizes always encodes. The returned
   prefix is always one `fits()` accepted, so even a non-monotone case could only shorten a turn,
   never fail one. A prefix that fits fresh is never refused at the current offset, and none was
   taken there in the probe, so `fitting` neither builds a turn encode refuses nor refuses one it
   takes. The logical-bytes bound (turn_log.h:284) only caps the search. The `used <= whole_room()`
   fast path (turn_driver.h:460) rests on `turn_room`, the sealers' bound; `drive` staged every
   such turn without a throw.
2. Yes. `refuse()` removes the record and its group's later ones from `pending_` before sealing,
   so they never reach `stage` or the disk; it settles their operations and fails waits at or
   past the hole (turn_driver.h:486-513); `await_ready` fails later waits through `satisfied()`
   (turn_driver.h:377, 411-414). No bytes are written for a refusal, so the crash model is unchanged
   (A3 suite), and `drive` shows later turns of every other group commit and recover exactly.
3. Yes. F1: `refuse()` sets every refused group's sealer quiet and re-marks it
   (turn_driver.h:507-510), `link()` never marks a refused group owed (turn_driver.h:425), and
   `seal_turn` skips it, so `owed_` drops to zero. A re-registration (`seal()`, unchanged) cannot
   make it owed: waits at or past the hole are already failed and never link, and those below it
   are at or under `last_staged`. `owed` covers all three paths and fails without the fix (70d0b4).
   F2: `forget()` takes `last + 1` (turn_driver.h:148); `forget` shows the old open-record wait
   fails `group_closed` and the reopened incarnation commits at 4 without acknowledging it, and
   fails without the fix (24e131).

## Findings

| ID | Blocking | Kind | Finding | Evidence | Fix |
|---|---|---|---|---|---|
| N1 | no | nit | A split turn pays up to 1 + log2(n) trial encodes of up to a segment each on the loop (turn_log.h:292-295), from 0 to n; at the default 64 MiB segment a split of about 100k records is about 17 full-segment encodes and checksums before the real one. It runs only when pending records pass `turn_room`, and it does not affect durability. Derived from the code, not measured. | turn_log.h:283-296; turn_driver.h:460. | Owner's call: keep it, or start `low` at the prefix whose `record_cost` sum fits `turn_room` (known to fit), which narrows the search to the last block or so. |
| N2 | no | nit | Whether a near-limit record is refused depends on the log's state: a fresh segment's first block carries the current manifest (framing.h:107-110), so a record refused now could fit after a turn with fewer groups. Encode refuses the same record in the same state, where main failed the whole driver, so this is strictly better. `bound` measures the limit only in a new log, where no manifest is carried. | framing.h:107-110; turn_bound_test.cc `main_single`. | Optional: one line in turn-seal.md, "does not encode alone in an empty segment now". |

## Not run

- No measurement of `fitting`'s cost at 64 MiB segments (N1).
- No crash model during a split turn beyond the suite's `crash` case: refusal and `fitting` write
  nothing, so the cut points are those A3 already covers.
- No TSan run: the fix round adds no thread interaction (`sealers_mutex_` is taken as before);
  CI's sanitizers cover the suite.
