#!/usr/bin/env bash
# apt-get on a hosted runner with every call bounded. Installs sat 7 to 24 minutes with no output on random runners (and one mirror served a package at 55 kB/s for
# 34 minutes): a call past its bound is cut and retried, and what held it up (processes, lock holders, the tail of the apt and dpkg logs) is printed while it hangs.
#   scripts/apt-install.sh [--update] PACKAGE...   download first (resumed by a retry), then install from the local files
#   . scripts/apt-install.sh                       the functions only (scripts/pgdg-repo.sh)
#   scripts/apt-install.sh --selftest              a fake apt-get that hangs once, and one that fails
set -euo pipefail

# sudo drops the environment, so the settings that keep a maintainer script from waiting on stdin travel through `env`
export DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a
APT_UNIT=${APT_UNIT:-60} # seconds; every bound below is a multiple of it
# a connection that sends nothing for 20 s is cut and tried again (five times, on the next mirror of the list when there is one) inside the attempt, before the attempt itself is cut: the mirror that served a package at 55 kB/s, or none, held a call for its whole bound
APT_OPTS=(-o DPkg::Lock::Timeout=60 -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold -o Acquire::Retries=5 -o Acquire::http::Timeout=20 -o Acquire::https::Timeout=20)
# APT_ARCHIVES: where the downloaded packages are kept, a directory the workflow caches between runs (the install of PostgreSQL and its development files is 90 MB from one mirror)
[[ -z ${APT_ARCHIVES:-} ]] || APT_OPTS+=(-o "Dir::Cache::archives=$APT_ARCHIVES")

# no lsof, lslocks or fuser on a runner: /proc/locks names the holder of a lock file by its inode
apt_diag() {
	local f ino pid
	echo "::group::apt diagnostics"   # no --forest: it takes seconds on a table of tens of thousands of processes (a shared box), the ppid column gives the tree
	ps -eo pid,ppid,etime,stat,wchan:20,cmd | grep -E 'apt|dpkg|needrestart|postinst|debconf' | grep -v 'grep -E' || true
	for f in /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/lib/apt/lists/lock /var/cache/apt/archives/lock; do
		[[ -e $f ]] || continue
		ino=$(stat -c %i "$f")
		for pid in $(awk -v i=":$ino\$" '$6 ~ i { print $5 }' /proc/locks); do
			echo "lock $f held by $pid: $(tr '\0' ' ' </proc/"$pid"/cmdline)"
		done
	done
	sudo -n tail -n 30 /var/log/apt/term.log /var/log/dpkg.log 2>&1 || true
	echo "::endgroup::"
}

# apt_bounded SECS CMD...: CMD as root with no stdin, cut after SECS; at four fifths of it the diagnostics print, while the culprit still runs
apt_bounded() {
	local secs=$1 rc=0 w i
	shift
	(
		for ((i = 0; i < secs * 4 / 5; i++)); do sleep 1; done
		echo "apt-install: '$*' still running after $((secs * 4 / 5)) s"
		apt_diag
	) &
	w=$!
	sudo env DEBIAN_FRONTEND=noninteractive NEEDRESTART_MODE=a timeout -k 10 "$secs" "$@" </dev/null || rc=$?
	kill "$w" 2>/dev/null || true
	wait "$w" 2>/dev/null || true
	(( rc != 124 && rc != 137 )) || echo "apt-install: '$*' cut after $secs s"
	return "$rc"
}

# The runner image lists azure.archive.ubuntu.com first, and it served the 14 MB libclang-cpp19 (a dependency of the server development files) at a trickle that no apt timeout
# cuts, on two jobs of a run of 32 at once. A try that failed or was cut puts archive.ubuntu.com in its place for the next one; a host without the list is left alone.
apt_mirror_fallback() {
	local f=${APT_MIRRORS:-/etc/apt/apt-mirrors.txt}
	[[ -f $f ]] || return 0
	sudo sed -i 's#//azure\.archive\.ubuntu\.com/#//archive.ubuntu.com/#' "$f"
	echo "apt-install: the next try reads from $(grep -m1 -o 'http[^[:space:]]*' "$f")"
}

# apt_get TRIES SECS ARGS...: apt-get under the bound; a failed try lets dpkg finish what the cut one left half configured
apt_get() {
	local tries=$1 secs=$2 n
	shift 2
	for ((n = 1; n <= tries; n++)); do
		apt_bounded "$secs" apt-get "${APT_OPTS[@]}" "$@" && return 0
		(( n == tries )) || { apt_mirror_fallback; apt_bounded $((APT_UNIT * 2)) dpkg --configure -a || true; }
	done
	echo "apt-install: apt-get $* failed after $tries tries"
	return 1
}

# the packages a cache saves are readable by the user that saves it, and the lock file of apt is not one of them
apt_archives_ready() {
	[[ -z ${APT_ARCHIVES:-} ]] || sudo install -d -m 755 "$APT_ARCHIVES" "$APT_ARCHIVES/partial"
}
apt_archives_done() {
	[[ -z ${APT_ARCHIVES:-} ]] || { sudo rm -f "$APT_ARCHIVES/lock"; sudo chmod -R a+rX "$APT_ARCHIVES"; }
}

# the download that did not finish is not fatal: the install resumes it
apt_install() {
	local rc=0
	apt_archives_ready
	# four tries of a minute, each resuming the partial file of the last, the first from the mirror of the image and the others from the next: 100 MB takes seconds from a mirror that works
	apt_get 4 "$APT_UNIT" install -y --download-only "$@" || true
	echo "apt-install: download step ended after $SECONDS s of the script"
	apt_get 3 $((APT_UNIT * 2)) install -y "$@" || rc=$?
	apt_archives_done
	return "$rc"
}

apt_selftest() {
	[[ $(uname) == Linux ]] || { echo "apt-install selftest: skipped (not Linux)"; return 0; }
	local d out bad=0 self
	self=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
	d=$(mktemp -d); trap "rm -rf '$d'; trap - RETURN" RETURN
	mkdir "$d/bin"
	printf '#!/bin/sh\n[ "$1" = -n ] && shift\nexec "$@"\n' >"$d/bin/sudo"
	printf '#!/bin/sh\necho "dpkg $*" >>"$LOG"\n' >"$d/bin/dpkg"
	cat >"$d/bin/apt-get" <<'FAKE'
#!/bin/sh
echo "apt-get $*" >>"$LOG"
case " $* " in
*" --download-only "*) [ -e "$LOG.hung" ] || [ -n "$FAIL" ] || { touch "$LOG.hung"; exec sleep 30; } ;;
esac
exit "${FAIL:-0}"
FAKE
	chmod +x "$d"/bin/*
	printf 'http://azure.archive.ubuntu.com/ubuntu/\tpriority:1\nhttp://security.ubuntu.com/ubuntu/\tpriority:3\n' >"$d/mirrors"
	local run="env PATH=$d/bin:$PATH APT_UNIT=3 LOG=$d/log FAIL= APT_MIRRORS=$d/mirrors bash $self pkg"
	# a hang in the download: cut, diagnosed (the stuck call is in the process list), retried, then the install
	out=$($run 2>&1) || { echo "FAIL: apt-install does not recover from a hung download: $(tail -n 5 <<<"$out")"; bad=1; }
	[[ $out == *'still running after'* && $out == *'timeout -k 10 3 apt-get'* && $out == *'cut after 3 s'* ]] || { echo "FAIL: apt-install does not print the diagnostics of a hung call: $(tail -n 5 <<<"$out")"; bad=1; }
	[[ $(grep -c 'download-only' "$d/log") == 2 && $(grep -c ' install -y --no-install-recommends pkg$' "$d/log") == 1 && $(grep -c '^dpkg --configure -a$' "$d/log") == 1 ]] ||
		{ echo "FAIL: apt-install retry path differs from one cut download, one reconfigure, one install: $(cat "$d/log")"; bad=1; }
	[[ $out == *'next try reads from http://archive.ubuntu.com/'* && $(grep -c azure "$d/mirrors") == 0 ]] || { echo "FAIL: apt-install does not move to the other mirror after a cut try: $(cat "$d/mirrors")"; bad=1; }
	# an install that keeps failing is retried three times and fails the step
	: >"$d/log"
	out=$(env FAIL=1 PATH="$d/bin:$PATH" APT_UNIT=1 LOG="$d/log" bash "$self" pkg 2>&1) && { echo "FAIL: apt-install exits 0 when the install fails: $(tail -n 5 <<<"$out")"; bad=1; }
	[[ $(grep -c ' install -y --no-install-recommends pkg$' "$d/log") == 3 ]] || { echo "FAIL: apt-install does not try the install three times: $(cat "$d/log")"; bad=1; }
	# the directory of the cache is the archive directory of every call
	: >"$d/log"
	env APT_ARCHIVES="$d/debs" PATH="$d/bin:$PATH" APT_UNIT=1 LOG="$d/log" FAIL= bash "$self" pkg >/dev/null 2>&1 || { echo "FAIL: apt-install with APT_ARCHIVES fails"; bad=1; }
	[[ $(grep -c "Dir::Cache::archives=$d/debs" "$d/log") == 2 && -d $d/debs/partial ]] || { echo "FAIL: apt-install does not keep its packages in APT_ARCHIVES: $(cat "$d/log")"; bad=1; }
	rm -rf "$d"
	(( bad )) || echo "apt-install selftest: a hung call is cut, diagnosed and retried; a failing install is tried three times; the packages go to APT_ARCHIVES"
	return "$bad"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	case ${1:-} in
	--selftest) apt_selftest ;;
	--update)
		shift
		apt_get 3 $((APT_UNIT * 2)) update
		apt_install --no-install-recommends "$@"
		;;
	*) apt_install --no-install-recommends "$@" ;;
	esac
fi
