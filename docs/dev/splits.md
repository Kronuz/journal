# Source layout: split files

Where a file over 1,000 lines was looked at, what it became, and why each kept whole earns it
(cpp-libraries `docs/SPLITTING.md`). A split moves code only: the only added lines are includes,
namespace braces, prototypes, `extern` declarations and comments.

## Split

| File | Pieces |
|---|---|
| `test/host_test.cc` | `host_test.cc`: Host lifecycle, close and reopen, views, handoff, and `main` with the case list the CMake case regex reads. `host_idle_test.cc`: the idle-group cases and their model (`docs/design/idle-groups.md`). `host_support.h`: the recording IO, `Node` and the check helpers both files use. |
| `test/journal_test.cc` | `journal_test.cc`: `main`, the Journal's append, recovery, corruption and POSIX cases, and the sweeps `main` runs under several argument sets (default arguments stay with the definition). `journal_artifact_test.cc`: checkpoints, artifact preparation, publication and incremental verification, reclamation roots, resource plans. `journal_store_test.cc`: the Store over the memory disk. `journal_reopen_test.cc`: a Host's group ids across close, release and reopen, `park_count`, and the heap counter (the replaced `operator new`) only these cases read. `journal_support.h`: the memory-disk model (`MemoryIO`, `Model`) and the helpers that build and replay on it. |

Each piece is a source of the same test target (`journal_host_test`, `journal_journal_test`), so
`ctest -N` lists the same 106 tests. A support header holds its helpers in an anonymous
namespace, one copy per file; the check counters are `extern`, defined by `host_test.cc` and
`journal_test.cc`.

## Kept whole

| File | Lines | Why |
|---|---|---|
| `test/turn_seal_test.cc` | 1180 | One mechanism, `TurnDriver::seal` (`docs/mechanisms/turn-seal.md`): 23 cases share the `Consumer`, the model and the staging helpers. The `repeat*` cases (about 300 lines) use the same sealer API and helpers, so each half would be a fragment of the other. |

## Candidates outside the test files

| File | Lines | Boundary |
|---|---|---|
| `include/journal/turn_log.h` | 1196 | `turn_log_detail` (Emitter, Sectors, RecordReader, manifests, lines 114-496) is a header of its own; the `TurnLog` class body (700 lines) is a restructure, not a move. |
| `include/journal/host.h` | 1068 | `host_detail` (shard and range codecs, lines 95-319) is a header of its own; the `Host` class body (700 lines) is a restructure. |

Neither is split here: the quest's file set is `test/**` and `CMakeLists.txt`.
