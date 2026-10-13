// The Desktop audio window of Virgil Control for Linux (GTK 3), the same as
// the Windows one: master switch, sources with level meters, the
// receive-channel output, status.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "desktop_audio_engine.h"

namespace dagtk {

// Virgil's channels as the control API reports them (used while desktop
// audio itself is not connected).
struct ChannelInfo {
  unsigned tx = 0, rx = 0;
  std::vector<std::string> tx_names, rx_names;
};
using ChannelSource = std::function<ChannelInfo()>;

// Opens the window (non-modal), or brings it up when it is open already.
void show(GtkWindow* parent, GdkPixbuf* icon, desk::Engine& engine, ChannelSource channels);

extern const char* const kHelpText;

}  // namespace dagtk
