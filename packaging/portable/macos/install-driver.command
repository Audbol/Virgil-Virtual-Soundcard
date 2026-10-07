#!/bin/sh
# Installs the Virgil Core Audio device from this folder.
cd "$(dirname "$0")" || exit 1
echo "Installing VirgilAudio.driver into /Library/Audio/Plug-Ins/HAL (needs your password)..."
sudo rm -rf /Library/Audio/Plug-Ins/HAL/VirgilAudio.driver &&
sudo cp -R VirgilAudio.driver /Library/Audio/Plug-Ins/HAL/ &&
sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/HAL/VirgilAudio.driver 2>/dev/null
sudo killall coreaudiod 2>/dev/null
echo "Done. \"Virgil Virtual Soundcard\" is now available. You can close this window."
