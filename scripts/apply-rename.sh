#!/bin/sh
# apply-rename.sh [--check] DIR : convert a worktree to the naming vocabulary (idempotent); --check lists files that still use an old name
exec python3 "$(dirname "$(readlink -f "$0")")/apply-rename.py" "$@"
