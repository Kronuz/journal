# One directory per host

Status: design for quest `journal-driver` A3 and A4 (Detent async-drivers D10, Q3 (b), D97);
not built yet. The coroutine drivers (A1, A2) are built: ARCHITECTURE.md, "Coroutine drivers".

Today a shared group keeps a directory of its own: its owner lock, the `journal.shared` marker
and its artifacts, so an idle group holds two descriptors (its directory and its lock) and a
million groups hold two million. Q3 (b) puts every group of a host in one directory under one
owner lock, independent of the host's loop count L, so an idle group holds none.

## Names

```
<host>/
  owner.lock                          the host's owner lock; strata's cold tier takes the same lock
  layout                              RJHL0001: L, generation; replaced whole (layout.pending)
  groups                              RJGT0001: the group table; replaced whole (groups.pending)
  turns-<generation:16x>-<loop:4x>/   one RFTW0001 log per loop of the generation, unchanged inside
  a-<group:16x>-<incarnation:16x>-<serial:16x>   an artifact
```

- **Group ids are opaque and fixed-width**: a `uint64` the host assigns, printed as 16 hex digits.
  No name is derived from an index path, so `twitter` and `twitter/user` can never collide, and
  every operation on one group names only its own files.
- **Artifacts are named by group, incarnation and serial.** The serial counts up per incarnation.
  A group reserves serials in blocks (64) with one group-table update, so after a crash the
  names that may exist unreferenced are the ones between its reclamation mark and its reserved
  high, all probed by name (`inspect_entry`), never by a scan.
- **Turn logs** are named by (generation, loop), each in its own subdirectory, so RFTW0001's scan
  of one directory for `turns-<n>` stays as is (R13).

## The group table

One entry per group the host stores: id, incarnation, state (open, closed, lost), the reserved
serial high and the reclamation mark, and for a closed group its last checkpoint record (the
manifest bytes and sequence), so reopening a closed group decodes its frontier from the table
without reading any turn log. Closing a group publishes a checkpoint covering every batch first.
The table is read once when the host opens and replaced whole (write `groups.pending`, sync,
replace, sync the directory) when an entry changes state: an open, a close, a lost mark, a serial
block. Admission and inventory count a group's live artifacts by name from its frontier and its
probed range.

## Placement and re-partition (A4)

A group's slot hashes its id into 4,096 slots; its loop is `(slot × L) >> 12`. When the host opens
with an L other than the layout's:

1. Recover every log of the stored generation, mark the ids recovery reports lost in the group
   table, and sync it, before anything else (every open makes this mark).
2. Write each group's live records into its new loop's log of generation + 1, and sync each log.
3. Commit by replacing `layout` with (L, generation + 1), then remove the old generation's logs.

`TurnLog` opens with the table's lost ids and refuses each, so a lost group stays lost after the
commit, after a crash at any step, and after retention reclaims the damaged segment. A crash
before step 3 leaves the old generation authoritative; the next open starts again.

## What the host holds

Its directory and lock descriptors, and each loop's open segments. A group holds an entry in the
table and no descriptor: its Store reaches the host's directory through a view that prefixes its
artifact names, and its owner lock is the host's.

## Open for the lead

Each loop's log lives in a subdirectory, which completion's `IO` cannot create or open today
(it has no directory operations). Recommendation: completion gains
`std::unique_ptr<IO> IO::open_directory(std::string_view name, bool create)` (POSIX: `mkdirat`,
then `openat(O_DIRECTORY)`, the parent's directory sync on create) and a matching
`remove_directory` for step 3; journal's `MemoryIO` implements both by prefix. The alternative, segment names prefixed by (generation, loop) in the host directory,
changes RFTW0001's scan, which R13 keeps.
