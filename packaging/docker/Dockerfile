# pg_sift on the official PostgreSQL image. Build from the repository root (packaging/docker/build.sh does it for several majors):
#   docker build -f packaging/docker/Dockerfile --build-arg PG_TAG=18 -t pg_sift:pg18 .
# Targets:
#   server     (default) PostgreSQL with the extension installed; every new database gets it (initdb.sql)
#   extension  FROM scratch: lib/pg_sift.so and share/extension/ only, the layout of Kubernetes image-volume extensions (docs/CONTAINERS.md)
# The extension is built on the image it runs on, so the library always matches the server's major version and libc.
ARG PG_TAG=18

FROM postgres:${PG_TAG} AS build
ARG JOBS=4
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential postgresql-server-dev-${PG_MAJOR} liblz4-dev libzstd-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make -j${JOBS} USE_PGXS=1 SIFT_LTO=1 \
 && make USE_PGXS=1 SIFT_LTO=1 DESTDIR=/out install \
 && mkdir -p /ext/lib /ext/share \
 && cp /out/usr/lib/postgresql/*/lib/pg_sift.so /ext/lib/ \
 && cp -r /out/usr/share/postgresql/*/extension /ext/share/

# server is the last stage, so a plain `docker build` makes it
FROM scratch AS extension
COPY --from=build /ext/ /

FROM postgres:${PG_TAG} AS server
LABEL org.opencontainers.image.title="pg_sift" \
      org.opencontainers.image.description="PostgreSQL with the pg_sift extension installed" \
      org.opencontainers.image.source="https://github.com/brauliobo/pg_sift" \
      org.opencontainers.image.licenses="PostgreSQL"
COPY --from=build /out/usr/ /usr/
COPY packaging/docker/initdb.sql /docker-entrypoint-initdb.d/10-pg_sift.sql
