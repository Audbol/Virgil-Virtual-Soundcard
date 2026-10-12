// The Desktop audio window of Virgil Control (Windows): master switch,
// sources with level meters, the receive-channel output, status.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "desktop_audio.h"

namespace da {

// Virgil's channels as the control API reports them (used while desktop
// audio itself is not connected).
struct ChannelInfo {
  unsigned tx = 0, rx = 0;
  std::vector<std::string> tx_names, rx_names;
};
using ChannelSource = ChannelInfo (*)();

// Runs the (modal) window; returns when it closes. A second call while it is
// open brings it to the front instead.
void show_dialog(HINSTANCE inst, HWND owner, HICON icon, Engine& engine, ChannelSource channels);
HWND open_dialog();  // the open window, if any

extern const wchar_t* const kHelpText;

}  // namespace da
