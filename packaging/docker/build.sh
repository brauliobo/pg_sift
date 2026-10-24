#!/usr/bin/env bash
# Build the pg_sift container images locally; no CI, no registry. From any directory:
#   packaging/docker/build.sh [-e] [PG_TAG...]     PG_TAG: a tag of the official postgres image, default 17 18 (19rc1, and 19 once the tag exists)
#   JOBS=2 packaging/docker/build.sh 18            compile with 2 jobs (default 4)
# For each tag it makes pg_sift:pg<MAJOR> and pg_sift:<extension version>-pg<MAJOR>; with -e also pg_sift-extension:pg<MAJOR> (the
# image-volume layout, docs/CONTAINERS.md). IMAGE_REPO=ghcr.io/OWNER/pg_sift renames the repository (then `docker push` is yours to run).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VER=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$REPO/pg_sift.control")
NAME=${IMAGE_REPO:-pg_sift}
ext=
[[ ${1:-} == -e ]] && { ext=1; shift; }
tags=("$@"); (( ${#tags[@]} )) || tags=(17 18)

build() { # build TARGET REPOSITORY PG_TAG MAJOR: the build log is shown only when the build fails
	local log; log=$(mktemp)
	docker build -f "$REPO/packaging/docker/Dockerfile" --target "$1" --build-arg PG_TAG="$3" --build-arg JOBS="${JOBS:-4}" \
		-t "$2:pg$4" -t "$2:$VER-pg$4" "$REPO" >"$log" 2>&1 || { tail -n 40 "$log" >&2; rm -f "$log"; return 1; }
	rm -f "$log"
	echo "$2:pg$4 ($2:$VER-pg$4) on postgres:$3"
}

for tag in "${tags[@]}"; do
	major=${tag%%[!0-9]*}
	build server "$NAME" "$tag" "$major"
	[[ -z $ext ]] || build extension "$NAME-extension" "$tag" "$major"
done
