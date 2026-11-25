# Containers

Image recipes for Docker, Docker Compose, and Kubernetes (CloudNativePG) live in [packaging/docker/](../packaging/docker/). No prebuilt image is published — build locally with `packaging/docker/build.sh` and push to your own registry. For a 5-minute walkthrough, see [QUICKSTART.md](QUICKSTART.md).

## 1. Server Image (Docker)

`packaging/docker/Dockerfile` (target `server`) compiles `pg_sift` against the official `postgres` image of the same tag and copies the extension into a clean second stage. On an empty data directory, `initdb.sql` runs `CREATE EXTENSION pg_sift` in the default database and `template1`.

```sh
packaging/docker/build.sh 17 18          # builds pg_sift:pg17, pg_sift:pg18, and pg_sift:6.0-pg<major>
packaging/docker/build.sh 19             # PostgreSQL 19
docker run -d --name sift -e POSTGRES_PASSWORD=example -p 127.0.0.1:5432:5432 pg_sift:pg18
docker exec -it sift psql -U postgres -c '\dx pg_sift'
```

- Pass `IMAGE_REPO=registry.example.com/team/pg_sift` to tag for your registry and `JOBS=2` to cap compiler parallelism.
- **Data directory:** Mount volumes at `/var/lib/postgresql` on PostgreSQL 18+ images and `/var/lib/postgresql/data` on PostgreSQL 17.
- **Existing data directories:** `initdb.sql` runs only on fresh volumes. Start the new image on your existing volume and follow [UPGRADING.md](UPGRADING.md).
- Pass PostgreSQL configuration directly as container arguments (for example, `-c shared_buffers=1GB`).

## 2. Docker Compose

```sh
docker compose -f packaging/docker/compose.yaml up --build -d
docker compose -f packaging/docker/compose.yaml exec db psql -U postgres -d app
```

Set `PG_TAG=17` (with `/var/lib/postgresql/data`) to run PostgreSQL 17.

## 3. CloudNativePG (Kubernetes)

### Option A: Image-Volume Extension (PostgreSQL 18+, Recommended)

The `extension` target builds a `FROM scratch` image containing only `lib/pg_sift.so` and `share/extension/`, matching CloudNativePG's Image Volume Extensions layout. Build on the exact PostgreSQL major, OS, and architecture of your `Cluster` image. The image holds no shared library: tiering to S3 or R2 loads `libcurl` in the operand image (`libcurl4` on Debian and Ubuntu), and tiering to a `file://` location needs none.

```sh
packaging/docker/build.sh -e 18-trixie
docker tag pg_sift-extension:pg18 registry.example.com/team/pg_sift-extension:6.0-pg18
docker push registry.example.com/team/pg_sift-extension:6.0-pg18
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
          reference: registry.example.com/team/pg_sift-extension:6.0-pg18
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

Requires PostgreSQL 18+ (`extension_control_path`), Kubernetes 1.35+ (or 1.33–1.34 with the `ImageVolume` feature gate), and containerd 2.1+ or CRI-O 1.31+. `pg_sift` requires no `shared_preload_libraries` entry (unless publishing a `sift_table` for logical replication).

### Option B: Custom Operand Image (PostgreSQL 17 or Older Operators)

Bake the compiled extension directly into the CloudNativePG operand image:

```dockerfile
FROM ghcr.io/cloudnative-pg/postgresql:18-minimal-trixie
COPY --from=pg_sift-extension:pg18 /lib/pg_sift.so /usr/lib/postgresql/18/lib/
COPY --from=pg_sift-extension:pg18 /share/extension/ /usr/share/postgresql/18/extension/
```

For PostgreSQL 17, build with `packaging/docker/build.sh -e 17-trixie` and copy into `/usr/lib/postgresql/17/lib/` and `/usr/share/postgresql/17/extension/`.
