# Packaging

What this directory and the repository root hold for each way of installing pg_sift.

| Target | Files | Built or checked by | State |
|---|---|---|---|
| Debian, Ubuntu (PostgreSQL 17, 18, 19) | `debian/` | `scripts/package.sh`, the `release` workflow | released with every tag |
| RHEL family (PostgreSQL 17, 18, 19) | `pg_sift.spec` | `scripts/package.sh`, the `release` workflow | released with every tag |
| Arch (AUR) | `PKGBUILD` | the maintainer | released with every tag |
| Homebrew (macOS arm64, HEAD) | `../Formula/pg_sift.rb` | `brew test` | HEAD formula in this repository |
| Container image, PostgreSQL 17, 18 and 19 | `docker/` | `docker/build.sh`, `bench/bench kit` | recipe; no image is published ([docs/CONTAINERS.md](../docs/CONTAINERS.md)) |
| CloudNativePG, Kubernetes image volume (PostgreSQL 18) | `docker/Dockerfile`, target `extension` | `docker/build.sh -e` | recipe; layout checked with a docker image mount, not on a cluster |
| pgxman | `pgxman.yaml` | `pgxman build -f packaging/pgxman.yaml` | buildkit file written to the published format; not built with the tool |
| PGXN | `../META.json` | `git archive`, then upload to pgxn.org | fields checked against the meta spec; not uploaded |
| SQLite, MariaDB ports | `sqlite/`, `mariadb/` | `scripts/package-sqlite.sh`, `scripts/package-mariadb.sh` | released with every tag |

`scripts/release.sh` keeps the version of `pgxman.yaml` and `META.json` in step with the release (its `bump` and version check).

## Packages for each PostgreSQL major

A release attaches twelve packages, one pair for each major the extension supports and each architecture (x86-64 and arm64). The name carries the major `N`; the architecture is `amd64`/`arm64` in the `.deb` and `x86_64`/`aarch64` in the `.rpm`:

| Family | File | Built and smoke-tested in | Needs |
|---|---|---|---|
| Debian, Ubuntu | `postgresql-N-pg-sift_X.Y.Z-1_{amd64,arm64}.deb` | built on Debian 13, installed in fresh Debian 13 and Ubuntu 24.04 containers | PGDG `postgresql-N` |
| RHEL, Rocky, Alma | `pg_sift_N-X.Y.Z-1.el9.{x86_64,aarch64}.rpm` | built on Rocky 9, installed in fresh Rocky 9, Alma 9 and Alma 10 containers | PGDG `postgresqlN-server` |

Pick the major of the server (`SHOW server_version`); a package of another major does not install. There is one Debian recipe (`debian/`, with `@PG@` where the major goes) and one `pg_sift.spec` (`%{pgmajor}`):
`packaging/container.sh` substitutes the major, so a new major is one word in `PGS`, not a copy of the recipes; the recipes name no architecture (`Architecture: any`, `ExclusiveArch: x86_64 aarch64`), so a new one is one word in `ARCHES` and a runner.

| Parameter | Default | Effect |
|---|---|---|
| `PGS` (environment of `scripts/package.sh`) | `17 18 19` | the majors built, tested, verified and listed in the matrix of the `release` workflow (`scripts/package.sh majors`); a release publishes only when every package of every major and family is in the set (`scripts/package.sh verify DIR`) |
| `FAMILIES` | `deb rpm` | the families built |
| `ARCHES` | `x86_64 aarch64` | the architectures `verify` requires and the matrix lists (`scripts/package.sh arches`); a host builds its own, with the native images: the release runs `x86_64` on `ubuntu-24.04` and `aarch64` on the hosted `ubuntu-24.04-arm` runner |
| `PG` (`scripts/package.sh build`) | none | the major of one build |
| `BUILD_DEB`, `BUILD_RPM`, `TEST_DEB`, `TEST_RPM` | Debian 13, Rocky 9; the images above | the images that build and test |

The server and its development files come from PGDG in the build container: apt `trixie-pgdg` and yum `pgdgN` for 17 and 18. PostgreSQL 19 is not released yet (PGDG ships `19~rc1`), so its packages come from the apt
component `19` of the same suite and the yum repository `pgdg19-updates-testing`, which `container.sh` enables when the stable `pgdgN` does not exist; the library is built against the release candidate and the smoke test
runs on it. The build is the same for every major (`src/core/compat.h` holds the differences in the source). `scripts/package.sh --selftest` (in `scripts/ci.sh --selftest`) checks the guard and that no recipe names a literal major.

## arm64 (aarch64)

The arm64 packages are generic AArch64: they run on any aarch64 Linux whose distribution matches the package family (AWS Graviton, Ampere, Azure and Google Cloud Arm instances, a Raspberry Pi 4 or 5, a Linux VM on Apple silicon).

- **Baseline.** No `-march`, `-mcpu` or feature flag is passed: the compiler's default, plain `armv8-a`, which includes Advanced SIMD (NEON). The SIMD dispatch (`sift.simd`, `core/cpu.c`) takes NEON at compile time on arm64, so the level is `neon` on every aarch64 CPU; nothing depends on CRC32, LSE, dot-product, FP16 or SVE at compile time (`src/` has no `__ARM_FEATURE_*`, `getauxval` or CRC32 intrinsic; the shipped `pg_sift.so` has no CRC32, dot-product, FP16 or SVE instruction). The only LSE instructions in `pg_sift.so` are the six of libgcc's outline-atomics helpers (`__aarch64_cas*`, `__aarch64_ldadd*`, `__aarch64_swp*`), which the helpers pick at run time after testing the CPU.
- **Names and sources.** `postgresql-N-pg-sift_X.Y.Z-1_arm64.deb` from PGDG `postgresql-N` on Debian 13 and Ubuntu 24.04, `pg_sift_N-X.Y.Z-1.el9.aarch64.rpm` from PGDG `postgresqlN-server` on Rocky, Alma and RHEL 9 and 10, installed with the same commands as x86-64.
- **Build.** The `release` workflow builds them on the hosted `ubuntu-24.04-arm` runner and installs each in fresh arm64 containers of the images above. GCC 14.2 of Debian 13 failed to compile `src/col/codec_vec.h` on arm64 until the type of its double vectors lost an alignment attribute.
- **AWS (Graviton).** On an EC2 Graviton instance with Debian 13 or Ubuntu 24.04: `sudo apt install ./postgresql-N-pg-sift_X.Y.Z-1_arm64.deb`; with a RHEL-compatible AMI: `sudo dnf install ./pg_sift_N-X.Y.Z-1.el9.aarch64.rpm`; then `CREATE EXTENSION pg_sift`. Amazon RDS and Aurora do not load extensions that are not on their list, so pg_sift needs a self-managed PostgreSQL (EC2, or a container).

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
