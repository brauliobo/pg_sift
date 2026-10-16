#!/usr/bin/env bash
# Usage: bench/steps/fetch.sh [sift|rum|pg_search|bm25|elasticsearch|tantivy ...]
# (default: sift rum pg_search). Versions are pinned in bench/versions.env.
# Builds or unpacks the engines under $ROOT/engines/<name>, never system-wide: the PGXS extensions (sift from this
# checkout, rum, pg_textsearch) are installed with DESTDIR into <name>/root, the ParadeDB pg_search package is
# unpacked there; the cluster finds every */root (bench/steps/lib.sh). Downloads land in <name>/dl.
# SIFT_SRC=worktree (default) builds the tracked files of this checkout, SIFT_SRC=head the last commit.
# BM25_PKG: the pgx-bm25 Arch package to unpack. Builds run with nice and JOBS (4) jobs.
. "$(dirname "$0")/lib.sh"

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

fetch_rum() { pgxs_release rum "https://github.com/postgrespro/rum/archive/refs/tags/$RUM_VERSION.tar.gz"; }

fetch_bm25() { mkdir -p "$ENG/bm25/root"; bsdtar -xf "${BM25_PKG:-$HOME/pkg/pgx-bm25/pgx-bm25-1.0.0-1-x86_64.pkg.tar.zst}" -C "$ENG/bm25/root"; }

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

fetch_elasticsearch() {
  local tgz; tgz=$(dl elasticsearch "https://artifacts.elastic.co/downloads/elasticsearch/elasticsearch-$ES_VERSION-linux-x86_64.tar.gz")
  [[ -d $ENG/elasticsearch/es ]] || { mkdir -p "$ENG/elasticsearch/es"; tar -xzf "$tgz" -C "$ENG/elasticsearch/es" --strip-components=1; }
}

fetch_tantivy() { (cd "$BENCH/xbench" && CARGO_TARGET_DIR=$ROOT/target nice -n 19 cargo build --release -j "$JOBS" -q); }

mkdir -p "$BUILD"
for t in ${*:-sift rum pg_search}; do "fetch_$t"; log "fetched $t"; done
