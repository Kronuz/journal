# Host::group_io cost by live id count

`Host::group_io(group)` costs time proportional to the number of live group views the host holds, and the cost per view grows with the table size. One call takes 4.8 us at 1,000 ids, 147 us at 10,000, 4.9 ms at 100,000 and 136 ms at 1,000,000.

## What a call does

Every call runs `std::erase_if(ranges_, expired)` (`include/journal/host.h:292`). `ranges_` is a `std::multimap<group id, std::weak_ptr<Range>>` with one entry per group view handed out and still alive (the view's scan closure owns the `Range`). The sweep visits every entry and tests `weak_ptr::expired()`, which reads the use count in the entry's control block: one node visit and one control-block read per live id. The rest of the call (the `Range` allocation, one `emplace`, the `resident` lookup, the `PrefixIO`) does not depend on the table size. A call is therefore O(live ids).

## Measurement

Command, from the repository root (`bench/group_io_bench.cc`):

```
clang++ -O3 -DNDEBUG -std=gnu++20 -Iinclude <dependency includes> bench/group_io_bench.cc -o .scratch/group_io_bench
.scratch/group_io_bench 1000 10000 100000 1000000
```

Revision: `journal:dcbd791`, which is `journal:4d364f1` plus the bench file only; `include/` is identical. Run as `campaign` exclusive job `261118-072911-17102d`, on an Intel Core i9-9980HK (2.4 GHz, 256 KiB L2 per core, 16 MiB L3, 32 GiB), Darwin 24.6.0, Apple clang `/usr/bin/clang++`.

The bench builds a `Host` over `MemoryIO` and places N live `Range` entries directly in `ranges_`, then times `group_io` on fresh ids. Each cell is the median of 5 runs, each run sized to about 0.3 s. "Random ids" are splitmix64 keys (the order a real host sees); "ascending ids" are 1..N, so the multimap's nodes were allocated in key order.

| ids | random ids, ns/call | ns per id | ascending ids, ns/call | ns per id |
|---|---|---|---|---|
| 1,000 | 4,763 | 4.8 | 4,215 | 4.2 |
| 10,000 | 147,173 | 14.7 | 67,680 | 6.8 |
| 100,000 | 4,871,407 | 48.7 | 1,133,686 | 11.3 |
| 1,000,000 | 135,846,060 | 135.8 | 23,822,221 | 23.8 |

Min and max of the 5 runs, and the controls, are in the job's log. Two controls check the injection: a population built by `group_io` itself (10,000 ids, 136,106 ns) and a call on one resident id with the table at 10,000 ids (137,658 ns) time within 8% of the injected random table (147,173 ns). Building through `group_io` is quadratic, so the larger sizes are injected.

The least-squares fit of the random-id medians over the four sizes is

```
ns/call = -3,269,861 + 138.57 * ids        (r^2 = 0.9965)
```

The slope is 138.6 ns per id, dominated by the 1M point. The negative intercept shows the curve is convex: the per-id cost at 1M (135.8 ns) is 28 times the per-id cost at 1K (4.8 ns).

## Why random ids grow faster per id

The sweep walks the tree in key order. Each live id owns a 64-byte multimap node (24-byte entry plus the 32-byte tree header, rounded by the allocator) and a 112-byte `Range` control block, about 176 bytes, so the table is about 1.7 MB at 10,000 ids, 17.6 MB at 100,000 and 176 MB at 1,000,000, against a 16 MiB L3 (sizes from `sizeof`, not measured resident). Past the L2 the table lives in L3 and past roughly 100,000 ids it no longer fits.

With random keys, key order is unrelated to allocation order, so each step of the walk is a dependent load to an unpredictable address, and a miss in the larger tables goes to memory (the per-id cost rises 3.3 times from 10K to 100K, 2.8 times from 100K to 1M). With ascending keys, key order equals allocation order, the walk reads nearly sequential memory and the hardware prefetcher hides most of the latency; the per-id cost still rises from 4.2 to 23.8 ns (5.7 times) as the table outgrows the cache, against 28 times for random ids. This attribution is from the table sizes and the two orderings; no cache-miss counters were collected.

At 1,000 ids the two populations cost the same (4.8 and 4.2 ns per id): the table is in cache and the walk is bound by the node visit itself.

## Consequence

A host with 100,000 live group views blocks 4.9 ms in each `group_io`; with 1,000,000 it blocks 136 ms. Opening N groups costs the sum of those, which is quadratic in N.

The fix, so that a call costs O(1) amortized rather than O(live ids), is the quest `journal-group-io-sweep`.
