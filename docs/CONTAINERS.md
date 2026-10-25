# Containers

Image recipes for Docker, Docker Compose and Kubernetes (CloudNativePG), in [packaging/docker/](../packaging/docker/). No image is published: build locally
(`packaging/docker/build.sh`) and push to your own registry. A tour of the extension is [QUICKSTART.md](QUICKSTART.md).

## The server image

`packaging/docker/Dockerfile` (target `server`) builds the extension on the official `postgres` image of the same tag and installs it into a second stage of that image,
so the library matches the server's major version and libc. `initdb.sql` runs `CREATE EXTENSION pg_sift` for the first database and `template1` when the data directory is
created, so every later `CREATE DATABASE` has it.

```sh
packaging/docker/build.sh 17 18            # pg_sift:pg17, pg_sift:pg18 and pg_sift:<version>-pg<major>; JOBS=2 for fewer compiler jobs
packaging/docker/build.sh 19rc1            # PostgreSQL 19: the `19rc1` tag until the official `19` tag exists
docker run -d --name sift -e POSTGRES_PASSWORD=example -p 127.0.0.1:5432:5432 pg_sift:pg18
docker exec -it sift psql -U postgres -c '\dx pg_sift'
```

The argument is a tag of the official image (`18`, `18-trixie`, `17`, `19rc1`, and `19` once it exists); the build installs `postgresql-server-dev-<major>` from the PGDG
repository the image already uses. `IMAGE_REPO=registry.example.com/team/pg_sift` renames the repository.

- Data directory: `/var/lib/postgresql` in the PostgreSQL 18 image, `/var/lib/postgresql/data` in 17; mount the volume accordingly.
- The initialization script runs only on an empty data directory. For an existing one, start the new image on it and run `ALTER EXTENSION pg_sift UPDATE` in each database ([SUPPORT.md](SUPPORT.md#upgrade-paths)).
- Built and run on x86-64 only. The Makefile adds no x86-only flags on other CPUs, but an arm64 image has not been built or tested.
- No settings are changed; pass them as arguments (`-c shared_buffers=1GB`).

### Docker Compose

```sh
docker compose -f packaging/docker/compose.yaml up --build -d
docker compose -f packaging/docker/compose.yaml exec db psql -U postgres -d app
```

`PG_TAG=17` selects the other major (mount the volume at `/var/lib/postgresql/data`, see the comment in the file).

## CloudNativePG

Two ways, by operator and PostgreSQL version.

### Image-volume extension (PostgreSQL 18, a recent operator)

The `extension` target is an image `FROM scratch` with `lib/pg_sift.so` and `share/extension/`, the layout CloudNativePG documents for extension images
(`/extensions/<name>/lib` is added to `dynamic_library_path`, `/extensions/<name>/share` to `extension_control_path`). The PostgreSQL major, operating system and CPU
architecture must match the `Cluster`'s image: build on the matching tag.

```sh
packaging/docker/build.sh -e 18-trixie          # also makes pg_sift-extension:pg18
docker tag pg_sift-extension:pg18 registry.example.com/team/pg_sift-extension:3.3-pg18 && docker push registry.example.com/team/pg_sift-extension:3.3-pg18
```

```yaml
apiVersion: postgresql.cnpg.io/v1
kind: Cluster
metadata:
  name: sift-example
spec:
  instances: 1
  imageName: ghcr.io/cloudnative-pg/postgresql:18-minimal-trixie
  postgresql:
    extensions:
      - name: pg_sift
        image:
          reference: registry.example.com/team/pg_sift-extension:3.3-pg18
  storage:
    size: 1Gi
---
apiVersion: postgresql.cnpg.io/v1
kind: Database
metadata:
  name: sift-example-app
spec:
  name: app
  owner: app
  cluster:
    name: sift-example
  extensions:
    - name: pg_sift
```

Requirements (CloudNativePG documentation, "Image Volume Extensions"): PostgreSQL 18 or later (`extension_control_path`), Kubernetes 1.35 or later (1.33 and
1.34 with the `ImageVolume` feature gate), and a container runtime with image-volume support (containerd 2.1 or later, CRI-O 1.31 or later). Adding the extension to a
running cluster triggers a rolling update. pg_sift needs no `shared_preload_libraries` entry.

Checked: the image's contents, and that PostgreSQL 18 of the `18-minimal-trixie` operand image loads the library, creates the extension, builds a `sift` index and answers a
search from an image mounted at `/extensions/pg_sift` with `extension_control_path = '$system:/extensions/pg_sift/share'` and `dynamic_library_path = '$libdir:/extensions/pg_sift/lib'`
(a docker image mount, no cluster). The manifests above were not applied to a Kubernetes cluster, and the first operator release that accepts `.spec.postgresql.extensions` was not checked (see the documentation of your release).

### Operand image (PostgreSQL 17, or an operator without image volumes)

An image that contains the files needs nothing from the operator. Build it on the CloudNativePG operand image of your major:

```dockerfile
FROM ghcr.io/cloudnative-pg/postgresql:18-minimal-trixie
COPY --from=pg_sift-extension:pg18 /lib/pg_sift.so /usr/lib/postgresql/18/lib/
COPY --from=pg_sift-extension:pg18 /share/extension/ /usr/share/postgresql/18/extension/
```

The same checks passed on an image built this way (`docker build`, then `CREATE EXTENSION` and a search on PostgreSQL 18 of the operand image). Set it as the `Cluster`'s `imageName` and create the extension with the `Database` resource above (without `.spec.postgresql.extensions`). For PostgreSQL 17 build the
extension image with `packaging/docker/build.sh -e 17-trixie` and use the 17 paths. It costs more to maintain (one image per PostgreSQL minor you roll) than an
image-volume extension, so prefer the first way where available.
