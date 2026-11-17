> Status: CURRENT (2026-11-16) how `TurnDriver::seal` in `include/journal/turn_driver.h` works.
> Consumer: strata `docs/design/standalone-turn-log.md` revision 1 (D3, D4, D5; review R3), at
> strata:8e919415, and the shim it replaces, strata:efe25bcd `include/strata/core/turn_seal.hh`.
> Format: `RFTW0001`, unchanged.

# Sealed records: one record per group per turn

A group that collects work between turns (strata's logged index, with its open record) registers a
sealer instead of staging one record per operation. Each turn asks every sealer for at most one
record, so the turn writes every group's work with one `pwrite` and one `File::sync_data`.

## The contract

```cpp
using Sealer = std::function<std::optional<TurnRecord>(std::uint64_t room, bool first)>;
std::uint64_t TurnDriver::seal(std::uint64_t group, Sealer fn, const void* owner = nullptr);
void TurnDriver::unseal(std::uint64_t group, const void* owner) noexcept;
void TurnDriver::wake() noexcept;  // any thread
```

- **When it runs.** `start()` (behind `begin()`, `commit()`, `run()` and the blocking entry) calls
  each registered sealer once, on the driver's loop, after the turn's pending records and before
  encoding. Its record joins the turn after them, so a checkpoint staged before it keeps its lower
  sequence. The driver is loop-confined like the rest of `TurnDriver`: `seal`, `unseal`, `forget`
  and the sealers all run on the loop's thread; only `wake()` may come from any thread.
- **Lock order: the turn, then the group's record mutex.** The turn is the loop's own context; the
  driver holds no lock of its own while a sealer runs, so a sealer takes its record mutex freely.
  `wake()` takes only a leaf lock that no sealer call runs under, so a writer may call it while it
  holds its record mutex. Nothing in the driver ever waits for a record mutex holder.
- **What it gets.** `room` is what the turn has left in `turn_log_detail::record_cost` units, and
  `first` says no record has sealed in this turn yet (its room is then the whole segment's).
- **What it returns.** Nothing, or one `TurnRecord` of its group whose `record_cost` fits `room` and
  whose sequence is the group's last staged sequence + 1: the last record of the group pending, in
  flight or durable, which `seal` returns. A sealer that frames its record with a stricter bound,
  as strata's `turn_framed_bytes` is, stays within the room. A sealer
  continues its group and never starts one, so the log's rule that a checkpoint may open a new group
  at any sequence does not apply to sealed records.
- **Refusals.** A record of another group, out of sequence, past its room, or returned by a sealer
  that forgot its group fails the turn before any byte is encoded, exactly as a record the log
  refuses does: every pending operation and every `durable()` waiter fails, so no client is told a
  refused record was written. A sealer that throws fails the turn the same way, and a sealer may not
  call `stage()`.
- **One per group, keyed by its owner.** `seal` refuses group 0; registering a group again
  replaces its sealer and owner, as a reopened index does. `unseal(group, owner)` removes it only if
  `owner` registered it, so the old index's destructor leaves the reopened one's sealer in place.
  `forget(group)` removes it whatever its owner. Neither lets a call start once it returns, including later in the turn being
  sealed when another sealer forgets the group. A sealer that forgets its own group finishes its
  call, because the driver holds its function for the call. `forget` still refuses a group with a
  record pending or in flight, and the sealer stays registered then.

## The checkpoint guard

A checkpoint record drops every earlier record of its group when its barrier returns. One whose
state covers less than the record before it would drop an acknowledged record (the document-gate
review's F1, reproduced by its probe P1). `TurnRecord::covers` names a checkpoint's covered
sequence, the group's last record its state includes, and `TurnLog::stage` (so `append`, the
driver's turns, Journal, Host and strata standalone alike) refuses a checkpoint whose `covers` is
below the record before it. `covers` is not stored and not compared; unset, a checkpoint covers
the record before it, as every caller's did before. A group's first checkpoint in the log has no
record before it, so the guard never refuses one.

## The budget

`room` is in cost units: a record of `payload` bytes costs `record_cost(payload) = payload + 65`.
Each turn must fit an empty segment, or
`TurnLog::encode` throws `length_error` and the loop fails, so the driver works from a bound on a
turn's encoded bytes that holds whatever the block manifests are:

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
- **Later sealed records** share `turn_room(segment_bytes / 2)`. When less than `record_cost(0)` is
  left, the remaining sealers are not called this turn.
- **Nothing is dropped.** A sealer the turn did not call, or one that returned nothing under a
  share, is the first called next turn, with the first record's budget. Its record stays open in
  the consumer meanwhile. A sealer that passes its budget breaks the contract and fails the turn.
- Pending records staged with `stage()` are not budgeted: they count against the sealers' room,
  and a turn of pending records alone that does not fit fails as before.

## Asking for a turn

A turn starts when a record is pending, as before; when `wake()` was called since the last turn
began sealing, which a consumer does when an open record takes its first transaction; or when a
sealer owes one: a `durable(group, s)`
waiter is linked with `s` above the group's last staged sequence. That is how a consumer with only
an open record gets it written, under `run()` or the blocking entry, with no staged record and no
new call: its watcher awaits `durable` for the open record.

- `wake()` sets a flag `idle()` reads and, while `run()` serves the driver, posts to its loop to
  resume it; the blocking entry sees it at its next pass, so a wake from another thread reaches
  `journal::run` only while that entry sleeps toward a deadline (a post cuts the sleep short).
- A sealer that returns nothing when offered the whole budget goes quiet: it owes no turn until a
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
| `bound` | The bound against the framing |
| `forget_in_flight`, `forget_from_sealer`, `forget_threads` | `forget` with a turn in flight, from inside a sealer, and with a writer thread appending under the consumer's lock (TSan) |
| `supersede` | The review's probe P1: a checkpoint at 3 covering only 1 is refused through `append` and through a driver's turn, and acknowledged record 2 survives recovery; an unset `covers` changes nothing |
| `unseal_owner` | A second registration replaces the first; the first owner's `unseal` leaves it, its own removes it; `seal` returns the last staged sequence |
| `lock_order` | A sealer holding its record mutex calls `wake()`, which asks for exactly one more turn |
| `wake_threads` | Under TSan: a writer thread calls `wake()` under its record mutex at each record's first transaction; `run()` on another thread's loop makes every record durable with no waiter or staged record |
| `trigger` | A waiter makes `run()` seal; a quiet sealer sleeps; a new waiter asks again; the entry reports a lost wake |
