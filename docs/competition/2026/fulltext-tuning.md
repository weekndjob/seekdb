# Full-text tuning: zero-score diagnosis and submission candidate

## Why the prior submission scored zero

The platform reported successful execution, not a correctness failure. Its
`fts_mean_improvement=-0.08015937353268185` gives `fts_raw_score=0` after the
negative improvement is clamped. The score averages metric improvements within
build/tokenize/query, then weights those three categories equally. `git_dirty=1`
is provenance information, not evidence that it caused the zero score.

The July checked-in baseline does not reproduce the platform's reported score.
The effective platform baseline is unavailable; no online score is promised.
The previous three small tokenizer changes were insufficient to establish a
robust speedup (an earlier local paired run averaged only +0.40%).

## Production changes

- Decode each IK input character once and reuse its classification in segmenters.
- Avoid an arena/charset conversion when stopwords already share the input
  charset, retaining the stopword set's collation for comparison.
- Aggregate repeated words with one collation-aware hash lookup, preserving
  the original key, first-occurrence count and parser frequency behavior.
- For a single-table scalar COUNT(*) whose only condition is a bare natural
  language MATCH without boosts, compute membership without BM25 scores.
  Thresholds, score projections, grouping, ordering, LIMIT, boolean search,
  boosted/JSON searches and complex predicates retain the scored path.
- Complete score-free DaaT and TaaT initialization, token range switching,
  skip buffers, dummy positive membership scores, duplicate membership and
  TaaT partition setup. This includes queries exceeding 256 search tokens.

This branch starts from official contest source
`5bce9e06d1281347ad61344e4b01fab9158c58a2`. It contains no AI worker, AI column or
file-query feature work. Other working directories were preserved. Neither the
benchmark SQL/scripts/data nor the scoring baseline were modified.

## Validation

Release build of seekdb, storage_tests and sql_tests passed. Six storage and four
optimizer unit tests passed. A separate comparison against the official binary
checked 509 outputs before and after benchmark runs: 488 TOKENIZE outputs over
four parsers, complete MATCH document IDs, COUNT predicates, threshold/boolean
queries, grouped/HAVING/LIMIT queries and prepared statements. No differences.

`unittest/storage/fts/test_fts_count_sql.py` additionally exercises a disposable
fixture including NULL/empty/unmatched search terms, DML, projected/ranked score
plans, numeric thresholds and 300-term retrieval, both literal and prepared.
Run against a disposable test instance, using a root Unix socket:

```sh
build_release/unittest/storage/storage_tests
build_release/unittest/sql/sql_tests
python3 unittest/storage/fts/test_fts_count_sql.py \
  --client deps/3rd/u01/obclient/bin/obclient --socket /path/to/run/sql.sock
```

## Local performance evidence (2026-11-05)

Same Release toolchain and runtime parameters for official/candidate binaries:
CPU count 2; memory limit/budget 2G; datafile 512M; log disk 2G; Unix socket,
MySQL TCP disabled. Benchmark settings are unchanged: rows 20000, batch 500,
rounds 3000, query_rounds 200, samples 3, warmup 30, skip_load 0. Timed runs
never overlap each other or our compilation. Other server users were not stopped.

The first interleaved candidate/official/candidate/official group improved
COUNT queries by 9.93-11.63%, all queries by 7.77%, and the three-category
composite by 3.36%. An additional run (`release-final.txt`) overlapped other
seekdb activity and had substantially slower builds; it is retained as evidence
of environmental variability, not omitted or treated as proof of a regression.

The final adjacent official/candidate/official comparison is below. It averages
the two official runs per metric, then applies the unchanged score formula.
Positive improvement means less elapsed time. These are local measurements,
not predicted platform points.

| Metric | Official mean | Candidate | Improvement |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.842000 | 4.841000 | +0.02% |
| build_ik_content_sec | 3.725000 | 3.724000 | +0.03% |
| build_beng_en_sec | 2.075000 | 2.018000 | +2.75% |
| tokenize_ik_avg_ms | 0.053200 | 0.050300 | +5.45% |
| tokenize_beng_avg_ms | 0.026600 | 0.027900 | -4.89% |
| query_cn_avg_ms | 0.950100 | 0.796900 | +16.12% |
| query_beng_avg_ms | 1.428200 | 1.257000 | +11.99% |
| query_mixed_avg_ms | 1.082050 | 0.939500 | +13.17% |
| query_limit_avg_ms | 1.220600 | 1.229400 | -0.72% |

Final local composite improvement: **+3.78%**. Build/tokenize/query category
improvements: **+0.93% / +0.28% / +10.14%**. All benchmark hits remain
8001 / 11000 / 7332 / 20. The JSON and raw reports are in
`fulltext-tuning-evidence/` beside this file. Reports say `git_dirty=1` because
measurements preceded the delivery commit; the official executable was built
from the clean official revision and preserved separately.

The platform must rerun this commit to establish the actual competition score.
