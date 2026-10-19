# Field weights and facets

What other systems offer for field weights and facets, what pg_sift had, what 3.1 adds and what it leaves out, with the reasons. The comparison
rows S6 and S7 in [COMPARISON.md](COMPARISON.md) follow this page; the syntax is in [USAGE.md](USAGE.md).

- [Research](#research)
- [Gap analysis](#gap-analysis)
- [Design](#design)
- [Which aggregate shapes run in SiftAgg](#which-aggregate-shapes-run-in-siftagg)
- [Not done, and why](#not-done-and-why)
- [Format and compatibility](#format-and-compatibility)
- [Tests](#tests)

## Research

Read on 2026-10-19 from each project's own documentation. A statement marked UNVERIFIED was not found on a documentation page.

### Field weights

| System | What it offers |
|---|---|
| PostgreSQL full-text | `setweight(tsvector, 'A'..'D')` at index time; `ts_rank(weights, ...)` takes four weights at query time; one text-search configuration per `to_tsvector` call. Not BM25 ([textsearch-controls](https://www.postgresql.org/docs/current/textsearch-controls.html)). |
| pg_search | A boost per column predicate at query time, a tokenizer per field, `k1` and `b` per field. That a query sums boosted predicates is UNVERIFIED; BM25F is UNVERIFIED ([docs](https://www.paradedb.com/docs/)). |
| Elasticsearch | `fields: ["title^3", "body"]` in `multi_match`. `best_fields` takes the best field, `most_fields` sums the fields, `cross_fields` blends the term statistics, `combined_fields` is BM25F-like (boosts of at least 1, one analyzer for all fields). Also `field:term` in `query_string`, an analyzer per field, phrases inside a field ([multi_match](https://www.elastic.co/docs/reference/query-languages/query-dsl/query-dsl-multi-match-query), [combined_fields](https://www.elastic.co/docs/reference/query-languages/query-dsl/query-dsl-combined-fields-query), [query_string](https://www.elastic.co/docs/reference/query-languages/query-dsl/query-dsl-query-string-query)). |
| Lucene | `CombinedFieldQuery`: BM25F-like term and length statistics combined with the field weights; experimental ([javadoc](https://lucene.apache.org/core/10_0_0/sandbox/org/apache/lucene/sandbox/search/CombinedFieldQuery.html)). |
| Typesense | `query_by` with `query_by_weights` ([search](https://typesense.org/docs/latest/api/search.html)). |
| Meilisearch | The order of the searchable attributes only, no numeric weight ([attribute ranking order](https://www.meilisearch.com/docs/learn/relevancy/attribute_ranking_order)). |
| ClickHouse | None found in the text index docs ([text indexes](https://clickhouse.com/docs/engines/table-engines/mergetree-family/textindexes)). |

What a field-weight feature needs to be called complete (the line used for the comparison): weights on two or more fields in one query; a field
named in the query text for terms and for phrases; a documented rule for combining the fields; length normalisation per field; a highlight per
field; an analyzer per field. BM25F is what Elasticsearch `combined_fields` and Lucene offer, and is a plus rather than part of that line.

### Facets

| System | What it offers |
|---|---|
| Elasticsearch | `terms` (`size`, `sum_other_doc_count`, `missing`, `order`), `range`, `histogram`, `date_histogram` (empty buckets filled by default), `filters`, several aggregations in one request; multi-select with `post_filter` plus a filter aggregation ([terms](https://www.elastic.co/docs/reference/aggregations/search-aggregations-bucket-terms-aggregation), [date_histogram](https://www.elastic.co/docs/reference/aggregations/search-aggregations-bucket-datehistogram-aggregation), [post_filter](https://www.elastic.co/docs/reference/elasticsearch/rest-apis/filter-search-results)). |
| Solr | Field, range and query facets; `{!ex=tag}` excludes a filter from its own facet, the canonical multi-select ([faceting](https://solr.apache.org/guide/solr/latest/query-guide/faceting.html)). |
| pg_search | `pdb.agg` aggregates: terms with `sum_other_doc_count`, `date_histogram`; exact by default. Range and histogram pages were not read (UNVERIFIED). `FILTER (WHERE ...)` gives a filter per aggregate, but no multi-select pattern is documented. |
| Typesense | `facet_by`, `max_facet_values`, numeric ranges, sort by value ([search](https://typesense.org/docs/latest/api/search.html)). |
| Meilisearch | `facetDistribution` and `facetStats` (min and max); no ranges or date buckets ([search](https://www.meilisearch.com/docs/reference/api/search/search-with-post)). |

The line used for the comparison: terms with a top N; the count of the other values and a statement on exactness; range, histogram and date
buckets; several facets in one request; a multi-valued field counted once per value; every facet computed over the same filtered set; a facet
that ignores its own selection.

## Gap analysis

### Facets

`sift.facets(index, facet_index, query, limit)` (kept as it is) counts the matches per value of one column from the bitmaps of a
`analyzer = keyword` index. It lacked, against the line above:

- numeric, date, boolean and array columns (the keys are text; integers need an expression index and a cast);
- range, histogram and date buckets;
- several facets in one call (each facet is a call and a keyword index);
- the count of the values past the limit, and the NULL bucket;
- a multi-select (the selection of a facet applied to the others but not to itself);
- a plan visible in `EXPLAIN` (it is a function);
- tables under row-level security.

### Fields

The `fields` option builds one key from sections separated by `chr(29)`, names the first ones (up to 14) and adds a term per section and word.
At query time `name:word`, `name:{a b}^3` and `name:(a b)` search the section. What it lacked:

- weights that a query chooses: the boost went on every term by hand, and the section terms were the only ones that could be weighed;
- the key written without `chr(29)`;
- length normalisation per field: the section terms have their own document frequency and term frequency but use the length and average
  length of the whole document;
- phrases, regular expressions and highlights inside a field: the section terms carry no positions;
- a word of exactly 255 bytes gets no section term;
- an analyzer per field;
- a multicolumn index. The planner, the estimator, the count scan, the aggregate extraction, `sift.facets` and the SQLite and MariaDB hosts all
  match a key by `indnatts = 1` and a single expression.

## Design

### Facets: `sift.facet_counts`

```sql
SELECT * FROM sift.facet_counts('items', 'title', 'wireless',
  '[{"expr":"brand","size":10,"missing":true},
    {"expr":"price","edges":[100,300]},
    {"expr":"added","unit":"month","fill":true},
    {"expr":"tags","array":true}]',
  'active', hits => true);
-- (table, key expression, SiftQL query, facets, SQL filter, hits): facet, kind, value, lo, hi, count
```

`facets` is a JSON array; an element is a column expression (a terms facet) or an object:

| Key | Meaning |
|---|---|
| `name` | label of the facet in the output (default: `expr`) |
| `expr` | column expression of any type |
| `type` | `terms` (default; `range` with `edges`, `histogram` with `interval`, `date_histogram` with `unit`) |
| `size`, `order` | top N of a terms facet; `count` (default), `count asc`, `value`, `value desc` |
| `missing` | adds the bucket of the NULL values |
| `array` | the expression is an array: each element counts once per row |
| `edges` | range bounds: buckets `[,e1) [e1,e2) ... [en,)` |
| `interval`, `offset` | histogram width and origin |
| `unit` | `second`, `minute`, `hour`, `day`, `week`, `month`, `quarter`, `year` (`date_trunc` on the `timestamp`) |
| `fill` | returns the empty buckets between the first and the last; more than 10000 buckets is an error |
| `selected` | the values the user chose (bucket numbers of a range, bucket starts of a histogram) |
| `filter` | a SQL condition the user chose in this facet |

Output: one row per bucket, `kind` is `value`, `bucket`, `other` (the values past `size`: `lo` is how many, `count` the sum of their counts),
`missing`, or `hits` (facet NULL; the rows left after every selection).

Rules:

- **Exact.** Every count is the `count(*)` of the same snapshot; there is no estimate. The other-values row is the exact total of the rest.
- **Multi-select.** The scan of a facet has the search, the shared `filter` and the selections of the other facets, not its own.
- **One statement.** `sift.facet_sql(...)` returns the SQL text; `facet_counts` runs it as the caller (permissions and row-level security apply).
- **Scans.** Each key facet is a `MATERIALIZED` `GROUP BY` scan of its own. All the range facets with the same `WHERE` share one scan of
  `count(*) FILTER (WHERE ...)`. An array facet unnests in a lateral `SELECT DISTINCT`, so a repeated element counts once per row.
- **Plan.** With a `sift_col` index on the columns, each scan is a `Custom Scan (SiftAgg)` with the search as the row selection and shows in
  `EXPLAIN` of `sift.facet_sql(...)`. Without one it is the ordinary plan, with the same result.

Why a scan per facet and not one `GROUPING SETS`: the first version used `GROUPING SETS`. With four expression keys the planner chose a
`SiftScan` and a `HashAgg` (54-163 ms on a million rows), and a single set is not pushed down. A scan per facet with `FILTER` aggregates for the
ranges was 7-13 times faster. At 1,000,000 rows and four facets (category, top 10 brands, two price ranges, month):

| Search | `sift.facet_counts` | the same four `GROUP BY`s with `sift.enable_agg = off` |
|---|---|---|
| `wireless` (100,000 matches) | 10.4 ms | 140 ms |
| `wireless OR wired OR portable OR compact` | 12.7 ms | 310 ms |
| `wireless headphones black` (selective) | 2.9 ms | 4.2 ms |

The planner's choice between the aggregate pushdown and a `SiftScan` with a normal aggregate flips with a cold or warm term-statistics cache
(`sift_estimate_cached`); the results are the same.

### Fields: `sift.sections` and `sift.field_query`

```sql
CREATE INDEX ON items USING sift (sift.sections(title, body, tags)) WITH (fields = 'title,body,tags');
SELECT id FROM items
WHERE sift.sections(title, body, tags) @@> sift.field_query('wireless headphones', 'title^3, body, tags^0.5')
ORDER BY sift.score(ctid) DESC LIMIT 10;
```

- `sift.sections(VARIADIC text[])` joins the sections with `chr(29)` (a NULL section is empty); the same expression is the key of the index and the
  left side of `@@>`.
- `sift.field_query(query, fields [, index])` rewrites a SiftQL query: every word, prefix, wildcard and fuzzy term outside a `name:` qualifier is
  searched in each listed field, with its boost times the weight of the field (`^w`, above 0 and up to 10000; no weight is 1). A row scores the
  sum of its fields' scores; the match set is that of the unweighted query on those fields. With `index`, the field names are checked against
  the index (`42601` for an unknown field, `22023` for an index with no `fields`).
- Phrases, regular expressions and the terms of a `NOT` are not scoped to a field (the section terms have no positions); they match the whole key.
- Top-k with weights returns the full ranking's top k (tested for 60 combinations of weights from 0.01 to 10000), and the scores equal a SQL
  per-field BM25 reference.

Combination rule: the sum of per-field BM25 scores, each field's term with its own `idf` and `tf` and the document's length normalisation.
This is Elasticsearch `most_fields` with boosts, not `combined_fields`.

## Which aggregate shapes run in SiftAgg

Probed with the search as the row selection (a `sift_col` index on the grouped columns):

| Shape | Result |
|---|---|
| terms on integer, text and boolean columns | `SiftAgg` |
| `date_trunc(...)` and `width_bucket(...)` keys, `CASE` keys | `SiftAgg` |
| `count(*) FILTER (WHERE ...)`, `min`, `max`, `IN` lists | `SiftAgg` |
| `GROUPING SETS` of up to 4 keys, and the empty set | `SiftAgg`, but the planner often prefers `SiftScan` plus `HashAgg` (see above) |
| `unnest` of an array column, `jsonb ->> 'k'` without `paths` | ordinary plan |
| an `OR` between the search and another predicate | ordinary plan |
| one grouping set with `GROUPING()` | ordinary plan |

`sift.facet_counts` is built from the first group only, so its scans run in `SiftAgg` when the columns are in the index. An array facet, or a
filter on a column outside the index, makes that one scan an ordinary plan; the counts stay exact.

## Not done, and why

| Not done | What it needs |
|---|---|
| A multicolumn BM25 index (`CREATE INDEX ... USING sift (title, body)`) | Key matching assumes one key (`indnatts = 1`) in the planner hook, the estimator, the count scan, the aggregate extraction, `sift.facets`, and the SQLite and MariaDB hosts. Each would learn a list of keys and `@@>` which key it means. `sift.sections` gives the same result through one key and one planner path, so the change was declined for 3.1. |
| BM25F (one combined score with per-field term frequency and length) | A document length per field: the docmap rows, the block-max metadata and the scoring kernels change, so the segment format changes. It needs the compatibility work (side-by-side read, upgrade tests, merge and `VACUUM` rewrite, a `REINDEX` hint) and a format id coordinated with the other 3.1 branches. Not attempted here. |
| An analyzer per field | Part of the same change: the section terms are made by the index's one analyzer. |
| Phrase and regular expression inside a field | The section terms have no positions. A recheck that knows the section borders would be needed, and a position format change for the index scan. |
| Highlight per field | `sift.highlight` works on the text of one key; it would need the border of each section. |
| Array facets and filters off the column index in `SiftAgg` | `SiftAgg` reads one value per row from `sift_col`; an array element per row is a different kernel. They run, exact, as the ordinary plan. |
| `sift.facets` on other than keyword text keys | Left as it is: it reads bitmaps of whole values. `sift.facet_counts` covers the other types. |

## Format and compatibility

No on-disk change. The BM25 segment format (3 with the packed format on `feat/v31-bm25size`), the BM25 metapage minor and the column format
minor (4 on `feat/v31-cube2`) are untouched. The extension SQL moves to 3.1: `ALTER EXTENSION pg_sift UPDATE` runs `pg_sift--3.0--3.1.sql`, which
adds `sift.facet_cap`, `sift.facet_part`, `sift.facet_sql`, `sift.facet_counts`, `sift.sections` and `sift.field_query`, and the C function
`sift_field_query` of the library. No index needs a rebuild.

`pg_sift--3.0--3.1.sql` and `pg_sift--3.1.sql` also exist on other 3.1 branches; the merge of this branch needs one script with the blocks of each.

## Tests

- `facet_counts` (regress): 24 cases against hand-written oracles in every state (build, chunks, insert, update, delete, own transaction,
  rollback, `VACUUM`, promote, merge, parallel), with `SiftAgg` forced and with `sift.enable_agg = off`; a table with no column index; a
  `sift_table`; a role with limited permissions; the errors.
- `facet_counts_writes` (isolation): the counts against the `GROUP BY` while another session writes.
- `h_search_facet_scans`, `h_search_facet_counts` (column differential fixtures).
- `field_weights` (regress): weighted scores against the per-field SQL reference, top-k against the full ranking for 60 weight combinations,
  match sets against the unweighted query on the index and on a sequential scan.
- `extension_lifecycle` (TAP): the 3.0 to 3.1 update gives the same objects as a fresh 3.1 install.
