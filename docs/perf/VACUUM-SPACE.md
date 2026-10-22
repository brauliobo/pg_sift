# VACUUM space reclamation

This note compares two builds on a private native cluster. The README section on updates, deletes and VACUUM (`bench/bench store section`, `docs/results/updates.json`) measures the lanes of one build in the store harness.

2026-10-20, PostgreSQL 18.6 / pg_sift 3.2 Unreleased. AMD Ryzen 7 9700X, 16 hardware threads; servers and clients pinned to two physical CPUs (1, 3), nice 19. Dedicated NVMe test storage. Native PostgreSQL transports reuse the store harness’s generator, DDL, load, UPDATE/DELETE batches, probes and concurrent update workloads. Baseline `451d529`; candidate built on `edb865d` plus the storage change. The final branch was rebased after main’s packaging, portability, test and complete-JSON-kinds updates. Store schemas have no JSON paths and retain their previous format minor; focused checks were rerun on that integrated head (`36b7999` plus this change). A final clean rebase picked up the shared UTF-8 emitter and analyzer index-lookup refactors; no further local tests were started after the shared-host slot was released.

Three independent fresh 1M-row loads per version. Each pass updates 1%, updates another 10%, deletes 5%, performs 20 probes, then runs the harness’s 0/1,000/10,000 rows/s workloads for 15 seconds each before VACUUM. Query latencies use 15 samples after 3 warmups per pass. Values below are the median of the three passes, with their minimum–maximum in parentheses. Raw per-segment/unit/page accounting and all samples are in [vacuum-space.json](../results/vacuum-space.json); the stored run and verification log is [vacuum-space.log](../results/vacuum-space.log).

The host was shared and busy: one-minute load at pass boundaries was 11.2 (10.5–13.7) for baseline, 7.6 (6.3–9.0) for candidate. The candidate medians show no update/delete throughput regression; several ranges overlap, and shared host load limits timing comparisons. They do not establish an isolated CPU speedup.

| Total table and indexes (MiB) | sift_col baseline | sift_col fixed | sift_table baseline | sift_table fixed |
|---|---:|---:|---:|---:|
| Fresh load | 143.0 (142.9–143.1) | 143.0 (143.0–143.1) | 192.8 (192.8–192.8) | 42.4 (42.4–42.4) |
| After changes and concurrent workload | 189.4 (189.4–189.4) | 189.4 (189.4–189.4) | 207.1 (206.9–207.4) | 78.8 (78.8–79.0) |
| After VACUUM | 200.3 (200.3–200.3) | 154.5 (154.5–154.5) | 67.1 (66.8–67.3) | 46.1 (46.0–46.3) |

| Inside the column/table file after VACUUM | sift_col baseline | sift_col fixed | sift_table baseline | sift_table fixed |
|---|---:|---:|---:|---:|
| Free MiB | 16.055 (16.055–16.070) | 0.008 (0.008–0.008) | 8.133 (8.133–8.148) | 0.008 (0.008–0.008) |

| Throughput / scan latency | sift_col baseline | sift_col fixed | sift_table baseline | sift_table fixed |
|---|---:|---:|---:|---:|
| update 1% (rows/s) | 72,547 (60,167–77,621) | 73,510 (71,589–77,072) | 62,118 (58,646–69,477) | 81,658 (73,447–87,032) |
| update 10% (rows/s) | 139,711 (121,053–153,573) | 185,603 (147,126–201,722) | 67,662 (58,441–69,158) | 81,440 (72,931–94,249) |
| delete 5% (rows/s) | 441,662 (296,333–625,431) | 482,777 (447,806–533,236) | 190,920 (166,909–213,422) | 258,726 (208,274–342,507) |
| Q02 after VACUUM (ms) | 11.493 (6.989–11.985) | 7.753 (4.747–8.311) | 0.643 (0.542–0.809) | 0.547 (0.423–0.547) |

## Accounting and mechanism

- Fresh baseline `sift_table`: 20,944 file blocks, 19,252 free blocks (150.406 MiB), and 1,692 used blocks (13.219 MiB). The directory’s column segment accounts for 1,685 blocks over 62 units; the remaining seven used blocks are metadata and buffers. Its primary-key and title indexes are counted separately in the total. The free count is real: obsolete write-buffer and intermediate-segment extents from COPY’s promotions/merges remain inside the file, with the compressed output above them. This is temporary rewrite storage left allocated, rather than a free-space measurement error.
- The previous column VACUUM performs one merge, can leave new versions in open buffers, and does not truncate the column file. Replaced extents carry a visibility horizon, so a rewrite in the same VACUUM cannot ordinarily reuse them. A small live tail, including an active buffer or liveness pages, can pin otherwise free space. The stored snapshots show which segments/units occupy the file and how many pages are free.
- VACUUM now promotes sufficiently filled column buffers and continues its column merge policy within a write allowance. Under a conditional, WAL-logged exclusive relation lock, released extents are immediately reusable. Relocation plans every allocation below its destination boundary, keeps LIVE runs contiguous, splits data/lane runs across smaller holes within the existing extent-directory limit, copies pages without changing formats, atomically publishes the replacement, and truncates the free tail. All copied pages share one remaining allowance across rounds. The plan leaves free-list metadata headroom and cannot substitute file extension for a failed reservation.
- After maintenance the candidate’s column file is 12.2 (12.2–12.2) MiB versus 31.5 (31.5–31.5) MiB baseline. The BM25 title index is 8.1 (8.1–8.2) versus 34.7 (34.7–34.7) MiB. The heap is still 112.6 (112.6–112.6) MiB (fresh 102.7 (102.7–102.7)); ordinary index VACUUM cannot return those interior heap holes. A heap rewrite is required to remove that component of total-size growth.
- Current main already reclaims much of the table’s post-churn tail, so its baseline post-VACUUM result is smaller than the historical 3.1.0 table in README. The fresh-load waste still reproduces. The fixed fresh table file is 13.2 (13.2–13.2) MiB, and after churn 13.5 (13.5–13.5) MiB, plus its indexes. The per-file remaining free-block counts are recorded for each repetition in the raw accounting.

## Verification and limits

The space regressions assert shrinkage, little remaining free space, successful refill, and disabled truncation. `store_space` tests a 1 MiB allowance across profitable successive rounds and a 40 MiB stream relocated into 16-block holes without growth. Negative controls restoring the budget reset copy 225 pages under a 128-page allowance, and the double subtraction of copy occupancy rejects a profitable front-hole relocation. Both assertions fail with the faulty behavior and pass with the corrected implementation.

Local checks: 19 focused regressions (including the integrated JSON-kinds regression); three VACUUM isolation specs; TAP 006/014 and the 14 column/table scenarios of TAP 015. The new scenarios flush WAL and kill the backend after immediate recycling, replacement publication, and lowering high_water before physical truncation, for both engines. Recovery validates rows, overflow values, unique ownership of referenced/free extents, orphan reclamation, and subsequent writes. Primary cursors skip reclamation without waiting and permit it after close. Standby cursors are cancelled by lock replay with feedback off and on; the column cursor is confirmed to use SiftAgg. A young table buffer is copied with its overflow bytes and MVCC version while an older snapshot of another relation prevents promotion. A tiny table with no free space does not cancel a standby snapshot. Generic WAL consistency checking is enabled. CI area registration, portable-source and neutral-name checks pass. PostgreSQL 17 and the full cross-platform matrix remain the GitHub CI gate.

Post-review repair checks reproduced a separate error path: a persistent writer lost its next row after another session published an open-buffer relocation and raised an ordinary ERROR before truncation. BM25, column and table variants now retain the row with unchanged physical file length. Publication sends immediate shared/local invalidation; tracked write targets return unpublished allocations on failure and transfer ownership when the directory changes. Five focused regressions, TAP 015/016 (20 scenarios), and the 2.x/3.0 compatibility fixtures (11 checks each) pass on PostgreSQL 18. The compatibility assertions preserve old formats, counts and liveness while allowing relocation to assign new generations; the mixed-column assertion checks the exact format multiset. The previous timing measurements were not rerun for this error-path repair. Before/after evidence is appended to the stored verification log.

`vacuum_truncate = false` disables the space step; tables also honor `VACUUM (TRUNCATE FALSE)`. `sift.vacuum_rewrite_limit = 0` disables relocation and retains ordinary free-tail truncation and the first column merge. Contending primary readers, parallel cleanup that cannot assign an xid, a LIVE run without a fitting contiguous destination, or an extent directory that would overflow can defer relocation; free pages remain reusable. No on-disk format changes are introduced.

README, BENCHMARKS and the canonical store result set remain unchanged: the native two-CPU partial pass is not a replacement for the published three-CPU Docker comparison. The harness has a render-only path, but these result sets have a different transport, budget and result schema; merging them into those cells would mislabel the environment.

Reproduce against separate private baseline and candidate clusters (the three store databases are disposable):

```sh
taskset -c 1,3 nice -n19 python3 bench/steps/vacuum_space.py \
  --port "$PORT" --data "$DATA" --out "$OUT" --label "$LABEL"
```
