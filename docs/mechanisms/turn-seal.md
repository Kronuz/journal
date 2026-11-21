> Status: CURRENT (2026-11-20) how `TurnDriver::seal` in `include/journal/turn_driver.h` works,
> repeat calls per turn (journal-seal-repeat), the one-segment bound on staged records
> (journal-turn-bound) and the turn's own buffers (journal-turn-buffers, strata
> `docs/design/rocksdb-parity.md` D3) included.
> Consumer: strata `docs/design/standalone-turn-log.md` revision 1 (D3, D4, D5; review R3), at
> strata:8e919415, and the shim it replaces, strata:efe25bcd `include/strata/core/turn_seal.hh`.
> Format: `RFTW0001`, unchanged.

# Sealed records: a group's records per turn

A group that collects work between turns (strata's logged index, with its open record) registers a
sealer instead of staging one record per operation. Each turn asks every sealer for records until
it has none or the turn's room is spent, so the turn writes every group's work, a backlog of
several closed records included, with one `pwrite` and one data barrier through the IO seam (`TurnLog::flush`).

## The contract

```cpp
using Sealer = std::function<std::optional<TurnRecord>(std::uint64_t room, bool first)>;
using Recycler = std::function<void(std::string&& payload)>;
std::uint64_t TurnDriver::seal(std::uint64_t group, Sealer fn, const void* owner = nullptr, Recycler recycle = {});
void TurnDriver::unseal(std::uint64_t group, const void* owner) noexcept;  // any thread
void TurnDriver::wake() noexcept;  // any thread
```

- **When it runs.** `start()` (behind `begin()`, `commit()`, `run()` and the blocking entry) calls
  each registered sealer, on the driver's loop, after the turn's pending records and before
  encoding. Its records join the turn after them, so a checkpoint staged before them keeps its
  lower sequence.
- **Repeat calls.** A sealer that returns a record is called again at once, with the room left,
  until it returns nothing or less than `record_cost(0)` is left; then the next group's sealer is
  called. A sealer holding several closed records hands them over one per call, each in sequence. The driver is loop-confined like the rest of `TurnDriver`: `seal`, `forget`, `durable()` and
  the sealers run on the loop's thread; `unseal()` and `wake()` may come from any thread.
- **The loop's scheduler.** `wake()` reaches `run()` through its loop's `post_call`, so `run()`
  throws `std::logic_error` before it holds the driver when that scheduler declares none: no
  wake is ever refused while it serves. A `wake()` from another thread needs that `post_call` to
  be thread-safe, as task's `asio_scheduler` and `blocking_scheduler` are; `inline_scheduler`'s is
  not, so on it only the loop's own thread calls `wake()`. A `durable()` wait that suspends needs
  its caller's scheduler, not `run()`'s loop, to declare `post_call`, where its token's callback
  hops to unlink it: on one that declares none, it throws `std::logic_error` before it links,
  whatever its token.
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

`start()` takes the oldest pending records that one turn encodes in an empty segment, and leaves
the rest pending in order for the next turn. That is the limit `TurnLog::encode` has always
applied ("turn exceeds a segment"), so every load that fit one turn before still takes one. While
the pending records' `record_cost` sum fits `turn_room(segment_bytes)`, the first record's budget
above, they all fit and nothing is measured; past it, `TurnLog::fitting` finds the longest prefix
that encodes, by binary search over trial encodes of at most a segment (no forwarded copies, which
ride only where they fit). `turn_room` stays the sealers' budget: they need a bound before a record
exists, and the turn cannot hold them back. `run()`, `commit()` under the blocking entry, and a host calling `begin()` while
`idle()` is false write turn after turn until nothing is pending. A wave of opens, 64 groups each
staging a create checkpoint and a first batch at once, so commits in two or three turns instead of
failing the driver (consensus-wave-open, D114).

- **Sealed records are never deferred.** The sealers spend `turn_room` after the pending records,
  so a turn that seals anything is within the room and fits whole; while pending records alone
  exceed the room, no sealer is called and the turn takes only pending records.
- **Order and durability.** A turn takes a prefix of the pending records, so every group's records
  reach the log in sequence, and each turn is still one write and one data barrier: an operation
  completes at its own turn's barrier, and a crash between two turns of one wave keeps the first
  turn's records and the second whole or not at all.

**A record no segment holds.** A pending record that does not encode alone in an empty segment,
the record `TurnLog::encode` refuses, is refused before sealing, at the next turn, wherever it sits among the
pending records: waits on it can fail before its group's older deferred records are durable. Its operation
fails with `std::length_error("turn record exceeds a segment")`, and so does every later record of
its group: the ones pending now, and any staged until the group is forgotten. A `durable()` wait
on any of those sequences throws the same error, whether it linked before the refusal or after;
waits below it resolve as usual. The group's sealer is no longer called and owes no turn, whatever
waits on the group, so `idle()` turns true and `run()` and the blocking entry sleep. The driver
stays healthy, and every other group's records commit. `forget()` retires the refused sequences and
the open record past them, as it retires the one past a plain group's last record, so a reopened
incarnation follows them and never acknowledges an old wait. A refused group backed by a `Journal`
cannot close or reopen in-process (`Journal::close` needs every assigned sequence durable), so the
refusal holds it until the host restarts, as a failed turn holds every group.

Why the group's later records fail too, and not only the oversized one:

1. A group's records are one dense sequence. `TurnLog::stage` refuses a record that does not
   follow its group's last sequence, and refuses it for the whole turn: letting record `n + 1`
   through after refusing `n` would fail the driver and every group, the failure this rule removes.
   The one exception, a new group's first record as a checkpoint at any sequence, would make durable
   a state that includes the failed operation.
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

## The turn's buffers

A sealed body is copied once, into the turn's bytes (strata `docs/design/rocksdb-parity.md`, D3).

- **The seal moves the body.** A sealer returns its `TurnRecord` by value; the driver moves it into
  its pending records and `TurnLog::stage` reads its payload in place. A consumer encodes the body
  once into the payload it hands over, leaving room at its front for any header of its own, so
  sealing copies nothing. The log's 25-byte record header needs no room there: the emitter takes
  it as a separate piece.
- **Framed in place.** `stage()` frames each record's header (`turn_log_detail::header`, on the
  stack) and payload straight into fragments of the turn's buffer (`framing::Emitter::record(head,
  body, ...)`), with no `logical()` string first. A forward copy reads its original back through
  `framing::RecordReader` straight into the copy's fragments, verifying the original's fragments,
  its record header (kind, group, sequence) and its size (`Live` keeps each live record's payload
  size in its padding).
- **Laid out, not written.** Whether forward copies fit an empty segment (`fits()`, `fitting()`)
  is answered by a measuring emitter, which places fragments and manifests without a byte or a
  read. A turn past the current segment's rest is written once more, at the next segment's start.
- **Reused.** One turn's bytes, notes, manifest and `next` map, and the live index's map nodes,
  pass to the next turn emptied, so a warm turn allocates nothing. The spare map nodes never
  outnumber what their map held at its largest. Measuring lays a turn out in the turn's own
  manifest, and a forward copy's block headers stream through a stack buffer, so the log keeps no
  buffer beside the turn's.

- **Handed back.** A sealer registered with a recycler gets each sealed record's payload back,
  cleared with its capacity kept, so it encodes its next body into the same buffer and a warm
  sealer allocates nothing across turns. The driver keeps a sealed payload in flight and hands it
  back in `completed()`, after the log's completion and the waiters' resumption: the turn is
  durable before the buffer can be reused, and before anything citing it publishes. A recycler
  runs as a sealer call does, on the loop with no driver lock held and `unseal()` waiting for it;
  it cannot stage records, and its throw fails the driver once the turn's records are
  acknowledged, as a repeat sealer call's does. A payload whose sealer was unsealed or replaced
  while its turn was in flight, or whose sealer has no recycler, is freed; a failed turn hands
  nothing back. A staged record's payload is freed once `stage()` has framed it, as before.
  An unsealed sealer's function and recycler, with their captures, stay alive until the turn in
  flight completes: an owner that `unseal()`s must not count on its captures being released then.
- **The driver's own buffers.** Its pending and in-flight vectors, the turn's record list and
  the sealing order keep their capacity across turns, one buffer each, so a warm turn through the
  driver allocates nothing in `begin()` or `completed()`. What they keep is sized by the largest
  turn, never by the groups a host has held: a parked group costs the host no heap.

Measured (`test/turn_buffers_probe.cc`, the same source built against journal:178030b and this
code, faultfs, one `strict` job, 261120-201407-998e89, three alternations): a warm turn of 32
records from 8 groups, 384,800 body bytes, allocated 122.5 times and 1.91 MB in `stage()` and
`completed()` before, about five times its bodies, and took 155-170 us at the fastest of 96;
it now allocates nothing and takes 58-61 us, 2.6 times less.

## Asking for a turn

A turn starts when a record is pending, as before; when `wake()` was called since the last turn
began sealing, which a consumer does when an open record takes its first transaction; or when a
sealer owes one: a `durable(group, s)`
waiter is linked with `s` above the group's last staged sequence. That is how a consumer with only
an open record gets it written, under `run()` or the blocking entry, with no staged record and no
new call: its watcher awaits `durable` for the open record.

- `wake()` sets a flag `idle()` reads and posts to the loop of whoever holds the driver, which
  `TurnDriver::Hold` names. A `Hold` takes its loop as a required parameter and throws
  `std::logic_error` on an empty one before it holds the driver, so no holder is deaf to `wake()`.
  Under `run()` the post resumes it. Under the blocking entry the post is a no-op call on the entry's scheduler: it ends the entry's sleep toward a deadline at once, and
  the entry's next pass reads `idle()` and commits the turn. The post reaches the driver only
  while that `run()` lasts, so the owner may destroy the driver once `run()` returns with a wake
  still queued, and the entry's no-op touches no driver at all; the flag stays set for the next
  turn.
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
| `wake_refused` | `run()` on a scheduler that declares no `post_call` throws before it holds the driver; the wake it could not carry stays asked for, and `run()` on a loop with `post_call` seals it; also a `Hold` built without a loop does not compile, and one on an empty loop throws `std::logic_error` and leaves the driver free |
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
| `wave` | 16 seeds of 64 groups' create checkpoint and first batch plus a record of exactly the room between two small ones: every operation completes over several turns in staged order, and recovery holds every record in order |
| `wave_run` | The same wave under `run()`, with 64 `durable()` waiters: every waiter resumes, the driver stays healthy |
| `bound` | At main's limit, measured through `TurnLog::append` in a new log: one record of the largest payload main encodes commits, one byte more is refused alone; the most records of 10, 100 and 1000 bytes main encodes in one turn commit in one, and one more takes a second turn; each limit lies above the room |
| `oversize` | A record one byte past what a segment holds fails its operation with `length_error`, as do its group's later records and the waits on them, linked before or after; the records around it commit; its sealer is not called again; `forget` retires the refused sequences; recovery holds the acknowledged records |
| `owed` | A sealed group whose waiter on the open record makes its sealer owed when its staged record is refused, through `commit()` and through `begin()` with a waiter linking below the hole while its record is in flight: the driver goes idle, a re-registered sealer leaves it idle, the blocking entry returns and `run()` sleeps |
| `forget` | Forgetting a refused group retires through the open record past the refused ones (3 after a refused 2), as a plain group's through 2: a wait on it fails `group_closed`, and the reopened incarnation's first record does not acknowledge it |
| `crash` | Power cut at every I/O operation of two waves, 6 seeds, cuts between two turns of one wave counted: recovery keeps every acknowledged record and the in-flight turn whole or absent |

`test/turn_buffers_test.cc`, one CTest entry per case (`journal_turn_buffers_<case>`), covers the
turn's buffers:

| Case | Shows |
|---|---|
| `bytes` | Every record kind, 15 size classes, 6 start positions and manifests empty, small, past half a block and past 4096 groups, at 256-byte to 32 KiB blocks: the head-and-body path, the fill path in uneven pieces and the one-piece path write main's bytes (journal:178030b's emitter and `logical()`, kept in the test), offsets and manifests; a measuring emitter lays them out at the same positions and writes nothing |
| `turns` | 400 turns on 16 KiB segments, many rolling into a new segment: each turn's bytes and offset are main's, and the log recovers every record |
| `warm` | Rounds of 32 records from 8 groups (100 B to 40 KiB) and 8 checkpoints: once warm, `stage()` and `completed()` allocate nothing, the bytes are main's, and the payload bytes appended are the bodies' |
| `seal` | Four sealers hand over 128 KiB bodies through `TurnDriver::begin()`: once warm, sealing and staging allocate under a quarter of one body; every record recovers |
| `recycle` | Four sealers encode 128 KiB bodies into buffers their recyclers take back, beside a sealer without one: each buffer returns once per turn, cleared with its capacity, only after its turn is durable, and the next body is encoded into it; once warm, `begin()` (sealing, encoding, staging) and `completed()` allocate nothing; a group unsealed in flight gets nothing back; a recycler's throw fails the driver after its turn is acknowledged; every record recovers |
| `forward` | Four idle groups' checkpoints move forward one per turn: each copy reads back as its original, and once warm a forwarding `stage()` allocates nothing |
| `power` | Power cut at every mutating I/O operation of runs at 512-byte and 4 KiB blocks whose records cross blocks, roll into new segments and move forward: recovery holds exactly the acknowledged records and the in-flight turn whole or not at all |

`test/turn_seal_mutants.sh` applies each guard's mutant (`one-call`, `framing`, `staged`, `rotation`,
`unsealed-again`, `repeat-throw`, `deferred-ignored`, `seal-copy`, `stage-copy`, `forward-string`, `drop-recycled` and the earlier ones); the case named beside it fails at each.
