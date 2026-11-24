# Changelog

Notable changes to `journal`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#journal) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-24

### Changed

- The flush mode is the caller's: completion is pinned at `b4600a8`, whose `PosixIO` takes a
  required `PosixIOOptions::flush_mode` (`FlushMode::plain` or `FlushMode::full`) with no
  default, so `PosixIO(directory)` no longer compiles; pass `{FlushMode::full}` for what the
  journal promised before on macOS. Every sync the journal makes follows that `PosixIO`'s mode,
  the turn barrier included. On macOS `plain` makes `fsync` (survives a crash, not power loss)
  and `full` makes `F_FULLFSYNC` everywhere, ordering points included; Linux and FreeBSD make
  the same calls in both ([docs/mechanisms/sync-mode.md](docs/mechanisms/sync-mode.md)).
- faultlab is pinned at `83fbde7`, whose faultfs models plain and full syncs; T9 runs in both
  modes (`journal_turn_log_power`, `journal_turn_log_power_plain`).
- `journal_bench` takes the mode (`journal_bench plain|full [--compare]`), and its comparator,
  now the raw loop, flushes as the mode does; `journal_demo` takes it as its first argument.

### Added

- `test/sync_mode_test.cc`: a host's turn barrier and checkpoint follow its mode, the full-mode
  refusal fails without weakening, and completion's recorded calls match faultlab's model.

## 2026-11-23

### Changed

- The turn log's format is `RFTW0002`: every 512-byte sector holds 504 data bytes, the segment's
  low 32 bits and a CRC-32C over the data, the full segment and the sector's index; fragment
  headers are 4 bytes and block headers carry no CRC
  ([docs/mechanisms/turn-log-damage.md](docs/mechanisms/turn-log-damage.md)). Each sector reads
  alone as current, stale (a recycled file's earlier segment), never written, or damage, so damage
  to the newest turn is reported instead of dropping it as a torn tail: 36 of 201 newest-tail
  cases in the cut-header probe dropped silently under `RFTW0001`. A log reads only `RFTW0002`;
  `RFTW0001` segments do not open. `TurnLogOptions::block_bytes` is a multiple of 512.

### Fixed

- Turn log recovery reports damage to an acknowledged turn that later turns of its block follow
  ([docs/mechanisms/turn-log-damage.md](docs/mechanisms/turn-log-damage.md), rule 6). Damage that
  the sector rules could not prove ended its block's scan, so the damaged turn and every later
  turn of the block dropped as the torn tail without a report: 23 of 4807 earlier-turn cases in
  the cut-header probe. A record of a later turn, read after the failure or past it in its block,
  now proves the failing write durable. Only the newest turn can still drop silently, until a
  per-sector check. The on-disk format (`RFTW0001`) is unchanged.

## 2026-11-22

### Added

- `Host::delete_group(group)` deletes a closed group no Store holds, so a node that left a group
  can rejoin it empty ([docs/mechanisms/host-delete.md](docs/mechanisms/host-delete.md)). The
  group's table entry commits `deleted` (`GroupState::deleted`, the shard's state 4) before its
  artifacts go, and every open finishes a recorded delete. The deleted entry keeps the group's
  floor, so its old records in the turn log stay superseded; the next `create_group` starts the
  next incarnation empty, and a shared `Store::create` starts its records after
  `TurnDriver::retired(group)`. Refused with `std::logic_error` unless the group is closed, and
  with `std::system_error` (`EWOULDBLOCK`) while a hold of it is live.

## 2026-11-21

### Changed

- Damage to a turn log bounds a group's loss instead of losing the group
  ([docs/mechanisms/lost-bound.md](docs/mechanisms/lost-bound.md)). A group the damaged blocks'
  manifests name keeps its records from a checkpoint it kept after the damage, as if undamaged;
  without one, recovery reports it in `TurnRecovery::bounded` with its bound (the highest
  sequence the damage could hold) and its records after it. `Host::open` stores the bound in the
  group's table entry (`GroupEntry::bound`; the shard's state byte carries `bounded_state`) and
  keeps the group, which reports `GroupStatus::bounded`. A reader holding the group through
  `Host::bound(group)` continues it from the records after the bound; one that does not calls
  `Host::lose(group)`. A `Journal` Store loses a bounded group on recover.
- A damaged gap that runs on into later blocks costs each of their manifests, through the last
  block whose bytes may begin a lost record, and is `unknown` when one of them is missing. It
  cost only its first block's manifest: past the newest epoch's last kept turn, a group a later
  block named lost a record silently.

### Added

- `TurnLogGroups::bound`, `TurnLog::keep(group, bound)`, `TurnLog::refuse(group)`,
  `TurnDriver::keep(group)`, `Host::bound(group)`, `Host::lose(group)`.
- `lost_bound_test bounds`, `crash`, `second`, `retention` and `spans`.
- `JOURNAL_FUZZ_DECODER` (`segment` or `checkpoint`) pins `journal_fuzz` to one decoder, so each gets
  fuzzing time of its own; empty keeps the choice by the input's first byte, which is consumed either way.

### Fixed

- `TurnLog` recovery reports damage in a fragment that crosses into a sector where a later
  fragment of its segment verifies, when its own header, or a header of its write before it,
  proves its first sector. Both sectors are then written, so the failure is damage, not a torn
  tail: a damaged byte in such a fragment's second sector, in the last turn, no longer drops
  that acknowledged turn unreported. A fragment that fills the next sector, is the last written
  there, or has a header the sector boundary cuts with no header of its write before it still
  reads as a tear ([docs/mechanisms/turn-log-damage.md](docs/mechanisms/turn-log-damage.md)).
- `TurnLog` recovery no longer reads a tear as damage where the old sector holds another
  segment's record whose group and sequence equal this segment and epoch. A failing fragment's
  header proves its sector only with a type and reserved byte a writer emits; one with a bad type
  or reserved byte is damage only when its checksum verifies with that byte corrected.
- `TurnLog` recovery no longer reads a tear as damage where the old sector's last bytes equal the
  first bytes of a fragment whose header the sector boundary cuts, one stale byte in 256 when a
  single checksum byte lies in the sector. A fragment proves its sector only with its header whole
  there, for a failing block header and a failing fragment alike.

## 2026-11-20

### Added

- `Host::adopt_loop(loop)` hands a loop's range table to the calling thread, for a caller that
  serializes the loop itself, as strata's `turn_sealer` does under its turn mutex. Debug builds
  check a table's calls against one owner thread; after the handoff they check against the
  adopting thread, so a close on a thread other than the first no longer aborts.
- `Host::park(loop)` parks the loop's idle groups: every group no consumer has open (no hold,
  no sealer), with nothing staged and one live record, a checkpoint, closes into the table with
  one commit for them all. A parked group is a closed entry: no log record, no heap, 45 bytes plus
  its checkpoint in its shard ([docs/design/idle-groups.md](docs/design/idle-groups.md)).
- `TurnDriver::before_turn(hook)`: a host's table commit, run before each turn with records.
- `TurnLog::peek(group)` reads a group's live records without restoring it; `TurnLog::idle()`
  lists the groups whose only live record is a checkpoint.
- `host_test split_crash`, `split_stale_node`, `table_damage`, `park_mid_turn`,
  `idle_unmanifested` (review F3's unmanifested damage of a woken group's turn), `split_orphan`,
  `missing_shard`, `first_split_crash` and `park_unheld`.
- `TurnLogStats::forward_scanned` counts the live records `forward_slice` visits.

### Changed

- The group table is bounded: each slot's table is a trie of shard files
  `groups-<slot:3x>-<node:9x>`, split once one passes 4 KiB, so a create, wake, park or block
  reservation rewrites one file of at most 4 KiB instead of its whole slot (15 KB at 1M groups).
  The old `groups-<slot:3x>` files are not read.
- A damaged or missing table shard no longer fails the host's open: every id it could hold is
  lost, the shard is never rewritten, and the log forgets its groups. A slot's first commit
  writes its root whole; only a stored shard splits.
- A closed or parked group leaves the host's record of groups a consumer opened, so strata's
  groups, which take no hold, cost no heap once closed.
- The in-memory release step is renamed evict: `TurnDriver::evict`, `TurnDriver::evicted`,
  `TurnLog::evict` and `TurnLog::evicted` replace `park` and `parked`, which now name the durable
  table state alone (`Host::park`).
- `Host::create_group` and `Host::reopen_group` no longer commit their shard when they return:
  the loop's changed slots commit before its next turn with records, so each transition is
  durable before the turn carrying the group's first record, and a turn pays one batched file
  sync and one directory sync for all of them. A failed commit there fails the driver, as a
  failed turn does. A created group's first serial block is reserved, durably, by its first
  artifact.
- A table commit syncs every shard it replaces with one `sync_files` call, then renames them and
  syncs the directory once.
- A re-partition streams one old log at a time into the new generation, so its memory is one
  log's live records and one turn per new loop, not every log's records.

### Fixed

- `TurnLog` recovery no longer drops acknowledged records silently when a damaged block is
  followed by a block whose header, the damaged block's manifest, is damaged too. A header or
  fragment that fails while its 512-byte sector holds bytes bound to its segment is damage,
  which no crash leaves: past the last kept turn it is reported instead of read as a torn tail,
  and the turns dropped there cost every group with a record in them. Damage that no manifest
  names now costs every group, where it cost only those that wrote nothing after it: a
  forwarded copy after the damage read as such a write and kept a group whose newer records the
  damage held. A byte-granular cut inside a sector, which the sector crash model never leaves,
  now reads as damage.
- `TurnLog` recovery reports damage that reads as a segment's unwritten end: a tail gap holding
  a header or fragment that fails while its sector holds bytes bound to its segment costs what
  its block's manifest names, or every group. Before, in the oldest segment it cost nothing, so
  a group whose records were all there vanished unreported, and before an incomplete
  acknowledged turn it lost its charge to an earlier segment's empty tail.
- A fragment whose damaged length makes it cross its sector, read as a tear before, is damage
  when its checksum verifies at a length its sector holds whole: a flipped length byte in the
  last turn no longer drops that acknowledged turn as the torn tail.

## 2026-11-19

### Changed

- `TurnDriver::Hold` takes its loop as a required parameter with no default, and throws
  `std::logic_error` at construction when the loop is empty, before it holds the driver, as
  `durable()` refuses a scheduler without `post_call`. A holder without a loop could not be
  woken by `wake()`; code that built a `Hold` without one no longer compiles.

### Fixed

- A `TurnDriver::durable()` wait cancelled from another thread no longer unlinks its waiter on
  the cancelling thread, racing the loop's list. Any `durable()` wait that suspends on a
  scheduler that declares no `post_call` now throws `std::logic_error` before it links, whatever
  its token, even none; the token's callback always hops to the caller's loop.
- A `TurnDriver::wake()` from another thread ends the blocking entry's sleep toward a deadline
  at once, as it resumes `run()`: the woken turn commits on the entry's next pass instead of when
  the deadline fires. `TurnDriver::Hold` takes the holder's loop, where `wake()` posts.
- A `Store::close` whose table commit fails no longer leaves its group appendable in the live
  Host while the shard on disk may say closed, which a later directory sync and a power cut
  turned into a lost group. The Store is fenced and the group is in doubt: `create` and `recover`
  of the id in the same Host throw `std::system_error` with `EIO` until the Host reopens and
  reads whichever state the disk kept. See
  [group lifetime](docs/mechanisms/group-lifetime.md#the-rule).

## 2026-11-17

### Changed

- Every durable file sync goes through the IO seam: the host table's commit, the turn log's
  segment preparation and turn barrier, and the journal's header and torn-tail syncs call
  `IO::sync_files` or `IO::sync_data_files` instead of `File::sync`, so a host that counts or skips
  syncs sees them. Pins completion at `f4c19b1`; `PrefixIO` forwards `sync_data_files`;
  `TurnLog::flush(write)` is a turn's barrier for a driver that writes it itself.
- `TurnDriver` writes the oldest pending records that encode in one segment as a turn and leaves
  the rest for the next, so a wave of staged records larger than a segment commits over several
  turns instead of failing every operation with "turn exceeds a segment"; a load that fit one turn
  still takes one. A record that does not encode alone in a segment fails with
  `std::length_error`, together with its group's later records and the `durable()` waits on them;
  the driver and other groups carry on. `TurnLog::fitting` returns how many records, from the
  first, one turn encodes in an empty segment. The turn format is unchanged.
  See [the turn-seal note](docs/mechanisms/turn-seal.md#staged-records-one-segment-per-turn).
- `TurnDriver` calls a sealer again in the same turn while it returns records, with the room left
  after each record's `record_cost` and `first = false`, until it returns nothing or less than
  `record_cost(0)` is left, so one turn carries a group's backlog. A sealer returns nothing when it
  has no more. A repeat call that throws or returns a refused record leaves the earlier records in
  the turn; once that turn completes and acknowledges them, the driver fails with the error and
  `completed()`, `commit()` and `run()` throw it. The rotation still starts the next turn at the
  group the last one left waiting; there is no per-group cap. See
  [the turn-seal note](docs/mechanisms/turn-seal.md).
- `TurnDriver::unseal(group, owner)` from another thread waits for all of the group's calls in the
  turn being sealed, not only the one in progress.
- A `durable()` waiter whose record a turn made durable is answered even if the driver fails
  right after that turn; waiters the failure resolves still throw it.
- `Host::close_group(group, sequence, manifest)` throws `std::logic_error` when `sequence` is
  below the group's last acknowledged record, the log's durable sequence or the table's earlier
  `base`, and writes nothing: the forget would drop the records after it. `Journal::close` always
  passes its last record and is unaffected.
- `TurnDriver::run` throws `std::logic_error` before it holds the driver when its loop's scheduler
  declares no `post_call`, where `wake()` reaches it, so a wake is never refused while `run()`
  serves. A `wake()` from another thread needs that `post_call` to be thread-safe (task's
  `asio_scheduler` and `blocking_scheduler`; not `inline_scheduler`).
- `TurnDriver::forget` (behind `Host::close_group`) fails every `durable()` waiter of the group
  with the new `group_closed`, whose record is never written, and retires the group's sequences
  through its durable sequence + 1 and every waited one (`TurnDriver::retired`): a `durable()` at
  or below them fails with `group_closed`, `stage()` refuses a record there, and `Journal` reopens
  the group after them. A close with a sealed open record in flight no longer acknowledges it
  at the reopen.

## 2026-11-16

### Added

- `TurnDriver::seal(group, fn, owner)`: a group's sealer, `fn(room, first)`, returns at most one
  record per turn, its open record at the group's last staged sequence + 1, called after the
  turn's pending records and before encoding, so every group's record shares the turn's one write
  and data barrier. Registering again replaces it; `unseal(group, owner)`, from any thread,
  removes it only for its owner and returns once it is not running and never will be. `wake()`, from any thread, asks for a turn when an open record takes its first
  transaction; sealers run under no driver lock. The first
  sealed record of a turn may fill an empty segment; later ones share `segment_bytes / 2`, and a
  sealer the budget leaves out is called first next turn. A record out of sequence or past its
  budget fails the turn. A `durable()` waiter above a sealed group's staged sequence asks for a
  turn, so `idle()` is false while one is owed. `forget(group)` unregisters the sealer. No format
  change. `TurnLog::options()` and the turn bound (`turn_log_detail::turn_bound`, `turn_room`) are
  new too. [docs/mechanisms/turn-seal.md](docs/mechanisms/turn-seal.md) has the contract.

- `TurnRecord::covers`: a checkpoint's covered sequence, carried by every checkpoint record.
  `TurnLog::stage` and `append` refuse a checkpoint that covers less than the record before it,
  which it would drop when it lands; 0 (the default) covers nothing, so only a group's first
  record may leave it unset. Not stored, so no format change. `Journal` sets it on each
  checkpoint it stages; a caller building checkpoint records itself must set it too.

### Changed

- `commit()` with nothing pending calls the sealers and writes what they return; it throws "no
  records to write" only when nothing was pending, owed, or sealed.

## 2026-11-02

### Changed

- Pins completion at `576618d`, its `main` head, where
  `AsioCompletionDriver` returns `task::task<void>`. journal's headers and tests do not use the
  driver, and `completion::completion` is unchanged: no API of journal changes.
- Pins completion at `e2c6474`, its head, which moves completion's own metrics and allocators
  pins to their heads (docs-only changes past the previous pins).

## 2026-11-01

### Changed

- The README follows the family README standard: the description as its first sentence,
  Quick start before Installation (one program that creates, appends and recovers), a
  pinned FetchContent tag, and "Not for" limits. The notes on `grow_replacement` and on
  shared reservation accounting move to `ARCHITECTURE.md`. The Highlights no longer quote
  the 64 ms checkpoint publication sample, a single October measurement of an earlier
  revision that `docs/MEASUREMENTS.md` keeps.

### Removed

- The README diagram (`docs/journal.d2`, `docs/journal.svg`), an 811 x 851 layer list of
  `Store`, the two backends and completion I/O that the pitch states in one sentence.

## 2026-10-31

### Changed

- allocators is pinned at `2fc537f`.

## Before 2026-10-31

### Added

- `Store::grow_replacement(id, minimum_bundle_cap)` and `Admission::grow_replacement`
  raise an existing protected checkpoint reservation without I/O, keeping the same permit,
  ticket and replacement identity. Durable settlement and uncertain-I/O fencing are
  unchanged.

### Changed

- Checked resource arithmetic and the committed and reserved counters moved to
  `allocators::resources`. The shared aliases keep their C++ source names, but their
  canonical type identities and some internal object layouts changed: rebuild C++
  dependents together and do not mix precompiled objects from before and after the move.
  No disk format or I/O backend changed.
- CRC-32C moved to [crc32c](https://github.com/Kronuz/crc32c): `journal::crc32c` forwards
  to `crc32c::crc32c` and `journal::Checksum` is its `Crc32c`. Persisted bytes are
  unchanged; `test/fixtures/crc32c` holds a journal directory and a turn log written by the
  earlier byte-table CRC, and they keep verifying.
