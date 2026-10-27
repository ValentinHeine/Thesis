# Evaluation results

Measured data behind the two evaluation graphs. Numbers are **tuples/s** throughout — not MB/s,
because the XML and CSV renderings of the same workload differ ~8.7x in size (524.6 MB vs 60.5 MB
for an identical 1.373M tuples), so MB/s systematically flatters XML.

Machine: dev container `nes-baseline-dev`, 16 cores, idle (load avg 0.3–0.9, >13 GB free).
All runs COMPILER execution mode. All NES runs were validated by systest (`pass=1`), not just timed.

## graph1_threads.csv — single-stream throughput vs. worker threads

Date: 2026-07-20. Workload: `Bimbo_XML.test` Q0 (full scan), 1.373M tuples.
Input: `nes-systests/testdata/large/xml/Bimbo_500MB.xml` (524,576,618 B / 1,373,000 lines).
CSV twin: `nes-systests/testdata/large/csv/Bimbo_500MB.csv` (60,502,618 B).
Reps: NES 3 per point, Expat 5 (spread ~2–5%; Expat's cold-cache first run was indistinguishable
from the rest, so no page-cache artifact).

**`expat_sax` has exactly one data point, at threads=1, and this is the substantive result — not a
gap in the measurements.** A SAX parser cannot use more than one thread on a *single* stream: the
parser's continuation state (byte offset, element-depth stack, in-flight partial tag) is not
transferable between threads. Plot it as a horizontal reference line and say so in the caption.

`nes_csv` is a control, not a competitor: it bounds how much of the curve's shape is the framework
rather than XML. Both formats peak early (XML at 4T, CSV at 2–4T) and decline through 8T/16T — a
shared framework ceiling (source-ingest thread or checksum-sink serialization), **not** XML-specific.
Per-tuple XML cost is ~1.9–2.9x CSV, shrinking as thread count rises.

Invocation (NES):

    systest -b --workingDir=/tmp/xml-bench --data /tmp/nebulastream/nes-systests/testdata \
      -t /tmp/nebulastream/nes-systests/benchmark/Bimbo_XML.test:1 -- \
      --worker.default_query_execution.execution_mode=COMPILER \
      --worker.query_engine.number_of_worker_threads=N \
      --worker.number_of_buffers_in_global_buffer_manager=500000

Worker args are snake_case; **unknown keys terminate silently** rather than erroring.

Invocation (baseline):

    xml-sax-baseline --file .../Bimbo_500MB.xml --preset bimbo --runs 5

## graph2_sources.csv — aggregate throughput vs. source count

Date: 2026-07-24. Raw output: `raw/ms2_sources_190MB_2026-07-24.txt`.
Input: `nes-systests/testdata/large/xml/Bimbo_190MB.xml` (191,033,000 B / 500,000 lines),
500k tuples per copy, per-copy checksum `1054269500`. N identical physical File+XML sources
attached to one logical source (pattern from `nes-systests/operator/union/SourceExpansionUnion.test`),
all reading the same shard. NES worker threads **fixed at 8**; Expat runs one thread per source.
Expected checksums cross-validated against the Expat baseline's own `--sources N` aggregate
(per-copy `500000/1054269500 x N`, exact match) — cross-implementation correctness validation,
not just timing.

Supersedes an earlier 100k-line/38 MB run whose apparent collapse at N32/N64 was **setup overhead
on ~0.8 s runs, not a scaling wall** — it did not reproduce at this size. Do not cite that run.

### How to read this graph

This is **not** a performance claim, and NES does not beat Expat here. Expat throws one thread per
source at up to 16 cores and scales linearly to core count; NES is capped by its fixed 8-worker pool,
which is what a fixed pool is *for*. Crossover is ~N3–4.

What the NES line shows is a **property, not a speed**: throughput is flat at ~430k from 1 to 32
sources, i.e. **invariant to source count**. That is evidence the indexer holds no per-source state —
`indexRawBuffer(string_view) -> RawBufferIndex` is a pure function of one buffer. The scheduling that
turns that into parallelism is NebulaStream's, not this work's; CSV and JSON get the same treatment.
Cite this as a one-paragraph statelessness check supporting graph 1, not as a second speedup.

The regime where decoupling would actually pay — bounded thread budget with many *continuous*
streams, where a thread-per-source design must either oversubscribe or starve sources — is not
testable with finite files and was not measured. Present it as the scenario the property enables,
flagged as future work.

## Caveat on both graphs

The 8T/16T decline in graph 1 and the gentle N32 dip in graph 2 are **not root-caused**. They are
attributed to a shared framework bottleneck by inference (code reading + the CSV control showing the
same shape), not by profiling. If either appears in the thesis with any precision, back it with
`perf`, not with this note.
