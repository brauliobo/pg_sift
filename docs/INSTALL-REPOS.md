# Install from the package repositories

Every release publishes signed APT and DNF repositories for PostgreSQL 17, 18 and 19, on x86-64 and arm64, at `https://brauliobo.github.io/pg_sift`. The packages are the ones attached to the GitHub release ([packaging/README.md](../packaging/README.md)); the repositories add updates through `apt upgrade` and `dnf upgrade`. A container needs no repository: [QUICKSTART.md](QUICKSTART.md).

Check the key before you trust it: its fingerprint is in the release notes and in `SECURITY.md`, and `gpg --show-keys pg_sift.asc` prints it.

## Debian, Ubuntu

```sh
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSL https://brauliobo.github.io/pg_sift/apt/pg_sift.gpg -o /etc/apt/keyrings/pg_sift.gpg
echo "deb [signed-by=/etc/apt/keyrings/pg_sift.gpg] https://brauliobo.github.io/pg_sift/apt stable main" | sudo tee /etc/apt/sources.list.d/pg_sift.list
sudo apt update && sudo apt install postgresql-18-pg-sift
```

Debian 12 and 13, Ubuntu 22.04 and 24.04; one package serves them all. Replace `18` with the major of the server (`SHOW server_version`); the PostgreSQL package (`postgresql-18` from PGDG or the distribution) comes first.

## RHEL, Rocky, AlmaLinux (9 and 10)

```sh
sudo curl -fsSL https://brauliobo.github.io/pg_sift/rpm/pg_sift-el.repo -o /etc/yum.repos.d/pg_sift.repo
sudo dnf install pg_sift_18
```

## Amazon Linux 2023 (PostgreSQL 17 and 18)

```sh
sudo curl -fsSL https://brauliobo.github.io/pg_sift/rpm/pg_sift-amzn2023.repo -o /etc/yum.repos.d/pg_sift.repo
sudo dnf install pg_sift_18
```

`gpgcheck` and `repo_gpgcheck` are on: DNF verifies each package and the repository metadata against `https://brauliobo.github.io/pg_sift/pg_sift.asc`.

## Use it

```sql
CREATE EXTENSION pg_sift;
```

There is no `shared_preload_libraries` entry to add. Upgrade the extension in each database after a package upgrade: `ALTER EXTENSION pg_sift UPDATE` ([UPGRADING.md](UPGRADING.md)).

## Maintainers

`.github/workflows/publish-repos.yml` runs after a successful `release` workflow of a `v*` tag (or by dispatch for an existing tag): it downloads the `.deb` and `.rpm` assets of the last three releases, builds the tree with `packaging/repo/build-apt-dnf-repo.sh build`, checks it with `verify` and deploys it to Pages. Set once: Pages source "GitHub Actions", the secret `REPO_GPG_PRIVATE_KEY` (and `REPO_GPG_PASSPHRASE` for a protected key). `packaging/repo/build-apt-dnf-repo.sh --selftest` builds and verifies a repository of empty packages with a throwaway key; `PGS`, `ARCHES` and `AMZN_PGS` are the sets `scripts/package.sh` uses, and `scripts/check-release-sync.sh` keeps them equal.
