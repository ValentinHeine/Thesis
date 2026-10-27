# SIMD structural indexing prototype

Two standalone microbenchmarks behind the "where the indexing time goes" analysis. Both are
self-contained: they need only `rapidxml.hpp` from
`nes-input-formatters/include/InputFormatters/` and a C++20 compiler with AVX2.

```bash
cp ../../include/InputFormatters/rapidxml.hpp .
g++ -O3 -march=native -std=c++20 -o bench_simd bench_simd.cpp
taskset -c 3 ./bench_simd ../../../nes-systests/testdata/large/xml/Bimbo_190MB.xml 9
```

Both benchmarks gate on a **checksum over every emitted field offset**. If the variants disagree the
program reports a mismatch and exits non-zero, so a timing comparison is only ever printed for
variants that produced byte-identical results.

## `bench_simd.cpp` — RapidXML vs. SIMD structural scanning

Three variants of the same job (walk a delimiter-framed XML record stream; emit a `(start,end)`
offset pair per schema field, relative to the raw buffer):

| Variant | Throughput | Speedup |
| --- | --- | --- |
| `rapidxml` | 427 MB/s | 1.00x |
| `simd` — AVX2 scan for `<`/`>` per record, then walk tag positions | 730 MB/s | 1.71x |
| `simd + fast names` — plus direct-mapped name dispatch instead of `std::hash` | 1519 MB/s | 3.56x |

Bimbo_190MB.xml (182 MB, 382 B/record), min of 9 runs, pinned core, order rotated between variants.
Reproducible to within 0.5% over three trials. Plotted as `overleaf_thesis/img/simd_prototype.pdf`
from `overleaf_thesis/data/simd_prototype.csv`.

**What the prototype does and does not do.** It handles the *bare-name fast path* only: direct child
elements of the record root, no attributes, no nested paths, no comments, no CDATA, no entity
references, and no validation of malformed input. RapidXML in the baseline is doing strictly more
work, because it builds a general node and attribute tree. The comparison is therefore
*specialized extractor vs. general parser*, not a claim that this code out-parses RapidXML.

That framing is the point rather than a caveat: a schema-guided indexer never needs a general parse,
only the offsets of a fixed set of fields, so the specialization is available to the real
implementation too.

**Decomposition of the gain.** Only the first step is SIMD. Structural scanning contributes 1.71x;
replacing string hashing with `(length, first byte)` dispatch contributes the remaining 2.1x on top.
The hashing change requires no SIMD and no new parser, and is the cheaper of the two by a wide
margin. `perf` on the RapidXML variant attributes ~47% to `parse_element` and ~10% to
`std::_Hash_bytes`, which is consistent with both figures.

**System-level caveat.** These numbers are for the indexing stage in isolation. Indexing is one stage
of the NebulaStream pipeline, so a 3.56x faster indexer does not imply 3.56x end-to-end; the
worker-scaling results plateau against a framework ceiling that this prototype does not remove.

## `bench_parsecopy.cpp` — parse copy vs. in-place termination

Tests whether the per-record copy into thread-local storage can be avoided by writing `\0` over the
delimiter, parsing in place, and restoring the byte afterwards. `parse_non_destructive` is confirmed
not to mutate the buffer (15 repeated passes produce identical offsets).

| Workload | Bytes/record | Result |
| --- | --- | --- |
| Bimbo_190MB | 382 | **−1.0%** (in-place slower) |
| SDC_small_500MB | 1490 | **+3.4%** (in-place faster) |

The copy is not the bottleneck, and writing into the shared buffer dirties a cache line per record.
That fixed per-record cost is amortized over more bytes as records grow, which is why the sign flips
with record size. Not adopted: the gain is bounded, it is negative on the workload the throughput
evaluation was measured on, and transiently mutating a shared `TupleBuffer` would race with the
spanning-record path of an adjacent task.
