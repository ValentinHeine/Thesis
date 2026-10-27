# xml-sax-baseline — sequential SAX (Expat) baseline

Thesis evaluation baseline for the chunk-parallel XML input formatter
(`XMLInputFormatIndexer`): the "what you would build without NebulaStream"
comparison. One Expat parser instance per source, each source parsed
sequentially, parallelism only across sources (one thread per source).

## Build

Requires the system libexpat development package; the target is skipped (with a
CMake status message) when it is absent, so the main build never depends on it.

```
apt-get install libexpat1-dev   # inside the dev container: docker exec -u root <container> ...
ninja xml-sax-baseline          # in the CMake build directory
```

## Semantics (matched to the NES pipeline)

- Fields are direct children of the record element, matched by exact tag name,
  converted to the declared native types (same work as the JIT'd read path).
- Default output mode renders each tuple as a CSV line exactly like NES's
  `CSVOutputFormatter` (`std::to_string` for integers, `{:.6f}` with
  trailing-zero stripping for floats, `,`/`\n` delimiters) and accumulates the
  `Checksum`-sink figures (tuple count = newline count, checksum = byte sum).
  The final `systest_equivalent: <count> <checksum>` line is directly comparable
  to a Checksum-sink expectation in a `.test` file.
- `--parse-only` skips rendering and folds converted values into a sink
  (pure parse + convert throughput).

## Validated

`--file .../Bimbo_500MB.xml --preset bimbo` reproduces the
`benchmark/Bimbo_XML.test` Q0 expectation `1373000 2895024047` byte-exactly —
a cross-system correctness check between this baseline and the chunk-parallel
NES pipeline, float formatting included.

## Examples

```
# single stream, checksum mode (graph 1 baseline: flat line vs. worker threads)
xml-sax-baseline --file Bimbo_500MB.xml --preset bimbo --runs 3

# source-parallel (graph 2 baseline: aggregate throughput vs. #sources)
xml-sax-baseline --file Bimbo_500MB.xml --preset bimbo --sources 16

# custom schema
xml-sax-baseline --file data.xml --record rec --field id:INT64 --field price:FLOAT64
```

## Limitations (by design — the baseline stays naive)

- The input is a stream of concatenated record documents; it is wrapped in a
  synthetic root before feeding Expat, so it must not contain an XML
  declaration.
- Expat expands entities (the chunk indexer disables entity translation); the
  benchmark data contains none, so outputs agree.
- No attribute extraction yet (not needed for the element-text schemas).
