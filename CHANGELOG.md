# Changelog

Notable changes to `journal`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#journal) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-17

### Changed

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
