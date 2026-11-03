#!/usr/bin/env bash
# The --cpus of a docker run: the CPUs wanted (default 4), or all of the host's when it has fewer: docker refuses a limit above the CPUs of the host (a hosted runner has 2).
n=$(nproc); want=${1:-4}
echo $(( n < want ? n : want ))
