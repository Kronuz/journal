# Changelog

Notable changes to `journal`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#journal) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-02

### Changed

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
