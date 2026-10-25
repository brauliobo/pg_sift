# shellcheck shell=bash
# Sourced by the container scripts (mounted at /net.sh): a stalled mirror connection fails after 20 s and is retried, instead of
# hanging for many minutes (the dnf and apt runs of the release jobs sat silent for 13 to 24 minutes). Needs FMT (deb or rpm).
if [[ $FMT == deb ]]; then
	printf 'Acquire::Retries "5";\nAcquire::http::Timeout "20";\nAcquire::https::Timeout "20";\n' > /etc/apt/apt.conf.d/99net
else
	printf 'timeout=20\nretries=5\nip_resolve=4\n' >> /etc/dnf/dnf.conf
fi
