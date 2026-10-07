#!/bin/sh
# Adds the DSV ALSA devices to ~/.asoundrc using the plugin in this folder.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
rc="$HOME/.asoundrc"
if [ -f "$rc" ] && grep -q "DSV portable" "$rc"; then
	# Replace a previous block (folder may have moved).
	sed -i '/# >>> DSV portable/,/# <<< DSV portable/d' "$rc"
fi
cat >> "$rc" <<CONF
# >>> DSV portable
pcm_type.dsv { lib "$here/libasound_module_pcm_dsv.so" }
pcm.dsv_hw { type dsv hint { show on description "DSV Virtual Soundcard (raw)" } }
pcm.dsv { type plug slave.pcm "dsv_hw" hint { show on description "DSV Virtual Soundcard" } }
# <<< DSV portable
CONF
echo "Added ALSA devices 'dsv' and 'dsv_hw' to $rc"
