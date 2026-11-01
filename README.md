# journal

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/journal/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/journal/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Header-only crash-safe journal for C++20: durable batches and checkpoints that recover exactly what was acknowledged.
Append an opaque batch and get back a durable frontier, publish immutable checkpoint
generations, and recover the acknowledged prefix after a crash; a `Store` drives either a
per-group journal or a shared turn log over [completion](https://github.com/Kronuz/completion)
I/O. It is the storage under [consensus](https://github.com/Kronuz/consensus)'s Raft groups,
and fits anyone who needs a write-ahead log with real durability barriers.

## ✨ Highlights

- **Durable on return.** An append returns only after its barriers, macOS uses
  `F_FULLFSYNC`, and an unsupported operation fails instead of weakening the contract.
- **Fails closed.** Corruption anywhere in the acknowledged prefix stops recovery, and any
  uncertain I/O fences the journal until a fresh instance recovers.
- **Checkpoints without long pauses.** Artifacts are prepared, sealed and verified before
  the cutover, so publication does not reread them.
- **Bounded disk.** Admission reserves the worst-case footprint before any write, and
  reclamation removes only files it proves it owns.
- **One barrier for many groups.** The shared turn log persists every group on a core with
  one data barrier per turn.
- **Ready for completion I/O.** Every mutation is a resumable state machine that
  completion's BSD AIO and Linux io_uring backends can drive off the owner's thread.

## 🚀 Quick start

```cpp
#include "journal/journal.h"
#include "completion/posix.h"

#include <filesystem>
#include <iostream>

namespace j = kronuz::journal;

int main() {
	std::filesystem::create_directory("store");      // a dedicated directory
	j::Identity identity{};                           // 128 bits, unique per store
	identity[0] = 'S';
	{
		j::PosixIO io("store");
		j::Journal journal(io, 1024 * 1024);          // batches of at most 1 MiB
		journal.create(identity);
		auto frontier = journal.append_batch("hello");  // durable on return
		std::cout << "sequence " << frontier.sequence << '\n';  // sequence 1
	}
	j::PosixIO io("store");                            // after a restart
	j::Journal journal(io, 1024 * 1024);
	journal.recover([](auto, std::string_view batch) { std::cout << batch << '\n'; });  // hello
}
```

[`examples/demo.cc`](examples/demo.cc) is a small command-line tool around these calls.

## 📦 Installation

Requires a C++20 compiler and CMake 3.16 or newer; it fetches
[completion](https://github.com/Kronuz/completion),
[allocators](https://github.com/Kronuz/allocators) and
[crc32c](https://github.com/Kronuz/crc32c) at pinned commits. CI builds it on Linux (x86_64,
arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(journal
  GIT_REPOSITORY https://github.com/Kronuz/journal.git
  GIT_TAG        d5b8bc194afc520762ddf077e2171095771dbf00)  # or main, to track the tip
FetchContent_MakeAvailable(journal)

target_link_libraries(my_app PRIVATE journal::journal)
```

```cpp
#include "journal/journal.h"
```

## 🎯 When to use it

Use it for a log of opaque records that must survive power loss (Raft logs, write-ahead
logs, state machines with snapshots), and for many small groups on one machine, where the
shared turn log amortizes the barrier.

Not for:

- **Key-value or queryable data.** Records are opaque and replay in order.
- **Network filesystems or storage that ignores flushes.** The durability model assumes a
  local filesystem that honors `fsync` and atomic rename.

## ⚡ Performance

`journal_bench --compare` appends 64 batches of 1, 32 or 256 operations of 256 bytes each and
times each `append_batch()` until it returns durable; the table gives the median append in
milliseconds, the range over two runs. The comparator, the *plain loop*, is what a caller writes
without the library: one `write` and one `F_FULLFSYNC` per batch to one file, with no batch
header, checksum, manifest or recovery. Before timing, the bench checks that the journal's
recovered batches, the plain file and the input are the same bytes. Counted over the journal's
192 appends on both Macs, a journal append makes three `F_FULLFSYNC` calls, one `fsync` and
three `pwrite` calls; the plain loop makes one `F_FULLFSYNC`, so it waits about a third as long.

| Operations per batch | journal, Apple M4 Pro (ms) | plain loop, M4 Pro (ms) | journal, Intel Core i9-9980HK (ms) | plain loop, i9 (ms) |
|---:|---:|---:|---:|---:|
| 1 | 13.0 to 15.0 | 4.0 to 4.1 | 63.0 to 63.4 | 21.0 |
| 32 | 13.1 to 13.2 | 4.1 to 4.6 | 63.1 to 63.3 | 21.0 |
| 256 | 12.2 to 12.9 | 4.0 | 63.1 to 63.6 | 21.0 to 21.2 |

The flush sets the pace, so throughput grows with the batch: at 256 operations per batch the
journal makes 19,500 to 20,000 operations durable per second on the M4 Pro and 3,900 to 4,000
on the i9. [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md) has the checkpoint and reclamation
benchmarks.

Apple M4 Pro (arm64), macOS 26.7.2, internal SSD, and Intel Core i9-9980HK (x86_64), macOS
15.8.1, internal SSD, both with Apple clang 17.0.0 and `-O3 -DNDEBUG`, at `5a94035`; run it from
the repository root:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target journal_bench
./build/journal_bench --compare
```

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): the contract, checkpoints, reclamation, admission,
  `Store`, completion-driven operations, the turn log and the durability model.
- [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md): append, checkpoint and reclamation
  measurements, and the qualification log.
- [CHANGELOG.md](CHANGELOG.md): dated changes.
- [AGENTS.md](AGENTS.md): repository map and the rules for changing storage code.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries): the catalog.

## 🛠️ Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The benchmarks (`journal_bench`, `journal_checkpoint_bench`, `journal_reclaim_bench`,
`journal_turn_bench`) build with the tests, run from the repository root and write under
`.scratch/`; `journal_bench --compare` adds the plain write-and-flush loop. Add `-DJOURNAL_FUZZ=ON` (Clang) for the libFuzzer target.

## 🤝 Contributing

Read [AGENTS.md](AGENTS.md) and the [catalog's rules](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md)
first; storage changes need fault schedules for every new primitive.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
