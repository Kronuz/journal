> Status: CURRENT (2026-11-22) how a Host deletes a closed group so its id starts empty again,
> `Host::delete_group` in `include/journal/host.h` and `Journal::create_shared` in
> `include/journal/journal.h`.
> Consumers: consensus (a replica removed from membership deletes its log and image and rejoins
> as a fresh learner) and strata's placement repair (a drain or replace that moves a replica off a
> node and later back).
> Format: `RJGT0001` gains state 4, `deleted`; nothing else changes.

# Deleting a group

A node that leaves a group must be able to rejoin it later with nothing of the old group left:
no checkpoint record, no artifacts, and no old record in the turn log that a recovery could read
back as the group's. `Host::delete_group(group)` does that for a closed group, and the next
`create_group(group)` (a `Store::create`) starts the id empty.

## What a group leaves on the host

A closed group lives in three places:

1. **Its table entry**, in its slot's shard file: incarnation, serials `[mark, high)`, and the
   checkpoint record (`base`, its sequence, and the manifest bytes).
2. **Its artifacts**, `g<group>-artifact-<incarnation><serial>` for the serials in `[mark, high)`.
3. **Its records in its loop's turn log.** The log is shared by every group of the loop, so its
   segments are not the group's to remove. The close already made them dead (`TurnLog::forget`):
   retention reclaims them with the segments that hold them. Until then they are on disk, and
   recovery would read them as the group's if nothing superseded them.

## The deleted entry

The delete replaces the entry with a `deleted` one: the same incarnation and `base`, no manifest
bytes. It is a filed entry, as a closed one is: it lives in the shard file only, so a deleted id
costs the live host no memory, and a commit of its slot carries it as it carries closed entries
(`host_detail::filed`). Its status is `GroupStatus::none`.

The `base` is the floor that keeps the old records dead. Each open passes it to the log
(`TurnLogGroups::base`), which ignores every record of the group at or below it, as it does for a
closed group; the floor also retires those sequences in the turn driver (`Host::retired_floor`).
Without it, a reopen while the log still held the old records would recover the old group from
its last checkpoint record: the delete would be undone.

The next `create_group` loads the deleted entry, removes any artifact its serials still name, and
starts incarnation + 1 with the same `base`. `Journal::create_shared` starts the group's records at
`TurnDriver::retired(group)` + 1, past the floor, so the new incarnation's records never share a
sequence with the old one's: recovery keeps them and supersedes the old ones. A group the host
never held has a floor of 0 and starts at 1, as before. The new incarnation's views list only its
own artifacts, since names carry the incarnation.

## The steps

On the group's loop, synchronously:

1. Refuse unless `status(group)` is `closed` (`std::logic_error`), and while a hold of the group
   is live (`std::system_error`, `EWOULDBLOCK`, the code `hold()` gives a busy group). Nothing is
   written.
2. Commit the entry `deleted` (`update`): the shard is written to `.pending`, synced, renamed,
   and the directory synced. This is the delete's point of no return.
3. Remove the artifacts in `[mark, high)` and sync the directory.
4. Commit the entry with `mark = high`, so later opens probe no serial of it.

A power cut before step 2's rename is durable leaves the whole closed group: its record, its
artifacts, its floor. After it, the group is gone, and every open finishes a recorded delete:
`mark_lost` removes the artifacts of every deleted entry whose `mark < high` with the lost ones',
syncs the directory, and commits their marks. The artifacts never go before the record: a cut
between them would leave a closed group whose checkpoint names missing files.

A failure in step 2 leaves the group closed (`update` restores the entry in memory); one in step
3 or 4 leaves it deleted, and the next open or create removes what is left.

## Threads and holds

The delete runs on the group's loop, the thread that owns the loop's range table
([host-ranges](host-ranges.md)): its commits publish the group's range there. Called right
after `close_group` on that thread it needs no handoff and no `settle`: the close ended the hold,
and the group's queued release is harmless (evicting a forgotten group does nothing). Escaped
artifact readers keep their open descriptors; a view of the deleted group lists nothing once the
first commit publishes its range.

## Cost

Two shard commits (each one 4 KiB-bounded write, one file sync, one directory sync), one probe and
unlink per serial in `[mark, high)`, and one more directory sync. On disk the deleted entry takes
45 bytes of its shard, less than the closed entry it replaces; in memory, nothing.

## Tests

`test/host_delete_test.cc`, one CTest entry per case:

- `reopen_empty`: a deleted group reopens empty as its second incarnation, writes past its floor,
  and keeps what it writes across a reopen while the log still holds the old records; an open
  slot mate, a closed slot mate and a group of another slot keep every acknowledged record.
- `stays_deleted`: the group stays deleted across a reopen, a later commit of its slot, and a
  re-partition, and is created again after it.
- `crash`: a power cut after every mutation of the delete, under four crash draws each, leaves
  the whole group or none of it, and both occur; its slot mates stay intact.
- `crash_finish`: from every cut that leaves a recorded delete with artifacts on disk, a power cut
  after every mutation of the open that finishes it leaves the group gone.
- `refused`: an open group (held or released), an unknown one, a deleted one, and a closed one
  with a live hold are refused with their errors and change nothing on disk.
- `loop_thread`: two loops' threads each create, write, close and at once delete their group,
  three times, with no handoff and the range table's owner check on; a delete from a thread that
  does not own the loop aborts on that check.

## Limits

- A deleted entry stays in its shard until the id is created again. Dropping it once retention
  has reclaimed every segment that could hold the old records would need the log to say so.
- An older binary reads state 4 as a damaged entry and loses the shard's groups, as it would any
  shard it cannot decode. The table has no release, so no reader for the old one is kept.
