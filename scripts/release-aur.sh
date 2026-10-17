#!/usr/bin/env bash
# Publish the PKGBUILD of this checkout (packaging/PKGBUILD, pkgver = the release) to the AUR package pg_sift.
# The AUR account's SSH key must be usable by ssh: loaded in your ssh-agent, or named in AUR_SSH_KEY (it is then added to a private
# ssh-agent, killed at the end, and ssh-add asks for its passphrase through the SSH_ASKPASS program of your environment).
#
#   scripts/release-aur.sh        after the v<pkgver> tag is pushed and its release workflow has passed
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ver=$(sed -n 's/^pkgver=//p' "$REPO/packaging/PKGBUILD")
git -C "$REPO" rev-parse -q --verify "refs/tags/v$ver" >/dev/null || { echo "tag v$ver does not exist" >&2; exit 1; }

if [[ -n ${AUR_SSH_KEY:-} ]]; then
	eval "$(ssh-agent -s)" >/dev/null
	trap 'kill "$SSH_AGENT_PID"; rm -rf "$WORK"' EXIT
	ssh-add "$AUR_SSH_KEY"
fi
git clone -q ssh://aur@aur.archlinux.org/pg_sift.git "$WORK/aur"
cd "$WORK/aur"
cp "$REPO/packaging/PKGBUILD" PKGBUILD
makepkg --printsrcinfo > .SRCINFO
git diff --quiet && { echo "the AUR already has $ver"; exit 0; }
git add PKGBUILD .SRCINFO
git commit -q -m "pg_sift $ver-$(sed -n 's/^pkgrel=//p' PKGBUILD)"
git push origin HEAD:master
