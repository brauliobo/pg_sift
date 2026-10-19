#!/usr/bin/env bash
# Downloads the public benchmark datasets, verifies them and converts them to COPY files.
#
#   bench/datasets/fetch.sh articles|places|all
#
# articles  Simple English Wikipedia (CC BY-SA 4.0), pages-articles dump, sha1 checked against the
#           published sums -> $DATA/articles.tsv.gz (218k articles, gzip of 317 MB of COPY text)
# places    GeoNames cities500 and cities15000 (CC BY 4.0) with country, region and feature names
#           -> $DATA/places-full/ and $DATA/places-quick/ (places.tsv + queries.tsv)
# Every download goes into its own new directory under $DOWNLOADS (default $BENCH_STORE/download), next to a SHA256SUMS file; it is
# untrusted input and is only read by the python3 -I converters. Once a dataset is derived the download is removed (KEEP_DOWNLOADS=1
# keeps it) and a stamp next to the dataset records what it was made from (the pinned dump or the sha256 of the GeoNames files, and
# the converter): a later run with the same stamp downloads and converts nothing (REFRESH_DATASETS=1 does both again). `all` converts the
# two datasets at the same time. Dump date, GeoNames date and checksums are pinned in bench/versions.env.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../steps/lib.sh"   # ROOT, STORE, DATA, DOWNLOADS and `download`
GEO=$GEONAMES_BASE
mkdir -p "$DATA" "$DOWNLOADS"

get() { # url dir: into a new directory, skipped when the file is already there
  local dir=$2 f=${1##*/}
  mkdir -p "$dir"
  download "$1" "$dir/$f"
}
current() { [[ -z ${REFRESH_DATASETS:-} && -f $1 && $(<"$1") == "$2" ]]; }   # stamp wanted-content: nothing to do

articles() {
  local base=https://dumps.wikimedia.org/simplewiki/$WIKI_DATE dir=$DOWNLOADS/simplewiki-$WIKI_DATE f=simplewiki-$WIKI_DATE-pages-articles.xml.bz2
  local stamp
  stamp="simplewiki-$WIKI_DATE sha1 ${WIKI_SHA1:-published} $(sha256sum "$HERE/wiki.py" | cut -c1-16)"
  if current "$DATA/articles.stamp" "$stamp" && [[ -s $DATA/articles.tsv.gz ]]; then echo "articles: $DATA/articles.tsv.gz is up to date ($stamp)"; return; fi
  get "$base/$f" "$dir"
  get "$base/simplewiki-$WIKI_DATE-sha1sums.txt" "$dir"
  (cd "$dir" && grep " $f\$" "simplewiki-$WIKI_DATE-sha1sums.txt" | sha1sum -c - && sha256sum "$f" >SHA256SUMS)
  [[ -z $WIKI_SHA1 || $(sha1sum "$dir/$f" | cut -d' ' -f1) == "$WIKI_SHA1" ]] || { echo "$f differs from the pinned sha1 $WIKI_SHA1" >&2; exit 1; }
  (cd "$DATA" && python3 -I "$HERE/wiki.py" "$dir/$f" "$DATA/articles.new.tsv.gz" && mv "$DATA/articles.new.tsv.gz" "$DATA/articles.tsv.gz")
  rm -f "$DATA/articles.tsv"   # the plain file of an earlier setup
  echo "$stamp" >"$DATA/articles.stamp"
  [[ -n ${KEEP_DOWNLOADS:-} ]] || rm -rf "$dir"
}

places() {
  local f d dir sums
  dir=$DOWNLOADS/geonames-$(date +%Y%m%d)
  [[ -d $dir ]] || for d in "$DOWNLOADS"/geonames-*; do [[ -f $d/cities500.zip ]] && dir=$d; done
  if [[ -z ${REFRESH_DATASETS:-} && -f $DATA/places.stamp && -s $DATA/places-full/places.tsv && -s $DATA/places-quick/places.tsv ]] &&
     [[ $(tail -1 "$DATA/places.stamp") == "converter $(sha256sum "$HERE/places.py" | cut -c1-16)" ]]; then
    echo "places: $DATA/places-* are up to date (GeoNames files of $(head -1 "$DATA/places.stamp" | cut -d' ' -f1))"; return
  fi
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
  sums=$(cat "$dir/SHA256SUMS")
  echo "GeoNames files of $(basename "$dir"):"; echo "$sums"
  { echo "$(basename "$dir" | sed 's/geonames-//') GeoNames files (sha256):"; echo "$sums"; echo "converter $(sha256sum "$HERE/places.py" | cut -c1-16)"; } >"$DATA/places.stamp"
  [[ -n ${KEEP_DOWNLOADS:-} ]] || rm -rf "$dir"
}

both() { # the two datasets at the same time; their messages one after the other
  local log pa pp rc=0
  log=$(mktemp -d)
  articles >"$log/articles" 2>&1 & pa=$!
  places >"$log/places" 2>&1 & pp=$!
  wait "$pa" || rc=1
  wait "$pp" || rc=1
  cat "$log/articles" "$log/places"; rm -rf "$log"
  return $rc
}

case ${1:-} in
  articles) articles ;;
  places) places ;;
  all) both ;;
  *) sed -n '2,13p' "$0"; exit 1 ;;
esac
