# One directory per host

Detent async-drivers Q3 (b), D97, D105: every group of a host and every loop's turn log live in
one flat directory under one owner lock, independent of the host's loop count L, so an idle
group holds no descriptor. `include/journal/host.h` builds it; ARCHITECTURE.md, "One directory
per host", summarizes it.

## Names

```
<host>/
  layout                                            RJHL0001: L and the log generation
  groups-<slot:3x>                                  RJGT0001: the group table's entries of one slot
  l<generation:16x>-<loop:4x>-turns-<segment:16x>   a loop's RFTW0001 segment
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
manifest bytes). The table is sharded by slot, each shard replaced whole (`groups-<slot>.pending`,
synced, renamed, then the directory synced); at a million groups a shard holds about 244 entries.
It is read once when the host opens.

- **Naming without a scan.** After a crash, the names a group's artifacts may have are the
  serials in `[mark, high)`. A group's view lists those that exist by probing each name
  (`entry_footprint`), so reclamation and the admission census never list the directory. The
  mark moves past serials that no longer name a file when the table entry is next written; no
  serial is reused, and none is being created then, since a group prepares one artifact at a
  time.
- **Create.** A new entry, or a new incarnation of one whose first turn never became durable
  (the earlier incarnation's artifacts go first). Then the group's first checkpoint record is
  staged, as before.
- **Close.** The group's last record must be a durable checkpoint covering every batch. The
  table takes it as `base`, the loop's log forgets the group (`TurnLog::forget`), and retention
  reclaims its records.
- **Reopen.** The table flips the entry to open, then the journal restores the frontier from
  the table's record and stages it again as the group's record `base + 1`. No turn log is read.
  `TurnLog` ignores a group's records through its `base` (`TurnLogGroups::base`), so a log that
  still holds records from before the close never costs the group, and a crash between the flip
  and that record's barrier leaves the table's record authoritative.
- **Lost.** Every open marks the ids recovery calls lost, synced, before it serves; their
  artifacts then go. `TurnLog` opens with the table's lost ids (`TurnLogGroups::lost`) and refuses
  each, so a lost id stays lost after a re-partition and after retention reclaims the damage.

## Placement and re-partition

A group's slot mixes its id (splitmix64's finalizer) into 4,096 slots; its loop is
`(slot × L) >> 12`, so each loop owns a contiguous run of floor or ceil(4,096 / L) slots. When
the host opens with an L other than the layout's:

1. Read every log of the stored generation (`TurnLog::read`, which writes nothing), mark the ids
   recovery calls lost in the table, and sync it, before anything else.
2. Write each group's live records into its new loop's log of generation + 1, in turns of at
   most a quarter segment; each turn is synced.
3. Commit by replacing `layout` with (L, generation + 1), then remove the old generation's logs.

Every open removes logs of any other generation than the layout's: an unfinished
re-partition's, or the old generation's that a commit had not yet removed. A crash before step 3
leaves the old generation authoritative and the next open starts again. The cost is one write of
the live records and one barrier per turn of the new logs; the same L copies nothing.

## What the host holds

Its directory descriptor (which is also its lock) and each loop's open segments. A group holds a
table entry and no descriptor: its Store reaches the directory through its view, its owner lock
is the host's, and its census and reclamation cursors probe names without opening the directory.
`bench/idle_cost.cc` counts the process's descriptors with N groups open and idle, then closed.

## Costs at open

Opening lists the directory once (twice when it re-partitions), to find the layout, the table's
shards and the logs; artifact names are skipped, never opened. Then each loop's log recovers as
before.
