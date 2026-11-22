# Host::group_io cost by live id count

`Host::group_io(group)` costs O(log ids) amortized, whatever number of live group views the host holds. One call takes 361 ns at 1,000 ids and 667 ns at 1,000,000 (random ids), a ratio of 1.85 across a 1,000-fold growth.

## What a call does

`ranges_` (`include/journal/host.h`) holds one table per loop, a `host_detail::Ranges`: a `std::multimap<group id, std::weak_ptr<Range>>` with one entry per live `Range`, and its own sweep threshold `sweep_at`. A `Range` is owned by its views' scan closures. A call on group `g` touches only table `loop(g)`, on that loop's thread (`docs/mechanisms/host-ranges.md`), and does three things:

1. **Sweep when the table doubled.** When the table's size reaches its `sweep_at`, `std::erase_if` drops every expired entry and sets `sweep_at = max(2 * size after the sweep, 64)`. A sweep of S entries leaves L, and the next comes after at least `sweep_at - L >= sweep_at / 2` inserts, so a call pays at most 2 entry visits for sweeping: O(1) amortized.
2. **Find the group's range.** The call walks the group's own entries (`equal_range`, O(log ids) plus the group's few entries), erasing the dead ones. Live views of a resident group share one `Range`: the call reuses it. A group that is closed or does not exist yet shares only an unpublished `Range` (`incarnation == 0`), so a new view of a closed group never lists the closed incarnation's artifacts; it gets its own `Range`, and the view held across the close keeps the one it has.
3. **Build the view.** The remaining work (the `Range` when none is shared, one `emplace`, the `resident` lookup, the `PrefixIO`) does not depend on the table size.

`publish` writes a table entry's incarnation, mark and high to each live `Range` of the group, so a group opened N times between sweeps costs one `Range` and one write per commit, not N.

## Bound

Each loop's table is bounded on its own. Between sweeps it holds dead entries. It never exceeds its `sweep_at`, which is the larger of 64 and twice the live count at the last sweep. Entries of views that died since stay resident, about 176 bytes each (a 64-byte multimap node plus a 112-byte `Range` control block kept by the weak count), until the next sweep or until a call on their group drops them. After many views die, nothing is released until enough new ids reach `sweep_at`: up to 2 x the live count at the last sweep, plus 64, stays resident.

## Measurement

Command, from the repository root (`bench/group_io_bench.cc`):

```
clang++ -O3 -DNDEBUG -std=gnu++20 -Iinclude <dependency includes> bench/group_io_bench.cc -o .scratch/gio_after
.scratch/gio_after 1000 10000 100000 1000000
```

Before and after run the same bench source, "before" against the `include/` of `journal:ea397ab` (the per-call `erase_if` over every entry), "after" against `journal:965cd55`'s sweep. Exclusive `campaign` job `261118-123558-25b19c`, the sizes interleaved, on an Intel Core i9-9980HK (2.4 GHz, 256 KiB L2 per core, 16 MiB L3, 32 GiB), Darwin 24.6.0, Apple clang `/usr/bin/clang++`.

The bench builds a one-loop `Host` over faultfs and places N live `Range` entries directly in the loop's table, then times `group_io`. Each cell is the median of 5 runs. A run spans at least 2N calls when they fit in 10 s, so the amortized sweep falls inside every timed run. "Random ids" are splitmix64 keys (the order a real host sees); "ascending ids" are 1..N; "resident" calls the same id every time, so the range is shared.

Median ns per call, before then after:

| ids | random ids | ascending ids | resident |
|---|---|---|---|
| 1,000 | 5,182 then 361 | 4,582 then 395 | 5,812 then 311 |
| 10,000 | 156,254 then 396 | 57,179 then 401 | 149,625 then 388 |
| 100,000 | 5,126,739 then 594 | 1,321,488 then 493 | 5,040,784 then 331 |
| 1,000,000 | 132,537,331 then 667 | 40,510,399 then 590 | 134,144,072 then 354 |

Calls per run after: 324,675 at 1,000 ids, 118,530 at 10,000, 338,983 at 100,000 and 2,000,000 at 1,000,000 (random ids). Min and max of the 5 runs, and a population built by `group_io` itself (7,407 then 358 ns at 1,000 ids; 149,257 then 413 ns at 10,000), are in the job's log.

At 1,000,000 ids a call takes 667 ns against 132.5 ms, about 199,000 times less; at 100,000 ids 594 ns against 5.1 ms. The 1,000,000-id to 1,000-id ratio is 667 / 361 = 1.85. The remaining growth is the map lookup and the cache: 1,000,000 random-keyed nodes are about 176 MB against a 16 MiB L3 (sizes from `sizeof`, not measured resident).

## Why the per-call sweep grew with the table

The sweep walks the tree in key order, one node visit and one control-block read per entry. Each entry is a 64-byte node and a 112-byte `Range` control block, about 176 bytes, so the table is about 1.7 MB at 10,000 ids, 17.6 MB at 100,000 and 176 MB at 1,000,000, against a 16 MiB L3. With random keys, key order is unrelated to allocation order, so each step is a dependent load to an unpredictable address; with ascending keys the walk reads nearly sequential memory and the prefetcher hides most of the latency (the per-id cost before: 5.2 ns at 1,000 ids and 132.5 ns at 1,000,000 for random keys, against 4.6 and 40.5 for ascending). The sweep now runs once per N inserts, so that cost is amortized to at most two entry visits per call.

## Test

`host_test view_sweep` checks that two views of a group share one range, that a table with 4,000 dead views never exceeds twice the live ids (or 64), that every held view still lists its group's first artifact, and that the table falls to the floor once every view dies. `host_test closed_view` checks that a view taken while its group is closed lists none of the closed incarnation's artifacts while the view held across the close keeps its own.
