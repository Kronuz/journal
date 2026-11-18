> Status: CURRENT (2026-11-18) how a Host hands a group to one Store at a time and takes it back,
> `Host::hold`, `Host::settle`, `TurnDriver::park` and `TurnLog::live` in `include/journal/`.
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
   ends.
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
each queued group whose records settled and parks it (`TurnDriver::park`, `TurnLog::park`):

- the turn log drops the group's sequence entry and its index map, and keeps one compact entry:
  its first live sequence and each live record's position (segment, epoch, offset, kind);
- the driver drops any records the Host's open still kept for it;
- the Host drops the group from its per-loop set of created or recovered groups.

The records stay live for retention: the per-segment live counts do not change, and
`forward_slice` copies a parked group's records out of the oldest segment like any other group's,
updating its parked positions. `TurnLog::sequence` answers for a parked group from its entry, so
`holds()` and `status()` still see it as stored. A parked group stages nothing; `live()` unparks it.

The table entry stays in memory: the table is rewritten whole from memory per slot, so dropping an
entry would drop it from disk.

## Why the old guard existed

Before this, a group whose records the log had handed to a Store could not be recovered again
until the Host reopened (`recover_shared`'s Corruption guard). The log's in-memory index still
knew where every live record was, but nothing could read them back, and nothing stopped a second
Store while the first was live. The hold answers the second; `TurnLog::live` answers the first.

## Cost

`journal_test reopen_heap` releases 10,000 groups, each with its first checkpoint record and one
batch, and reports the host's heap per group before and after `settle()`; `reopen_churn` runs
10,000 close-and-recover cycles over 1,000 groups and reports the host's heap after the first pass
and after the last. Both read a heap counter that leaves out the memory disk's own allocations.
