# Column minor 6: size and build time

Measured 2026-10-20, PostgreSQL 18.6, AMD Ryzen 7 9700X (8 cores, 16 logical CPUs), nice 19, serial index builds. Three alternating runs on warmed data. The shared host had load averages around 10–11, so these short timings do not establish a speed regression or improvement.

The store generator (`bench/sql/store/gen.sql`, `n=1000000`) and the column index from `bench/sql/store/sift.sql` are unchanged. `store-json` adds one declared `items[]{s text}` path, cycling a string, JSON null and a missing member. The [raw runs](p12-format.tsv) include all timings and sizes.

| build | store build median (range), seconds | store-json build median (range), seconds |
|---|---|---|
| released 3.1.0 | 0.499 (0.459–0.532) | 0.858 (0.754–0.921) |
| 3.2 candidate | 0.650 (0.564–0.697) | 0.883 (0.836–0.900) |
| same 3.2 binary, setting 4 | 0.528 (0.497–0.578) | 0.923 (0.879–1.006) |
| same 3.2 binary, setting 6 | 0.484 (0.459–0.519) | 0.852 (0.796–0.990) |

Every run has exactly the same index size: 11,370,496 bytes (10.844 MiB) for store and 11,640,832 bytes (11.102 MiB) for store-json. Minor 6 adds no chunk bytes relative to minor 4; it certifies complete kinds. Store without declared JSON paths still writes minor 4. The released-build timing comparison includes other 3.2 changes; the paired setting comparison isolates the format choice and shows no consistent added build cost at this precision.
