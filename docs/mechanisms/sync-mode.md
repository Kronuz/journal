# Sync mode

How far the journal's syncs push data on macOS, and who chooses. The journal makes no sync
system call itself: every sync is a call on completion's `IO` or `File`, and completion's
`PosixIO` makes it as the flush mode it was built with names
([completion's flush-mode.md](https://github.com/Kronuz/completion/blob/main/docs/mechanisms/flush-mode.md)).
So the mode is chosen once, where the caller builds the `PosixIO`, and a `Host`, `Journal` or
`Store` over it follows it everywhere.

## Choosing it

```cpp
auto io = std::make_shared<j::PosixIO>(directory, j::PosixIOOptions{j::FlushMode::full});
j::Host host(io, identity, options);
```

`PosixIOOptions::flush_mode` is required and has no default: `PosixIO(directory)` and
`PosixIOOptions{}` do not compile, and there is no process-wide switch. Detent opens `full`;
Xapiand opens `plain`, and `full` under `--full-flush-ordering` (Xapiand PLAN D177).

## What each sync becomes

| Journal sync | Completion call | macOS full | macOS plain | Linux, either mode |
|---|---|---|---|---|
| Turn barrier, `commit()` | `IO::sync_data_files`, one file | `F_FULLFSYNC` | `fsync` | `fdatasync` |
| Turn barrier, `run()` | `SyncData` primitive: the queue's worker calls `File::sync_data` on the file | `F_FULLFSYNC` | `fsync` | io_uring's own `fdatasync` |
| Artifact, manifest, table | `IO::sync_files`, `File::sync` | `fsync` each, then `F_FULLFSYNC` per device | `fsync` | `fsync` |
| Namespace barrier | `IO::sync_directory` | directory `fsync`, then `F_FULLFSYNC` on `owner.lock` | directory `fsync` | `fsync` |
| Turn log segment change | `IO::order_files_and_directory` | `fsync` each, then `F_FULLFSYNC` | `fsync` each, then one `F_BARRIERFSYNC` | `fsync` each and the directory |

FreeBSD makes `fsync` everywhere, in both modes, and its AIO backend issues the run's barrier
itself. Each `PosixFile` copies its backend's mode when it opens, so the queue worker's direct
`File::sync_data`, which no `IO` wrapper sees, follows the mode too.

## What survives

| Event | full | plain |
|---|---|---|
| Process or kernel crash | every acknowledged record | every acknowledged record |
| Power loss, macOS | every acknowledged record | nothing promised: `fsync` leaves data in the drive's cache; each group keeps a prefix of its log, or is reported lost |
| Power loss, Linux and FreeBSD | as the filesystem's `fsync` | the same calls, the same promise |

In full mode an unsupported operation fails instead of weakening: an `F_FULLFSYNC` that fails,
with `ENOTSUP` or any other errno, fails the turn or the publication with that errno, no `fsync`
follows it, and the turn log fails. Plain mode's only fallback is stronger: a filesystem without
`F_BARRIERFSYNC` gets `F_FULLFSYNC` at the ordering point.

## The crash model in both modes

faultlab's faultfs models each mode's calls (`filesystem::set_flush_mode`); T9 sets the macOS
calls on every host, so the two modes differ on Linux CI too.

- `journal_turn_log_power` (full): a power cut after every mutation, and again during
  recovery: every acknowledged record survives, the in-flight turn whole or not at all.
- `journal_turn_log_power_plain`: the same cuts, each ended twice. As a process crash
  (`power_on`), every acknowledged record survives. As a power loss (`crash`), recovery keeps of
  each group not reported lost a prefix of a state it was in, the in-flight turn's included:
  never a record that was not written or one out of place. The sweep requires that some power
  loss does lose an acknowledged record, so a plain model that kept everything would fail it.
  It tallies what plain mode costs: of 5,600 crash points, 5,238 power losses lost an
  acknowledged record and 2,673 reported damage; one kept only part of a turn (group 1's
  sequence 1 of a turn that wrote 1 and 2, a recycled segment's name restored by the crash), and
  it reported damage. A part of a turn is a shorter prefix of the group's log, not a reorder.

## Checked against the real calls

`test/sync_mode_test.cc` installs a recorder through `PosixIO::replace_sync_calls` before any
file opens:

- `host`: a host in each mode; the turn barrier through `commit()` and through `run()` on the
  native queue is exactly the call the mode names for `File::sync_data`, and a checkpoint's
  publication in full mode never issues a bare barrier and ends on `F_FULLFSYNC`, in plain mode
  never issues `F_FULLFSYNC`. Off macOS both modes record the same calls.
- `refusal`: the full-mode call refused with `ENOTSUP` fails the turn with that errno and
  nothing after it; on macOS plain mode never makes it.
- `table`: for each sync site (`File::sync`, `File::sync_data`, `IO::sync_directory`,
  `IO::order_files_and_directory`) and mode, the site's last recorded call is faultlab's
  `call_for`, with the same `persists`, so a call completion weakens fails here instead of
  staying durable in the crash model; and completion's `sync_call` equals faultlab's `call_for`
  for every site, mode and platform.

## Cost

Not yet measured: `journal_bench plain|full` against main, on macOS.
