#!/bin/sh
# Download the newest Virgil Raspberry Pi installer from GitHub and run it:
#   curl -fsSL https://raw.githubusercontent.com/Audbol/Virgil-Virtual-Soundcard/HEAD/packaging/raspberry-pi/get-virgil.sh | sh
# Options after "sh -s --" go to the installer (--name, --interface).
# While the repository is private, set GITHUB_TOKEN to a token that can read it.
set -eu
repo=${VIRGIL_REPO:-Audbol/Virgil-Virtual-Soundcard}
api="https://api.github.com/repos/$repo/releases?per_page=20"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

get() {  # get URL OUT [extra curl args]
	url=$1; out=$2; shift 2
	if [ -n "${GITHUB_TOKEN:-}" ]; then
		curl -fsSL -H "Authorization: Bearer $GITHUB_TOKEN" "$@" -o "$out" "$url"
	else
		curl -fsSL "$@" -o "$out" "$url"
	fi
}

command -v curl >/dev/null 2>&1 || { echo "please install curl first: sudo apt install curl" >&2; exit 1; }
echo "Looking up the newest Virgil release..."
get "$api" "$tmp/releases.json" || {
	echo "Could not reach GitHub (is the Pi online? private repository: set GITHUB_TOKEN)." >&2
	exit 1
}
# Newest release that has a Raspberry Pi installer: "<api url> <name>".
asset=$(python3 - "$tmp/releases.json" <<'PY'
import json, sys
for rel in json.load(open(sys.argv[1])):
    if rel.get("draft"):
        continue
    for a in rel.get("assets", []):
        if a["name"].endswith("-raspberry-pi-installer.sh"):
            print(a["url"], a["name"])
            sys.exit(0)
sys.exit(1)
PY
) || { echo "No Raspberry Pi installer found in the releases of $repo." >&2; exit 1; }
url=${asset%% *}
file=${asset#* }
echo "Downloading $file..."
get "$url" "$tmp/$file" -H "Accept: application/octet-stream"
sh "$tmp/$file" "$@" </dev/null
