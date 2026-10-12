# Tantivy 0.26 in-process via bench/xbench (default tokenizer: split on non-alphanumerics,
# lowercase, no stemming), merged to one segment, mmap directory. Queries are term/phrase
# queries built from the trace terms; the whole driver process runs inside the CPU/RAM budget.
EXTERNAL=tantivy
TV_DIR=$ROOT/engines/tantivy/$TIER
URL=$TV_DIR/index
XB_ARGS=()
REQUIRES_PATH=$ROOT/target/release/xbench

x_start() { :; }
x_stop() { rm -rf "$TV_DIR"; }
x_pid() { :; }
x_build() { rm -rf "$TV_DIR"; limited "$XBENCH" tantivy-index "$URL" "$1" --threads 4 --mem-mb 4000 >/dev/null; }
x_size() { du -sb "$URL" | cut -f1; }

RENDER=$(cat <<'SQL'
CREATE FUNCTION pg_temp.x_req(kind text, mode text, terms text[], cutoff int) RETURNS text LANGUAGE sql AS
  $$ SELECT concat_ws(E'\t', kind, mode, cutoff, array_to_string(terms, ' ')) $$;
CREATE FUNCTION pg_temp.x_write(op text, doc jsonb) RETURNS text LANGUAGE sql AS
  $$ SELECT op || E'\t' || doc::text $$;
SQL
)
