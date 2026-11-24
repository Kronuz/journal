# One directory per host

Detent async-drivers Q3 (b), D97, D105: every group of a host and every loop's turn log live in
one flat directory under one owner lock, independent of the host's loop count L, so an idle
group holds no descriptor. `include/journal/host.h` builds it; ARCHITECTURE.md, "One directory
per host", summarizes it.

## Names

```
<host>/
  layout                                            RJHL0001: L and the log generation
  groups-<slot:3x>-<node:9x>                        RJGT0001: the group table's entries under one node of its slot's trie
  l<generation:16x>-<loop:4x>-turns-<segment:16x>   a loop's RFTW0002 segment
  g<group:16x>-artifact-<incarnation:16x><serial:16x>   a group's artifact
```

- **The lock** is the directory's own (`OwnerLockMode::directory`), which writes no file. It is
  the lock strata's cold tier takes (`acquire_directory_owner`), never a second one.
- **Group ids are opaque and fixed-width**: a `uint64` the host assigns, printed as 16 hex digits.
  No name derives from an index path, so `twitter` and `twitter/user` never collide, and every
  operation on one group names only its own files.
- **Prefixed views.** A loop's log and a group's artifacts reach the directory through
  `PrefixIO`, which prefixes every name and lists what its owner supplies. `TurnLog` and the
  per-group code inside the views are unchanged: a log still names its segments `turns-<n>`.
- **Artifacts are named by group, incarnation and serial.** An artifact's 16-byte identity is
  its incarnation and serial, big-endian, so its name follows from its descriptor. Serials count
  up per incarnation and are reserved 64 at a time with one table update, made synchronously on
  the loop when a preparation starts a new block.
- **Views on other threads.** A completion backend may run a reclamation's scan primitive on a
  worker; a group's view reads its serial range from a small locked copy the host updates
  whenever the entry changes, never the table itself.

## The group table

One entry per group: incarnation, state (open, closed, lost), the serial range `[mark, high)`,
and, once the group has closed, its last checkpoint record (`base`, its sequence, and the
manifest bytes). Each slot's table is a binary trie of shard files, each replaced whole
(`groups-<slot>-<node>.pending`, synced, renamed, then the directory synced) and split by the
group's mixed-id bits once it passes 4 KiB, so a transition rewrites at most 4 KiB however many
groups the host holds ([idle-groups.md](idle-groups.md) D9); a slot's first file is its root,
whole, and only a stored node splits. A damaged or missing shard costs only its own groups, which
are lost (D11). The table is read once when the host opens. Every transition writes its shard before anything acts on
it; a failed write restores the entry in memory, so the table there never runs ahead of the disk.
A failed close may still have reached the disk, so it also leaves the entry in doubt (below).

- **Naming without a scan.** After a crash, the names a group's artifacts may have are the
  serials in `[mark, high)`. A group's view lists those that exist by probing each name
  (`entry_footprint`), so reclamation and the admission census never list the directory. The
  mark moves past serials that no longer name a file when the table entry is next written; no
  serial is reused, and none is being created then, since a group prepares one artifact at a
  time.
- **Create.** A new entry, or a new incarnation of one whose first turn never became durable
  (the earlier incarnation's artifacts go first), with no serial block: the first artifact
  reserves one. Then the group's first checkpoint record is staged. Create and reopen change the
  entry in memory; the loop commits its changed slots before its next turn with records
  (`TurnDriver::before_turn`), so the entry is durable before that record
  ([idle-groups](idle-groups.md) D5).
- **Park.** `Host::park(loop)` closes every idle group of the loop that no consumer has open (no
  hold, no sealer), as close does, with one commit ([idle-groups](idle-groups.md) D1-D3).
- **Close.** The group's last record must be a durable checkpoint covering every batch. The
  table takes it as `base`; once that shard is durable the loop's log forgets the group
  (`TurnLog::forget`), and retention reclaims its records. A failed commit leaves the group open
  with its records live, and in doubt: its shard may already say closed (a replace whose
  directory sync failed), and any later directory sync could make that durable, so a record
  appended after it would follow a checkpoint the table supersedes. The host refuses every hold
  of a group in doubt (`EIO`), so no Store creates, recovers or appends to it, until the host
  reopens and reads whichever state the disk kept; both cover every acknowledged record, since
  the log never forgot the group. Later commits of its slot write it open. `Host::close_group` refuses a `base` below the group's last
  acknowledged record, the log's durable sequence or the table's earlier `base`, since the
  forget would drop the records after it: the rule TurnLog's checkpoint guard applies to a
  checkpoint's `covers`. Nothing is written, and the group stays open. The forget fails every
  `durable()` waiter of the group with `group_closed`, since a record not yet durable, such as a
  sealer's open record, is never written, and retires the sequences such a waiter could hold
  (`TurnDriver::retired`): through the durable sequence + 1 and every waited one.
- **Reopen.** The table flips the entry to open, then the journal restores the frontier from
  the table's record and stages it again as the group's record `max(base, retired) + 1`, so no
  sequence of the closed incarnation is reused in this process. No turn log is read.
  `TurnLog` ignores a group's records through its `base` (`TurnLogGroups::base`), so a log that
  still holds records from before the close never costs the group, and a crash between the flip
  and that record's barrier leaves the table's record authoritative. The table's record serves
  only while the log holds nothing of the group: a group whose records the log already handed to
  a Store of this host recovers again only after the host reopens.
- **Lost.** Every open marks the ids recovery calls lost, synced, before it serves; their
  artifacts then go. A damaged log also calls lost every open group it holds nothing of
  (`TurnLogGroups::open`): the damage may have held its acknowledged records. `TurnLog` opens with the table's lost ids (`TurnLogGroups::lost`) and refuses
  each, so a lost id stays lost after a re-partition and after retention reclaims the damage.

## Placement and re-partition

A group's slot mixes its id (splitmix64's finalizer) into 4,096 slots; its loop is
`(slot × L) >> 12`, so each loop owns a contiguous run of floor or ceil(4,096 / L) slots. When
the host opens with an L other than the layout's:

1. Read each log of the stored generation in turn (`TurnLog::read`, which writes nothing) and
   write its groups' live records into their new loops' logs of generation + 1, in turns of at
   most a quarter segment; each turn is synced. Memory holds one old log's records and one turn
   per new loop.
2. Mark the ids recovery calls lost in the table, and sync it.
3. Commit by replacing `layout` with (L, generation + 1), then remove the old generation's logs.

Every open removes logs of any other generation than the layout's: an unfinished
re-partition's, or the old generation's that a commit had not yet removed. A crash before step 3
leaves the old generation authoritative and the next open starts again. The cost is one write of
the live records and one barrier per turn of the new logs; the same L copies nothing, and a
parked group copies nothing at any L.

## What the host holds

Its directory descriptor (which is also its lock) and each loop's open segments. A group holds a
table entry and no descriptor: its Store reaches the directory through its view, its owner lock
is the host's, and its census and reclamation cursors probe names without opening the directory.
`bench/idle_cost.cc` counts the process's descriptors with N groups open and idle, then closed.

## Costs at open

Opening lists the directory once (twice when it re-partitions), to find the layout, the table's
shards and the logs; artifact names are skipped, never opened. Then each loop's log recovers as
before.
