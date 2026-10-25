#!/usr/bin/env bash
# apt-get install on a hosted runner with a time bound on the download: the mirror sometimes serves one package at 55 kB/s (the llvm packages that
# postgresql-server-dev pulls took 34 minutes in one job). A try that has not fetched everything in 120 s is cut and retried (apt resumes the partial files),
# then the install itself runs from the local files.
set -euo pipefail
for try in 1 2 3 4; do
	sudo timeout 120 apt-get install -y --no-install-recommends --download-only "$@" && break
done
sudo apt-get install -y --no-install-recommends "$@"
