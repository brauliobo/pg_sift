#!/usr/bin/env bash
# The PGDG apt repository on a hosted runner (the workflows install the server and its development files from it), without a default cluster:
# the tests start their own, so no service listens on 5432. Every call is bounded (scripts/apt-install.sh).
set -euo pipefail
. "$(dirname "$0")/apt-install.sh"
sudo install -d /usr/share/postgresql-common/pgdg
apt_bounded $((APT_UNIT * 5 / 2)) curl -fsSL --max-time 30 --retry 3 -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc https://www.postgresql.org/media/keys/ACCC4CF8.asc
. /etc/os-release
echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt ${VERSION_CODENAME}-pgdg main" |
	sudo tee /etc/apt/sources.list.d/pgdg.list
apt_get 3 $((APT_UNIT * 2)) update
apt_install postgresql-common
sudo sed -i 's/^#\?create_main_cluster.*/create_main_cluster = false/' /etc/postgresql-common/createcluster.conf
