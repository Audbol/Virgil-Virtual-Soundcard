#!/bin/sh
# apt-get install for CI runners: no prompts, network timeouts, and a bounded
# retry instead of hanging for an hour on a stuck mirror.
#   .github/apt-install.sh pkg...
set -u
export DEBIAN_FRONTEND=noninteractive
opts="-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o Dpkg::Use-Pty=0"
for attempt in 1 2 3; do
	if sudo -E timeout 300 apt-get $opts update -q &&
		sudo -E timeout 600 apt-get $opts install -y -q "$@"; then
		exit 0
	fi
	echo "apt-get attempt $attempt failed or timed out; retrying" >&2
	sleep 10
done
exit 1
