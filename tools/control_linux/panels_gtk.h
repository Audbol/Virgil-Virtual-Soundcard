// Virgil Control for Linux (GTK 3): the windows that used to be pages of the
// browser control panel, drawn natively on the shared models of
// tools/control_common:
//   * Dropouts and diagnostics (diagnostics_model.h)
//   * Channel names            (config_model.h)
//   * Edit configuration       (config_model.h)
// Each opens non-modal, one at a time (a second call brings it up), and talks
// to virgild on a worker thread so the GTK thread never waits on the network.
#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace vcgtk {

void show_diagnostics(GtkWindow* parent, GdkPixbuf* icon, unsigned port);

// `tx_live` / `rx_live`: the channel names on the network now
// (Status::tx_names / rx_names), shown as the fields' placeholders.
void show_channel_names(GtkWindow* parent, GdkPixbuf* icon, unsigned port, const std::vector<std::string>& tx_live,
                        const std::vector<std::string>& rx_live);

void show_config_editor(GtkWindow* parent, GdkPixbuf* icon, unsigned port);

}  // namespace vcgtk
