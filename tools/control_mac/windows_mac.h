// Virgil Control for macOS (AppKit): the "Dropouts and diagnostics" window,
// the channel name editor and the configuration file editor, the same as
// the Windows and Linux ones (shared models in tools/control_common). Each
// opens once (non-modal) and comes forward when opened again. Main thread
// only; their requests to virgild run off the main thread.
#pragma once

#include "desktop_audio_mac.h"  // damac::ChannelSource: the channel names on the network

namespace vcmac {

// Dropouts and diagnostics: headline, the last hour per minute, hints,
// events, Save report… and Clear. Refreshes every 2 s while visible.
void show_diagnostics(unsigned port);

// Channel names: loads the configuration afresh, edits the transmit and
// receive names, saves only what changed. `live` gives the names on the
// network (placeholders).
void show_channel_names(unsigned port, damac::ChannelSource live);

// Edit configuration…: the whole virgil.conf as text, Apply & restart.
void show_config_editor(unsigned port);

}  // namespace vcmac
