# AGENTS.md — journal

Durable record storage over completion I/O. Header-only; `include/journal/`.

## Repo map

```
include/journal/   journal.h (the journal), store.h, artifact.h, mutation.h,
                   reclamation.h, inventory.h, admission.h, codec.h, primitives.h,
                   resources.h.
test/              journal (failure injection over the POSIX backend), inventory,
                   admission, completion (native backends), linux_completion (Linux).
bench/             append, checkpoint, reclamation, and completion benchmarks.
examples/demo.cc   A runnable tour.
```

## Verify a change

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Completion-driven storage work

`include/journal/mutation.h` provides explicit resumable append and shared manifest publication states. Both synchronous and completion drivers must execute these same states. Owned backend construction retains the directory, files and stable owner session across suspension. An accepted driver retains the operation until every original primitive is reaped; only matching owner/operation/step completions advance it. Final barriers precede frontier settlement. Never offload `Worker::run_one()` as a whole, invent completion after cancellation, or refund started storage permits before terminal settlement. completion's `bsd_completion.h` supplies a one-slot native-AIO/fallback backend, and optional `completion/asio_completion.h` supplies the owner-executor coroutine driver. macOS writes and required full-flush fallbacks are qualified; FreeBSD native qualification remains open; Linux now has an owned io_uring backend with native guest qualification recorded in the journal README. Store and Worker now provide opt-in owned completion appends. Owned artifact creation/chunk/sealing jobs also retain a shared replacement cycle with an exact job lease and detached preparation settlement. Checkpoint publication also uses an owned operation and admitted whole-cycle lease; detached publication must complete or fence rather than settle as orphan preparation. Worker preparation/publication, verification, source reads and reclamation now have completion dispatch; bootstrap/recovery remain synchronous before admission, and existing default library paths remain synchronous. Never describe the intermediate append profile as a fully asynchronous service.

CTest configuration creates its build-local `.scratch/` parent before filesystem fixtures run. Fresh build directories must not depend on a developer having created that parent manually.

Linux owned completion now has a raw io_uring queue in completion's `linux_completion.h`, selected by `native_completion.h`; shared completion values live in `completion.h`. Preserve one original primitive, reconcile published submissions after enter errors without fallback duplication, and retain final file/directory barriers. Initial Linux kernel fixtures are qualified; authority, sanitizer and component acceptance remain in progress. Never treat automatic fallback as native qualification.
