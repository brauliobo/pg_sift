#!/usr/bin/env bash
# Downloads the public benchmark datasets, verifies them and converts them to COPY files.
#
#   bench/datasets/fetch.sh articles|places|all
#
# articles  Simple English Wikipedia (CC BY-SA 4.0), pages-articles dump, sha1 checked against the
#           published sums -> $DATA/articles.tsv (218k articles, 317 MB)
# places    GeoNames cities500 and cities15000 (CC BY 4.0) with country, region and feature names
#           -> $DATA/places-full/ and $DATA/places-quick/ (places.tsv + queries.tsv)
# Every download goes into its own new directory under $DOWNLOADS (default $SIFT_BENCH_ROOT/download),
# next to a SHA256SUMS file; it is untrusted input and is only read by the python3 -I converters.
# Dump date, GeoNames date and checksums are pinned in bench/versions.env.
set -euo pipefail
ROOT=${SIFT_BENCH_ROOT:-/tmp/sift-bench}
DATA=$ROOT/data DOWNLOADS=${DOWNLOADS:-$ROOT/download}
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../versions.env"
GEO=$GEONAMES_BASE
mkdir -p "$DATA" "$DOWNLOADS"

get() { # url dir: into a new directory, skipped when the file is already there
  local dir=$2 f=${1##*/}
  mkdir -p "$dir"
  [[ -s $dir/$f ]] || curl -fsS -m 3600 -o "$dir/$f" "$1"
}

articles() {
  local base=https://dumps.wikimedia.org/simplewiki/$WIKI_DATE dir=$DOWNLOADS/simplewiki-$WIKI_DATE f=simplewiki-$WIKI_DATE-pages-articles.xml.bz2
  get "$base/$f" "$dir"
  get "$base/simplewiki-$WIKI_DATE-sha1sums.txt" "$dir"
  (cd "$dir" && grep " $f\$" "simplewiki-$WIKI_DATE-sha1sums.txt" | sha1sum -c - && sha256sum "$f" >SHA256SUMS)
  [[ -z $WIKI_SHA1 || $(sha1sum "$dir/$f" | cut -d' ' -f1) == "$WIKI_SHA1" ]] || { echo "$f differs from the pinned sha1 $WIKI_SHA1" >&2; exit 1; }
  (cd "$DATA" && python3 -I "$HERE/wiki.py" "$dir/$f" "$DATA/articles.tsv")
}

places() {
  local dir=$DOWNLOADS/geonames-$(date +%Y%m%d) f
  [[ -d $dir ]] || for d in "$DOWNLOADS"/geonames-*; do [[ -f $d/cities500.zip ]] && dir=$d; done
  for f in cities500.zip cities15000.zip admin1CodesASCII.txt countryInfo.txt featureCodes_en.txt; do get "$GEO/$f" "$dir"; done
  (cd "$dir" && sha256sum cities500.zip cities15000.zip admin1CodesASCII.txt countryInfo.txt featureCodes_en.txt >SHA256SUMS &&
    unzip -o -q cities500.zip && unzip -o -q cities15000.zip)
  for t in full:cities500 quick:cities15000; do
    mkdir -p "$DATA/places-${t%%:*}"
    (cd "$DATA" && python3 -I "$HERE/places.py" "$dir" "$dir/${t##*:}.txt" "$DATA/places-${t%%:*}")
  done
  for f in cities500 cities15000; do
    local want=GEONAMES_SHA256_$f
    [[ -z ${!want} || $(sha256sum "$dir/$f.zip" | cut -d' ' -f1) == "${!want}" ]] ||
      echo "warning: $f.zip is not the file of the published results (GeoNames changes daily); expect slightly different numbers" >&2
  done
  echo "GeoNames files in $dir:"; cat "$dir/SHA256SUMS"
}

case ${1:-} in
  articles) articles ;;
  places) places ;;
  all) articles; places ;;
  *) sed -n '2,13p' "$0"; exit 1 ;;
esac
