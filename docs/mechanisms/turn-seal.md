> Status: CURRENT (2026-11-17) how `TurnDriver::seal` in `include/journal/turn_driver.h` works,
> repeat calls per turn (journal-seal-repeat) and the one-segment bound on staged records
> (journal-turn-bound) included.
> Consumer: strata `docs/design/standalone-turn-log.md` revision 1 (D3, D4, D5; review R3), at
> strata:8e919415, and the shim it replaces, strata:efe25bcd `include/strata/core/turn_seal.hh`.
> Format: `RFTW0001`, unchanged.

# Sealed records: a group's records per turn

A group that collects work between turns (strata's logged index, with its open record) registers a
sealer instead of staging one record per operation. Each turn asks every sealer for records until
it has none or the turn's room is spent, so the turn writes every group's work, a backlog of
several closed records included, with one `pwrite` and one `File::sync_data`.

## The contract

```cpp
using Sealer = std::function<std::optional<TurnRecord>(std::uint64_t room, bool first)>;
std::uint64_t TurnDriver::seal(std::uint64_t group, Sealer fn, const void* owner = nullptr);
void TurnDriver::unseal(std::uint64_t group, const void* owner) noexcept;  // any thread
void TurnDriver::wake() noexcept;  // any thread
```

- **When it runs.** `start()` (behind `begin()`, `commit()`, `run()` and the blocking entry) calls
  each registered sealer, on the driver's loop, after the turn's pending records and before
  encoding. Its records join the turn after them, so a checkpoint staged before them keeps its
  lower sequence.
- **Repeat calls.** A sealer that returns a record is called again at once, with the room left,
  until it returns nothing or less than `record_cost(0)` is left; then the next group's sealer is
  called. A sealer holding several closed records hands them over one per call, each in sequence. The driver is loop-confined like the rest of `TurnDriver`: `seal`, `forget` and the
  sealers run on the loop's thread; `unseal()` and `wake()` may come from any thread.
- **The loop's scheduler.** `wake()` reaches `run()` through its loop's `post_call`, so `run()`
  throws `std::logic_error` before it holds the driver when that scheduler declares none: no
  wake is ever refused while it serves. A `wake()` from another thread needs that `post_call` to
  be thread-safe, as task's `asio_scheduler` and `blocking_scheduler` are; `inline_scheduler`'s is
  not, so on it only the loop's own thread calls `wake()`.
- **Lock order: the turn, then the group's record mutex.** The turn is the loop's own context; the
  driver holds no lock of its own while a sealer runs, so a sealer takes its record mutex freely.
  The sealer table has its own mutex, taken only around table reads and writes, never across a
  call.
  `wake()` takes only a leaf lock that no sealer call runs under, so a writer may call it while it
  holds its record mutex. Nothing in the driver ever waits for a record mutex holder.
- **What it gets.** `room` is what the turn has left in `turn_log_detail::record_cost` units: every
  record pending or sealed earlier in the turn, its framing included, is charged before the next
  call. `first` is true only until the turn's first record seals, across groups: a repeat call,
  and every call after any group's record, passes false, and then the room is the share's
  (below).
- **What it returns.** Nothing, or one `TurnRecord` of its group whose `record_cost` fits `room` and
  whose sequence is the group's last staged sequence + 1: the last record of the group pending, in
  flight, durable, or sealed by its previous call in this turn, so three calls of one turn return
  `staged + 1`, `+ 2` and `+ 3`. `seal` returns that staged sequence. A record that does not fit
  stays with the sealer, which returns nothing, and seals in a later turn. A sealer that frames its record with a stricter bound,
  as strata's `turn_framed_bytes` is, stays within the room. A sealer
  continues its group and never starts one, so the log's rule that a checkpoint may open a new group
  at any sequence does not apply to sealed records.
- **Refusals.** A record of another group, out of sequence, past its room, or returned by a sealer
  that forgot its group fails the turn before any byte is encoded, exactly as a record the log
  refuses does: every pending operation and every `durable()` waiter fails, so no client is told a
  refused record was written. A sealer that throws fails the turn the same way, and a sealer may not
  call `stage()`.
- **A failing repeat call.** A repeat call that throws, or returns a record refused as above,
  leaves the group's earlier records in the turn, as separate turns would have written them: no
  more sealers are called and the turn is written. The error is parked until that turn completes,
  and `idle()` is false meanwhile. `completed()` then acknowledges the turn's records (their
  operations complete, and a `durable()` waiter they satisfy is answered even though the driver
  fails next), fails the driver with the error, failing every operation and waiter left, and
  throws it; so `commit()` and `run()` throw it too, with no further turn asked for. If the turn's
  write or barrier fails instead, the driver fails with that failure. A throw or refusal on a
  group's first call of the turn fails that turn, as above.
- **One per group, keyed by its owner.** `seal` refuses group 0; registering a group again
  replaces its sealer and owner, as a reopened index does. `unseal(group, owner)` removes it only if
  `owner` registered it, so the old index's destructor leaves the reopened one's sealer in place.
  Like a timer's cancel, `unseal` returns only once that sealer is not running and never will be:
  from another thread it waits for a call in progress of a sealer `owner` registered for the group,
  even one the table no longer holds because the sealer forgot or re-sealed its group, so a
  destroyed index's sealer never runs; its caller must not hold a lock the sealer takes. A record that call returns was
  sealed before `unseal` returned and is staged as the group's. From inside the sealer itself,
  `unseal` returns at once. `forget(group)` removes it whatever its owner. Neither lets a call start
  once it returns, including later in the turn being
  sealed when another sealer forgets the group. The driver marks a sealer running (`running_`)
  from its group's first call of the turn to its last, and checks the table before each repeat
  call: a group unsealed, re-sealed or forgotten during a call is not called again in the turn, and
  an `unseal` from another thread waits for the group's calls, not only the one in progress. A
  `forget` between repeat calls is refused, since the group's earlier record is pending. A sealer that forgets its own group finishes its
  call, because the driver holds its function for the call. `forget` still refuses a group with a
  record pending or in flight, and the sealer stays registered then.
- **Closing drops the open record.** `forget` (a group's close) drops its open record unwritten,
  so it fails every `durable()` waiter of the group with `group_closed`, never as acknowledged.
  It retires the sequences such a waiter could hold, the durable sequence + 1 and every waited
  one (`retired(group)`): a later `durable()` at or below them fails with `group_closed`, `stage()`
  refuses a record there, `seal()` returns at least them, and `Journal` stages a reopened group's
  record after them. A writer that took the open record's sequence before the close and waits
  after it, or after the reopen, is refused, never answered by the reopened group's records.

## The checkpoint guard

A checkpoint record drops every earlier record of its group when its barrier returns. One whose
state covers less than the record before it would drop an acknowledged record (the document-gate
review's F1, reproduced by its probe P1). Every checkpoint record carries `TurnRecord::covers`,
its covered sequence: the group's last record its state includes. `TurnLog::stage` (so `append`,
the driver's turns, Journal, Host and strata standalone alike) refuses a checkpoint whose `covers`
is below the record before it in its group, durable or earlier in the same turn, since that
record would be acknowledged at the barrier that drops it. The guard is unconditional: `covers`
defaults to 0, which covers nothing, so only a group's first record in the log may leave it
unset. `covers` is not stored and not compared.

The callers: `Journal` sets it to its last record when it publishes a checkpoint (its frontier
must equal the covered sequence and no mutation may be pending, so every batch record is in it),
to 0 for a new group's first record, and to the table's base when it restages a closed group's
record. `Host`'s re-partition restages each group's first kept record, which starts the group in
the new log. `Store` stages no checkpoint of its own.

## The budget

`room` is in cost units: a record of `payload` bytes costs `record_cost(payload) = payload + 65`.
Each turn must fit an empty segment, or
`TurnLog::encode` throws `length_error`, so the driver works from a bound on a turn's encoded bytes
that holds whatever the block manifests are:

```
E <= (C + 126 + B/2) * B / (B - 126),   C = sum over records of (payload + 65), end of turn included
```

Each record costs its payload, the 25-byte logical header, a 24-byte fragment header, and a 16-byte
manifest entry in the next block's header. Each block adds at most 126 bytes: its 62-byte fixed
header, up to 24 bytes of padding, and one record split across its start (24 + 16). The first
block's header, which names the groups of the block before it, is at most half a block, because a
larger manifest is not kept. With `k <= E/B + 1` blocks, `E <= C + 126k + B/2` gives the bound.
`turn_log_detail::turn_bound` computes it and `turn_room` inverts it; the `bound` case checks both
against `framing::Emitter` with 256-byte to 32 KiB blocks, many groups and full first manifests.
For 32 KiB blocks the overhead past the records is about 16.2 KiB plus 0.4%.

- **The first sealed record** of a turn (`first`) is offered all that an empty segment has left
  after the pending records (`turn_room(segment_bytes)`), so a full-size record always seals once it is
  first (review R3). The largest record it admits has `payload = turn_room(segment_bytes) - 65`.
  This is the framing bound R3 asks the segment rule to name: with 32 KiB blocks a 64 MiB command
  seals first in its turn once `segment_bytes` is at least about 64.3 MiB, and seals behind other
  records within a share once it is at least about 128.5 MiB (`turn_room(segment_bytes / 2) - 65 >=
  command_bytes`).
- **Later sealed records**, a group's repeat calls and the other groups' calls alike, share
  `turn_room(segment_bytes / 2)`, each charged its `record_cost` before the next call. When less
  than `record_cost(0)` is left, no sealer is called again this turn.
- **Nothing is dropped.** A sealer the turn did not call, or one whose first call returned nothing
  under a share, is the first called next turn, with the first record's budget. Its record stays
  open in the consumer meanwhile. A sealer that passes its budget breaks the contract and fails the
  turn.
- **Fairness.** There is no per-group cap: a group may take the whole room, one record per call.
  A group that sealed in the turn has been served, so its repeat call's nothing claims no first
  place; the group after it that was not called, or declined its first call, starts the next turn
  (`next_seal_`). Two groups that each fill a turn alternate, and a record that needs the whole room
  waits at most one turn for each group ahead of it in the rotation, while staged records
  leave it the whole room (staged records are not budgeted, so they can stretch the wait).
- Pending records staged with `stage()` count against the sealers' room before any sealer is
  called; how many of them a turn takes is the next section's rule.

## Staged records: one segment per turn

`start()` takes the oldest pending records whose `record_cost` sum fits
`turn_room(segment_bytes)`, the first record's budget above, and leaves the rest pending in order
for the next turn. `run()`, `commit()` under the blocking entry, and a host calling `begin()` while
`idle()` is false write turn after turn until nothing is pending. A wave of opens, 64 groups each
staging a create checkpoint and a first batch at once, so commits in two or three turns instead of
failing the driver (consensus-wave-open, D114).

- **Sealed records are never deferred.** The sealers spend the same room after the pending
  records, so a turn that seals anything fits whole; while pending records alone exceed the room,
  no sealer is called and the turn takes only pending records.
- **Order and durability.** A turn takes a prefix of the pending records, so every group's records
  reach the log in sequence, and each turn is still one write and one data barrier: an operation
  completes at its own turn's barrier, and a crash between two turns of one wave keeps the first
  turn's records and the second whole or not at all.

**A record no segment holds.** A pending record whose `record_cost` exceeds
`turn_room(segment_bytes)` is refused before sealing, at the turn that would take it. Its operation
fails with `std::length_error("turn record exceeds a segment")`, and so does every later record of
its group: the ones pending now, and any staged until the group is forgotten. A `durable()` wait
on any of those sequences throws the same error, whether it linked before the refusal or after;
waits below it resolve as usual. The group's sealer is no longer called. The driver stays healthy,
and every other group's records commit. `forget()` retires the refused sequences, so a reopened
incarnation follows them.

Why the group's later records fail too, and not only the oversized one:

1. A group's records are one dense sequence. `TurnLog::stage` refuses a record that does not
   follow its group's last sequence, and refuses it for the whole turn: letting record `n + 1`
   through after refusing `n` would fail the driver and every group, the failure this rule removes.
2. Recovery needs that density too: `Journal::recover_shared` reads a group's records as its
   checkpoint plus one record per sequence and reports a gap as corruption. Writing the later
   records around a hole would make a group that recovers worse than one that stops.
3. Renumbering is not available. The `Journal` assigned the sequence when it staged the record,
   and the frontier it reported to the caller names it; the driver cannot move a later record
   down without changing what an acknowledged operation already means.
4. The group is already finished by its own error path. The refused operation fails, so its
   `Store` fences, as it does for a failed turn, and recovery resumes it at its last durable
   record. Failing the later records reports at once what the sequence rule would report at the
   group's next turn, and only for that group.

A record that large means the segment is too small for the group's commands or manifests: the
first record's budget above names the segment size a command needs.

## Asking for a turn

A turn starts when a record is pending, as before; when `wake()` was called since the last turn
began sealing, which a consumer does when an open record takes its first transaction; or when a
sealer owes one: a `durable(group, s)`
waiter is linked with `s` above the group's last staged sequence. That is how a consumer with only
an open record gets it written, under `run()` or the blocking entry, with no staged record and no
new call: its watcher awaits `durable` for the open record.

- `wake()` sets a flag `idle()` reads and, while `run()` serves the driver, posts to its loop to
  resume it. The blocking entry sees it at its next pass; `wake()` posts nothing there, so a wake
  from another thread reaches `journal::run` only while that entry sleeps toward a deadline, when
  the deadline or another post ends the sleep. The
  post reaches the driver only while that `run()` lasts, so the owner may destroy the driver once
  `run()` returns with a wake still queued; that wake's flag stays set for the next turn.
- A sealer that returns nothing when offered the whole room, the turn's first with nothing pending,
  goes quiet: it owes no turn until a
  waiter links on its group again. `run()` then sleeps instead of spinning, and the blocking entry
  reports a waiter whose sealer has nothing as a lost wake (`stalled`).
- `idle()` is false while a sealer owes a turn. `commit()` with nothing pending calls the sealers
  and writes what they return; it writes nothing when an owing sealer had nothing, and throws
  "no records to write" only when nothing was pending, owed, or sealed.
- Registering a sealer for a group a waiter already waits on asks for a turn too.

## Evidence

`test/turn_seal_test.cc`, one CTest entry per case (`journal_turn_seal_<case>`):

| Case | Shows |
|---|---|
| `crash` | Power cut at every I/O operation of runs mixing staged and sealed records, committed inline or asked for by a waiter: recovery holds the acknowledged records and the in-flight turn whole or absent, so a sealed record is durable exactly when its turn's records are |
| `sequence` | Behind, ahead, another group's, a group's start, and a record whose sequence a racing checkpoint took (the review's F1/P1 shape) are refused unwritten; record 2 survives; the right order puts a checkpoint and a sealed record in one turn |
| `budget` | The first sealer is offered the whole segment, a spent share calls no other sealer, the sealer left waiting or declining is called first next turn, a record past its budget is refused, and every record is durable once |
| `full_size` | A record past half a segment seals as its turn's first record |
| `staged_room` | A waiter's record that fits an empty turn but not the room a staged record leaves is declined, then sealed first next turn with the whole room, under the blocking entry and under `run()` |
| `bound` | The bound against the framing |
| `forget_in_flight`, `forget_from_sealer`, `forget_threads` | `forget` with a turn in flight, from inside a sealer, and with a writer thread appending under the consumer's lock (TSan) |
| `supersede` | The review's probe P1: a checkpoint at 3 covering only 1 is refused through `append` and through a driver's turn, and acknowledged record 2 survives recovery; a later checkpoint with no `covers` is refused, a group's first needs none |
| `unseal_owner` | A second registration replaces the first; the first owner's `unseal` leaves it, its own removes it; `seal` returns the last staged sequence |
| `lock_order` | A sealer holding its record mutex calls `wake()`, which asks for exactly one more turn |
| `wake_threads` | Under TSan: a writer thread calls `wake()` under its record mutex at each record's first transaction; `run()` on another thread's loop makes every record durable with no waiter or staged record |
| `wake_refused` | `run()` on a scheduler that declares no `post_call` throws before it holds the driver; the wake it could not carry stays asked for, and `run()` on a loop with `post_call` seals it |
| host_test `sealer_close` | The durability review's F1 probe: a close at the durable sequence with a sealed open record and its waiter fails the wait with `group_closed`; a record at the retired sequence is refused; neither the reopen checkpoint nor a later wait for that sequence acknowledges it; recovery has no trace of it |
| host_test `sealer_close_crash` | Power cut at every I/O operation of rounds that commit the open record or close the group with it in flight and reopen: recovery keeps every acknowledged batch, at most the one in flight, and never a closed one |
| `wake_after_run` | Under ASan: a wake still queued when `run()` returns reaches nothing once the owner destroys the driver on the loop |
| `unseal_threads` | Under TSan: `unseal` from another thread while a turn calls the sealer returns only after the call; its record is durable; across 200 racing rounds no call starts after `unseal` returns; after the sealer forgets its group or re-seals it for another owner, the original owner's `unseal` still waits for the call |
| `trigger` | A waiter makes `run()` seal; a quiet sealer sleeps; a new waiter asks again; the entry reports a lost wake |
| `repeat` | One sealer gives three records in one turn at staged + 1, + 2, + 3; each call's room is the share less every earlier record's `record_cost`; a third record that meets the room exactly seals, one a byte over seals first next turn; recovery holds them in order |
| `repeat_turns` | `first` is true only for the turn's first call until a record seals, across groups; two groups that each fill a turn alternate as its first group |
| `repeat_throw` | A repeat call that throws, or returns a refused record, leaves the earlier records written and their waiter acknowledged, then the turn's completion fails the driver with the error; a first call's throw fails its turn unwritten |
| `repeat_latent` | Review F1: with no waiter and nothing more asked for, `commit()`, `completed()` after `begin()` (the driver busy meanwhile) and `run()` with its stop requested each throw the repeat call's error, and `failed()` is true |
| `repeat_unseal` | A group unsealed or re-sealed during a call is not called again in the turn and its record is durable; `forget` between repeat calls is refused |
| `repeat_threads` | Under TSan: the third call of a turn waits until another thread's `unseal` has dropped the group; the turn holds three records, no fourth call starts, and `unseal` returns only once the calls end; across 100 rounds of random unseals no call starts after `unseal` returns |

`test/turn_bound_test.cc`, one CTest entry per case (`journal_turn_bound_<case>`), covers the
staged records' bound:

| Case | Shows |
|---|---|
| `wave` | 16 seeds of 64 groups' create checkpoint and first batch plus a record of exactly the room between two small ones: every operation completes over three or more turns, each turn under the room and in staged order, and recovery holds every record in order |
| `wave_run` | The same wave under `run()`, with 64 `durable()` waiters: every waiter resumes, the driver stays healthy |
| `oversize` | A record one byte past the room fails its operation with `length_error`, as do its group's later records and the waits on them, linked before or after; the records around it commit; its sealer is not called again; `forget` retires the refused sequences; recovery holds the acknowledged records |
| `crash` | Power cut at every I/O operation of two waves, 6 seeds, cuts between two turns of one wave counted: recovery keeps every acknowledged record and the in-flight turn whole or absent |

`test/turn_seal_mutants.sh` applies each guard's mutant (`one-call`, `framing`, `staged`, `rotation`,
`unsealed-again`, `repeat-throw`, `deferred-ignored` and the earlier ones); the case named beside it fails at each.
