#!/usr/bin/env bash
# Usage: bench/steps/fetch.sh [sift|sift_ref|rum|pg_search|pg_textsearch|bm25|elasticsearch|tantivy ...]
# (default: sift rum pg_search pg_textsearch). Versions are pinned in bench/versions.env.
# Builds or unpacks the engines under $ROOT/engines/<name>, never system-wide: the PGXS extensions (sift from this
# checkout, rum, pg_textsearch) are installed with DESTDIR into <name>/root, the ParadeDB pg_search package is
# unpacked there; the cluster finds every */root (bench/steps/lib.sh). Downloads land in <name>/dl.
# SIFT_SRC=worktree (default) builds the tracked files of this checkout, SIFT_SRC=head the last commit.
# sift_ref: sift built from SIFT_REF (a git ref of this repository, default the tag in versions.env) under the extension name
# pg_sift_ref, the reference of `bench/bench regress`. bm25 (pgx-bm25): BM25_PKG, the Arch package to unpack, or BM25_SRC, its source
# tarball to build with PGXS. Builds run with nice and JOBS (4) jobs.
. "$(dirname "$0")/lib.sh"

# shellcheck disable=SC2153  # ROOT comes from lib.sh
BUILD=$ROOT/build

dl() { # engine url [sha256]: downloads once into engines/<engine>/dl, prints the path
  local f=$ENG/$1/dl/${2##*/}
  mkdir -p "$ENG/$1/dl"
  [[ -s $f ]] || curl -fsSL -o "$f" "$2"
  [[ -z ${3:-} ]] || echo "$3  $f" | sha256sum -c --quiet - >&2
  echo "$f"
}
pgxs_install() { # engine srcdir: build and install into engines/<engine>/root
  nice -n 19 make -s -C "$2" USE_PGXS=1 -j"$JOBS" && make -s -C "$2" USE_PGXS=1 DESTDIR="$ENG/$1/root" install
}
pgxs_release() { # engine url [sha256]: a PGXS extension from a release tarball
  local src=$BUILD/$1
  rm -rf "$src" "$ENG/$1/root"; mkdir -p "$src"
  tar -xzf "$(dl "$@")" -C "$src" --strip-components=1
  pgxs_install "$1" "$src" >/dev/null
}

# built tree differs from the last commit: only for SIFT_SRC=worktree
sift_dirty() {
  [[ ${SIFT_SRC:-worktree} == head ]] || git -C "$REPO" diff --quiet HEAD -- . ':!bench' ':!docs' ':!packaging' 2>/dev/null || echo "+uncommitted changes"
}

fetch_sift() {
  local src=$BUILD/sift
  rm -rf "$src" "$SIFT_ROOT"; mkdir -p "$src"
  if [[ ${SIFT_SRC:-worktree} == head ]]; then git -C "$REPO" archive HEAD | tar -x -C "$src"
  else (cd "$REPO" && git ls-files -z -- . ':!bench' ':!docs' ':!packaging' | tar --null -T - -cf -) | tar -x -C "$src"; fi
  pgxs_install sift "$src" >/dev/null
  echo "$(git -C "$REPO" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)$(sift_dirty)" >"$SIFT_ROOT/.build-info"
}

fetch_rum() { pgxs_release rum "https://github.com/postgrespro/rum/archive/refs/tags/$RUM_VERSION.tar.gz" "$RUM_SHA256"; }

fetch_pg_textsearch() { pgxs_release pg_textsearch "https://github.com/timescale/pg_textsearch/archive/refs/tags/v$PG_TEXTSEARCH_VERSION.tar.gz" "$PG_TEXTSEARCH_SHA256"; }

# A second sift in the same cluster: the extension is renamed (control file, scripts, library, and the library names the C code hands
# to the server for its parallel build and maintenance workers), the SQL it exposes is not.
fetch_sift_ref() {
  local src=$BUILD/sift_ref root=$ENG/sift_ref/root commit f
  commit=$(git -C "$REPO" rev-parse --verify --short=12 "$SIFT_REF^{commit}") || { echo "sift_ref: no git ref $SIFT_REF in $REPO" >&2; return 1; }
  rm -rf "$src" "$root"; mkdir -p "$src"
  git -C "$REPO" archive "$SIFT_REF" | tar -x -C "$src"
  for f in "$src"/pg_sift.control "$src"/pg_sift--*.sql; do mv "$f" "${f/pg_sift/pg_sift_ref}"; done
  sed -i -e 's/^EXTENSION = pg_sift$/EXTENSION = pg_sift_ref/' -e 's/^MODULE_big = pg_sift$/MODULE_big = pg_sift_ref/' \
    -e 's/wildcard pg_sift--/wildcard pg_sift_ref--/' "$src/Makefile"
  sed -i "s,/pg_sift',/pg_sift_ref'," "$src/pg_sift_ref.control"
  sed -i 's/"pg_sift"/"pg_sift_ref"/' "$src"/src/*.c
  pgxs_install sift_ref "$src" >/dev/null
  echo "$SIFT_REF ($commit)" >"$root/.build-info"
}

fetch_bm25() {
  local root=$ENG/bm25/root src=$BUILD/bm25
  rm -rf "$root"; mkdir -p "$root"
  if [[ -n ${BM25_PKG:-} ]]; then bsdtar -xf "$BM25_PKG" -C "$root"
  elif [[ -n ${BM25_SRC:-} ]]; then
    rm -rf "$src"; mkdir -p "$src"
    tar -xzf "$BM25_SRC" -C "$src" --strip-components=1
    pgxs_install bm25 "$src" >/dev/null
  else echo "bm25: set BM25_PKG (the pgx-bm25 Arch package) or BM25_SRC (its source tarball)" >&2; return 1; fi
}

fetch_pg_search() {
  local deb data root=$ENG/pg_search/root
  deb=$(dl pg_search "https://github.com/paradedb/paradedb/releases/download/v$PARADEDB_VERSION/postgresql-$PG_MAJOR-pg-search_$PARADEDB_VERSION-1PARADEDB-noble_amd64.deb")
  data=$(ar t "$deb" | grep '^data\.tar')
  rm -rf "$root"; mkdir -p "$root"
  case $data in
    *.xz)  ar p "$deb" "$data" | tar -xJf - -C "$root" ;;
    *.zst) ar p "$deb" "$data" | tar --zstd -xf - -C "$root" ;;
    *.gz)  ar p "$deb" "$data" | tar -xzf - -C "$root" ;;
  esac
}

fetch_elasticsearch() { # nothing to download: the system package is the engine (adapter: engines/elasticsearch.sh), and xbench drives it
  local home=${ES_HOME:-/usr/share/elasticsearch}
  [[ -x $home/bin/elasticsearch ]] || { echo "elasticsearch: $home/bin/elasticsearch is missing: install the system package (pacman -S elasticsearch)" >&2; return 1; }
  fetch_xbench
}

# the load generator of the engines outside Postgres (and Tantivy's driver)
fetch_xbench() { (cd "$BENCH/xbench" && CARGO_TARGET_DIR=$ROOT/target nice -n 19 cargo build --release -j "$JOBS" -q); }
fetch_tantivy() { fetch_xbench; }

mkdir -p "$BUILD"
for t in ${*:-sift rum pg_search pg_textsearch}; do "fetch_$t"; log "fetched $t"; done
