# Packaging

What this directory and the repository root hold for each way of installing pg_sift. Release steps: [docs/RELEASE.md](../docs/RELEASE.md); supported versions: [docs/SUPPORT.md](../docs/SUPPORT.md).

| Target | Files | Built or checked by | State |
|---|---|---|---|
| Debian, Ubuntu (PostgreSQL 18) | `debian/` | `scripts/package.sh`, the `release` workflow | released with every tag |
| RHEL family (PostgreSQL 18) | `pg_sift_18.spec` | `scripts/package.sh`, the `release` workflow | released with every tag |
| Arch (AUR) | `PKGBUILD`, `PKGBUILD.local` | `scripts/release-aur.sh` | released with every tag |
| Homebrew (macOS arm64, HEAD) | `../Formula/pg_sift.rb` | `brew test` ([docs/RELEASE.md](../docs/RELEASE.md#homebrew-tap)) | HEAD formula in this repository |
| Container image, PostgreSQL 17, 18 (19 with its tag) | `docker/` | `docker/build.sh`, `bench/bench kit` | recipe; no image is published ([docs/CONTAINERS.md](../docs/CONTAINERS.md)) |
| CloudNativePG, Kubernetes image volume (PostgreSQL 18) | `docker/Dockerfile`, target `extension` | `docker/build.sh -e` | recipe; layout checked with a docker image mount, not on a cluster |
| pgxman | `pgxman.yaml` | `pgxman build -f packaging/pgxman.yaml` | buildkit file written to the published format; not built with the tool |
| PGXN | `../META.json` | `git archive`, then upload to pgxn.org | fields checked against the meta spec; not uploaded |
| SQLite, MariaDB ports | `sqlite/`, `mariadb/` | `scripts/package-sqlite.sh`, `scripts/package-mariadb.sh` | released with every tag |

`scripts/release.sh` keeps the version of `pgxman.yaml` and `META.json` in step with the release (its `bump` and version check).

## PGXN archive

PGXN takes a zip of the tag with `META.json` at the top of it:

```sh
git archive --format zip --prefix=pg_sift-3.3.0/ -o pg_sift-3.3.0.zip v3.3.0
```

The upload needs a PGXN account and is not part of the release script.

## What is not here

- Trunk (Tembo): its registry repository (`tembo-io/trunk`) was not reachable when this was written, so no `Trunk.toml` could be checked against its format.
- Pigsty and other catalogues that list extensions in their own repository (a package table row, a build recipe): those files live in the catalogue's repository, not here.
  Pigsty's own packages are built from the PGDG-style `.deb` and `.rpm` that the release already produces; a catalogue entry would point at them.
- The PGDG repositories (`apt.postgresql.org`, `yum.postgresql.org`) take packages through their own request process.
