# The compression lanes of the orders benchmark and their block.
# sift_none, encodings only, auto, lz4 and zstd side by side.
import json
from .results import md_table, size
from .notes import citus_options


CZ_LANES = ['sift_none', 'sift_enc', 'sift_auto', 'sift', 'sift_lz4', 'sift_zstd']


CZ_NAMES = {'sift_none': '`none`', 'sift_enc': 'encodings only', 'sift_auto': '`auto` (explicit)', 'sift': 'unset (the default)', 'sift_lz4': '`lz4`', 'sift_zstd': '`zstd:3`'}


def compression_block(R):
    tier = R.tiers[-1]
    t = R.meta['tiers'][tier]
    n = t['rows']
    s = t['servers']
    host = R.meta.get('host', {})
    lanes = [e for e in CZ_LANES if R.engines(tier, [e])]
    sz = lambda e, v='index orders_col': R.num(tier, e, variant=v, metric='size_mib')
    leaves = {e: {l['leaf']: l for l in json.loads(R.get(tier, e, '-', 'orders_col', 'col_leaves')['value'])} for e in lanes}
    out = [f'The `sift_col` index of the same {n:,} orders under each `compression` setting, {t["parts"]["load"]["date"][:10]} on {host.get("cpu", "?")}, PostgreSQL {s["pg"].split()[0]}, pg_sift {s["sift_sift"]} linked with `{s["sift_libs"]}`; '
           'size and build time of the index alone, every setting read back from the server that ran it.', '']
    head = ['`compression` of the `sift_col` index', '`reloptions`', 'index size', 'build time', 'vs `none`']
    body = [[CZ_NAMES[e], f'`{R.get(tier, e, "-", "orders_col", "reloptions")["value"]}`', f'{sz(e):,.1f} MiB', f'{R.num(tier, e, variant="build orders_col", metric="build_s"):.1f} s',
             f'{sz(e) / sz("sift_none"):.2f}x'] for e in lanes]
    out += md_table(head, body, ['---', '---', '---:', '---:', '---:']) + ['']
    out += [f'**Takeaway:** the default (`compression` unset = `auto`) is {sz("sift"):,.1f} MiB, {sz("sift") / sz("sift_none"):.2f}x of `none` ({sz("sift_none"):,.1f} MiB), and `zstd:3` is {sz("sift_zstd"):,.1f} MiB; '
            f'the build time stays within {min(R.num(tier, e, variant="build orders_col", metric="build_s") for e in lanes):.1f}-{max(R.num(tier, e, variant="build orders_col", metric="build_s") for e in lanes):.1f} s.', '', '**Caveats**', '']
    if 'sift_auto' in lanes and 'sift' in lanes:
        same = all(leaves['sift'][k]['bytes'] == leaves['sift_auto'][k]['bytes'] for k in leaves['sift'])
        enc = 'sift_enc' in lanes and all(leaves['sift'][k]['bytes'] == leaves['sift_enc'][k]['bytes'] for k in leaves['sift'])
        lz = 'sift_lz4' in lanes and all(leaves['sift'][k]['bytes'] == leaves['sift_lz4'][k]['bytes'] for k in leaves['sift'])
        out += [f'- The chunk bytes of every column ({"identical" if same else "NOT identical"} to an explicit `auto`, {"identical" if enc else "different"} to the encodings alone, {"identical" if lz else "different"} to `lz4`; `sift.col_leaves`) '
                'show that `auto` layered no LZ4 or zstd over the encodings on this data (the cost rule of [docs/perf/COMPRESSION.md](perf/COMPRESSION.md) accepts a layer only when its decode time is worth the bytes); no SQL function reports the layer chunk by chunk, so it is read from the byte counts.']
    qs = R.queries(tier)
    spread = [max(v) / min(v) for q in qs if len(v := [R.num(tier, e, q, 'warm', 'latency_ms') for e in lanes]) and all(v)]
    out += [f'- Warm time of a question across the {len(lanes)} settings: the slowest against the fastest {min(spread):.2f}x to {max(spread):.2f}x, in no fixed order, so the differences are run-to-run noise (the medians are in `store-compression.json`).']
    tbl = [(p, R.num(tier, 'sift_table', variant='codec ' + p, metric='size_mib'), R.num(tier, 'sift_table', variant='codec ' + p + ' free', metric='size_mib')) for p in ('none', 'auto', 'lz4', 'zstd:3')]
    others = [f'pg_sift `sift_table` with `sift.table_compression` = {p}: {size(v, f)}' for p, v, f in tbl if v is not None]
    others += [f'ClickHouse, {k}: {size(R.num(tier, "ch", variant=f"codec {k}", metric="size_mib"))}' for k in ('default (LZ4)', 'ZSTD(3)') if R.num(tier, 'ch', variant=f'codec {k}', metric='size_mib') is not None]
    others += [f'Elasticsearch, {k}: {size(R.num(tier, "es", variant=f"codec {k}", metric="size_mib") if k != "default (LZ4)" else R.num(tier, "es", metric="size_mib"))}' for k in ('default (LZ4)', 'best_compression') if R.engines(tier, ['es'])]
    others += [f'PostgreSQL (heap, btree, GIN): {size(R.num(tier, "pg", metric="size_mib"))}'] if R.engines(tier, ['pg']) else []
    others += [f'Citus columnar ({citus_options(R, tier)}), primary key: {size(R.num(tier, "citus", metric="size_mib"))}'] if R.engines(tier, ['citus']) else []
    out += ['- Table and indexes on disk of the other layouts and systems with their own compression settings: ' + '; '.join(others) + '.']
    out += ['', 'Raw results: [results/store-compression.json](results/store-compression.json); reproduce with `bench/bench store all --sizes 1000000 --compression --docs`.']
    return out
