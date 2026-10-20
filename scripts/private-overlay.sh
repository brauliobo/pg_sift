#!/usr/bin/env bash
# Private test overlay: tests, helpers and data that are not part of this repository run from a directory of the maintainer's, laid over the work tree.
#
#   SIFT_PRIVATE_TESTS=DIR scripts/private-overlay.sh sync     copy every file of DIR to the same path in the work tree (DIR mirrors it: test/sql/x.sql,
#                                                              test/expected/x.out, test/corpus/...); the paths are added to .git/info/exclude
#   scripts/private-overlay.sh clean                           remove the files of the last sync
#   scripts/private-overlay.sh status                          list them
#
# Without SIFT_PRIVATE_TESTS, sync does nothing and succeeds: CI never has an overlay and never fails for the lack of one. A file of the overlay never replaces a
# tracked file (the sync stops). The Makefile takes every test/sql/*.sql, so an overlaid test is part of `make installcheck` and of scripts/pgtest.sh regress.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
LIST=$(git rev-parse --git-path private-overlay.list)
EXCL=$(git rev-parse --git-path info/exclude)

case ${1:-} in
	sync)
		[[ -n ${SIFT_PRIVATE_TESTS:-} && -d ${SIFT_PRIVATE_TESTS} ]] || { echo "private-overlay: no SIFT_PRIVATE_TESTS, nothing to lay over"; exit 0; }
		src=$(cd "$SIFT_PRIVATE_TESTS" && pwd)
		mkdir -p "$(dirname "$LIST")" "$(dirname "$EXCL")"
		: >"$LIST.new"
		while IFS= read -r -d '' f; do
			p=${f#"$src"/}
			if git ls-files --error-unmatch -- "$p" >/dev/null 2>&1; then echo "private-overlay: $p is tracked, not replaced" >&2; rm -f "$LIST.new"; exit 1; fi
			mkdir -p "$(dirname "$p")"; cp -p "$f" "$p"
			echo "$p" >>"$LIST.new"
			grep -qxF "/$p" "$EXCL" 2>/dev/null || echo "/$p" >>"$EXCL"
		done < <(find "$src" -type f -not -path '*/.git/*' -print0 | sort -z)
		sort -u "$LIST.new" >"$LIST"; rm -f "$LIST.new"
		echo "private-overlay: $(wc -l <"$LIST") files laid over from $src" ;;
	clean)
		[[ -f $LIST ]] || { echo "private-overlay: nothing to clean"; exit 0; }
		while IFS= read -r p; do rm -f -- "$p"; done <"$LIST"
		if [[ -f $EXCL ]]; then grep -vxFf <(sed 's#^#/#' "$LIST") "$EXCL" >"$EXCL.new" || true; mv "$EXCL.new" "$EXCL"; fi
		rm -f "$LIST"; echo "private-overlay: removed" ;;
	status) [[ -f $LIST ]] && cat "$LIST" || echo "private-overlay: none" ;;
	*) sed -n '2,11p' "$0"; exit 1 ;;
esac
