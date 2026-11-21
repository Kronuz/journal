> Status: CURRENT (2026-11-18) how a Host hands a group to one Store at a time and takes it back,
> `Host::hold`, `Host::settle`, `TurnDriver::evict` and `TurnLog::live` in `include/journal/`.
> Consumers: consensus's Driver (a group's Worker owns its Store) and Xapiand's index pool, which
> evicts idle indexes and reopens them in the same process.
> Format: unchanged (`RFTW0001`, `RJGT0001`); everything here is in memory.

# A group's lifetime in a live Host

A shared Store (`Store(Host&, group, ...)`) holds its group from its `create` or `recover` until it
closes. Then the same Host can hand the group to a new Store, with exactly the records that were
durable, and without reopening the Host.

## The rule

1. **One holder.** `create` and `recover` take the group's hold (`Host::hold`) before anything
   else. While another Store holds it, they throw `std::system_error` with `EWOULDBLOCK`, as a held
   owner lock does, and change nothing. Busy is transient: the caller retries after the holder
   ends. After a failed `Store::close` the group is in doubt: that Store is fenced, and `create`
   and `recover` of the id in the same Host throw `std::system_error` with `EIO` until the Host
   reopens.
2. **A hold ends at close or at the Journal's end.** `Store::close` (a checkpoint covering every
   batch, durable) closes the group into the host's table and ends the hold at once. A Store that
   cannot close, fenced or with batches no checkpoint covers, ends its hold when its Journal ends:
   when the Store and every operation that holds the Journal are gone. Escaped readers do not
   extend it; they read immutable artifacts.
3. **Records settle first.** While records a Store staged are still pending or in flight in the
   group's turn driver, the group is busy too, even after its hold ended (a `create` whose first
   record is staged leaves such a record with no operation behind it). Once the turn completes,
   those records are durable and the next Store recovers them; a failed turn fails the driver,
   and the group recovers when the Host reopens.
4. **What a recover reads.** A group the Host's open recovered and no Store has held since reads
   the records the open kept (`TurnDriver::records`). A group closed into the table restarts from
   the table's checkpoint record. A group an earlier Store of this Host held and released reads
   its live records back from the log by position (`TurnLog::live`): its latest checkpoint record
   and every record after it, as a reopened Host would recover them.

## Release: what the Host keeps for a released group

A hold's end queues the group for its loop (`Host::unhold`, any thread, under the holds' mutex).
`Host::settle(loop)`, run on the loop by every `hold()` and callable by the loop's owner, takes
each queued group whose records settled and evicts it (`TurnDriver::evict`, `TurnLog::evict`):

- the turn log drops the group's sequence entry and its index map, and keeps one compact entry:
  its first live sequence and each live record's position (segment, epoch, offset, kind);
- the driver drops any records the Host's open still kept for it, and its refusal of the group:
  refused records never reached the log, and no waiter of the old holder remains, so the next
  holder appends from the last durable record;
- the Host drops the group from its per-loop set of created or recovered groups.

The records stay live for retention: the per-segment live counts do not change, and
`forward_slice` copies an evicted group's records out of the oldest segment like any other group's,
updating its evicted positions. `TurnLog::sequence` answers for an evicted group from its entry, so
`holds()` and `status()` still see it as stored. An evicted group stages nothing; `live()` restores it.

`Host::park(loop)` takes the next step for a group no consumer has open (no hold, no sealer)
whose only live record is a checkpoint: it closes the group into the table, as `Store::close`
would, so the log and memory forget it ([idle-groups](../design/idle-groups.md) D2). A released group with batches after its
checkpoint stays in the log until a holder checkpoints it. It runs on the loop's thread, costs one
table commit, and is safe mid-turn: the turn's groups are skipped (D10 there).

A released group's table entry stays in memory while it is open: `next_artifact` and the serial
range its views scan read it there.

## Closing: what the Host keeps for a closed group

Nothing. `close_group` commits the entry closed, with its checkpoint record, then drops it from
memory; `Host::entry` and `status` read a closed entry back from its slot's shard file, and
`reopen_group` loads it again. `commit` writes a slot's shard as the entries in memory plus the
closed ones only the file holds, so an entry out of memory is never dropped from disk. A commit
that fails restores the entry it changed in memory, a closed one too: the file may already hold
the reopened entry open, and only memory wins over the file, so the next commit of the slot
writes it closed again. A slot whose commit succeeds keeps no closed entry in memory. A failed close is the other way
round: its shard may already say closed while memory restores the entry open, so the entry stays
open, in doubt, and no Store holds the group until the Host reopens and reads the disk
([host-layout](../design/host-layout.md#the-group-table)). The open
loads every shard (the logs' recovery needs each closed group's base) and then drops the closed
entries. Open and lost entries stay in memory.

The turn driver's retired floor goes with it. A closed group's stale writer must not reuse a
sequence its incarnation could have handed out, and a stale `durable()` wait at or below it fails
with `group_closed`. `TurnDriver::forget` used to keep that floor per group forever. A Host's
drivers now ask the table (`TurnDriver::retire_with`, `Host::retired_floor`): a group closed at
sequence `s` retires through `s + 1`, the open record a writer may still hold, and its reopened
incarnation keeps that floor until its own close. `forget` keeps an entry only for a floor past
`s + 1` (a waiter on a later sequence, or refused records), so an ordinary close leaves none. A
bare `TurnDriver` with no host keeps every floor, as before.

## Why the old guard existed

Before this, a group whose records the log had handed to a Store could not be recovered again
until the Host reopened (`recover_shared`'s Corruption guard). The log's in-memory index still
knew where every live record was, but nothing could read them back, and nothing stopped a second
Store while the first was live. The hold answers the second; `TurnLog::live` answers the first.

## Cost

`journal_test reopen_heap` releases 10,000 groups, each with its first checkpoint record and one
batch, and reports the host's heap per group before and after `settle()`; `reopen_churn` runs
10,000 close-and-recover cycles over 1,000 groups and reports the host's heap after the first pass
and after the last; `reopen_distinct` closes 10,000 distinct groups once each and reports the
host's heap after 1,000, 5,000 and 10,000. All three read a heap counter that leaves out the memory
disk's own allocations. Under TSan, whose runtime defines `operator new`, nothing is counted and the
three say they skip their heap checks.
