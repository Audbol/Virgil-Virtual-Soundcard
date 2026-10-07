#!/bin/sh
# Installs the DSV Core Audio device from this folder.
cd "$(dirname "$0")" || exit 1
echo "Installing DSVAudio.driver into /Library/Audio/Plug-Ins/HAL (needs your password)..."
sudo rm -rf /Library/Audio/Plug-Ins/HAL/DSVAudio.driver &&
sudo cp -R DSVAudio.driver /Library/Audio/Plug-Ins/HAL/ &&
sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/HAL/DSVAudio.driver 2>/dev/null
sudo killall coreaudiod 2>/dev/null
echo "Done. \"DSV Virtual Soundcard\" is now available. You can close this window."
