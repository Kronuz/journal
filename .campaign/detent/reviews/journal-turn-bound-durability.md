# journal-turn-bound review: durability

Commit: journal:7990fc66995bf63a726cb0a6dc8daa60becbd59c
Verdict: CHANGES
Date: 2026-11-17

APPROVE: lands as is, at this commit's code, with no blocking finding. CHANGES: lands once each
blocking finding's fix and its test pass the suite at the fixed commit; with only nits, an APPROVE.
REJECT: the approach is wrong; the quest is re-planned.

## Evidence I relied on

- Suite 261117-204541-5581db at 7990fc6: exit 0, 53/53, including `journal_turn_bound_wave`,
  `_wave_run`, `_oversize`, `_crash` and the `journal_turn_seal_*` crash suites (A1, A2, A3, A5).
- Fails-without 261117-200402-8a9d1f: `wave` fails with "turn exceeds a segment" at main's driver (A1).
- Consensus 261117-200406-4608c2: 64 at once, all committed, none fenced (A4, read by the lead).

## What I checked myself

Probe job 261117-205342-a75577 at 7990fc6 (exit 1: three probes fail as the findings say).
Scripts: `.scratch/review-journal-turn-bound-durability/` in the Detent home (`run.sh`, `probe.cc`,
`CMakeLists.txt`); rerun with
`campaign job submit --quest journal-turn-bound -- sh <dir>/run.sh <dir>`.

Answers to the lead's questions:

1. Wave across turns: durable and ordered. `start()` takes a FIFO prefix
   (turn_driver.h:455-465), one turn is in flight at a time, and each turn's write and
   `sync_data` finish before the next is staged, so recovery sees acknowledged turns plus the
   in-flight turn whole or absent, never a gap or reorder. `crash` truly cuts between turns:
   `split` is set after each acknowledged commit of a wave with records left
   (turn_bound_test.cc:277), so a counted cut falls in turn 2 or later of that wave, and
   `between > 0` is asserted and passed in 5581db.
2. Failing the later records is required. `TurnLog::stage` refuses an out-of-sequence record for
   the whole turn (turn_log.h:251-255) and recovery marks a gap as lost (turn_log.h:575-576); the
   one exception, a fresh group's first record being a checkpoint, would make durable a
   checkpoint whose state includes a failed operation, so the doc's rule is the consistent one
   (N3). After the refusal the log holds the group through `hole - 1` and recovery rebuilds
   that, as with a failed turn. The in-memory state is not consistent, though: see F1 and F2.
3. `start()` uses `whole_room()`, the same `turn_room` as `seal_turn`'s `whole`. Both
   boundaries are `cost <= room` (taken, sealed) against `cost > room` (refused); `wave` commits
   a record of exactly the room. The head always fits, so a large record waits only for older
   records and never starves. "A load that fit one turn before still takes one turn" is not
   true for a band above the room (N1).
4. Sealed records are never held back. `seal_turn` budgets every pending record before calling a
   sealer, so pending plus sealed is at most `whole` whenever a sealer runs, and the prefix takes
   it all. No sealer runs while pending exceeds the room, and `next_seal_` keeps the rotation.
   turn_log.h is untouched, and forwarded copies still ride only when `fits()`. Bytes unchanged.
5. `pending_`, `refused_` and the waiter list are touched only on the loop: `refuse()` runs in
   `start()`, `refusal()` in `satisfied()` and `await_ready`/`await_resume` on the waiter's home
   loop, and `forget()` on the loop. `seal_turn` reads `refused_` under `sealers_mutex_` on the
   loop. `unseal()` and `wake()`, the only other-thread entries, do not touch them.

## Findings

| ID | Blocking | Kind | Finding | Evidence | Fix |
|---|---|---|---|---|---|
| F1 | yes | defect | A refused group whose sealer is owed leaves `owed_` set for good, so `idle()` stays false while nothing can be written. `run()` then spins forever without suspending (turn_driver.h:312-320: `start()` returns nullptr, `continue`), and so does the blocking entry (blocking.h:98-101). This is the normal sealer pattern: a waiter on the open record (`staged + 1`) links, making the sealer owed, and the group's staged record is refused. `seal_turn` skips the refused group (turn_driver.h:541) before the call that would set `quiet`, and nothing else clears `owed`. | Probe a75577 `owed`: 1000 `commit()` calls, 0 turns written, `idle()` still false. `owed_entry`: `j::run` still spinning after 3 s. | When `refuse()` refuses a group, set its `Sealing` quiet and `mark()` it under `sealers_mutex_`. Keep `link()` and `seal()` from marking a refused group owed. Test in `oversize`: a sealed group with a waiter on `staged + 1` and a refused record; the driver goes idle, and `run()` sleeps instead of spinning. |
| F2 | yes | defect | `forget()` of a refused group retires through `last`, the last refused sequence (turn_driver.h:148), but a writer's open record follows it at `last + 1`. That is the record the unrefused path covers with `log_.sequence + 1` (turn_driver.h:146). A `durable(g, last + 1)` linked after the forget does not fail `group_closed`. It waits, and the reopened incarnation's first record (`retired + 1`, journal.h:597-598) then acknowledges it: a false acknowledgment across incarnations. | Probe a75577 `floor`: retired is 2 for both groups. The plain group's old open-record wait fails `group_closed`. The refused group's wait on 3 stays pending, then resolves without error once the new incarnation writes 3. | `floor = std::max(floor, found->second.last + 1)`. Test in `oversize`: after `forget`, `retired(2)` is at least `last + 1`, and a wait on it fails `group_closed`. |
| N1 | no | gap | `turn_room` is conservative. With 16 KiB segments of 1 KiB blocks, one turn encodes 91 records of 100 bytes (15015 cost units, 109% of the 13661 room), which main's driver wrote in one turn and this branch writes in two. Loads in that band pay one more barrier. Not a durability issue. | Probe a75577 `slack`. | Owner's call. Options: keep it (one bound shared with the sealers, simplest), or take the prefix by `fits()` on the encoded bytes with the sealers budgeted against the same check. |
| N2 | no | nit | The doc says a record is refused "at the turn that would take it". `refuse()` refuses it at the next `start()` wherever it sits in `pending_`, so waits at or above the hole can fail before the group's older deferred records are durable. The behavior is fine; the sentence is not. | turn_driver.h:489-509; turn-seal.md "A record no segment holds". | Say "at the next turn". |
| N3 | no | nit | Reason 1 in the doc is not exact for a group's first record: `TurnLog::stage` admits a fresh group's checkpoint at any sequence (turn_log.h:250). The rule still stands on reasons 2 to 4. | turn_log.h:250-253. | Add "except a new group's first checkpoint, whose state would include the failed operation", or leave it. |
| N4 | no | gap | Once refused, a Journal-backed group cannot close (`Journal::close` needs `sequence == record_sequence_`) or reopen in-process (journal.h:590-591), so `refused_` holds it until the host restarts. This matches a failed turn's outcome, but only for that group. | journal.h:359, 590-591. | None needed. Worth a line in the doc. |
| N5 | no | nit | Staged records that exceed the room on every turn keep sealers from running; the doc already says the fairness wait stretches. | turn_driver.h:521, 542. | None. |

## Not run

- No TSan run of the new paths: no new thread interaction, and CI's sanitizers cover the suite.
- No main-driver run of `slack`; main's one-turn result is inferred from `TurnLog::append` encoding
  those 91 records in one turn.
- No crash-model run with sealers active during a split wave; `crash` stages only.
