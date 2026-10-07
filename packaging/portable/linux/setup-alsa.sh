#!/bin/sh
# Adds the Virgil ALSA devices to ~/.asoundrc using the plugin in this folder.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
rc="$HOME/.asoundrc"
if [ -f "$rc" ] && grep -q "Virgil portable" "$rc"; then
	# Replace a previous block (folder may have moved).
	sed -i '/# >>> Virgil portable/,/# <<< Virgil portable/d' "$rc"
fi
cat >> "$rc" <<CONF
# >>> Virgil portable
pcm_type.virgil { lib "$here/libasound_module_pcm_virgil.so" }
pcm.virgil_hw { type virgil hint { show on description "Virgil Virtual Soundcard (raw)" } }
pcm.virgil { type plug slave.pcm "virgil_hw" hint { show on description "Virgil Virtual Soundcard" } }
# <<< Virgil portable
CONF
echo "Added ALSA devices 'virgil' and 'virgil_hw' to $rc"
