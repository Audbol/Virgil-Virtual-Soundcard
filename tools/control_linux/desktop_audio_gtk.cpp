#include "desktop_audio_gtk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace dagtk {

const char* const kHelpText =
    "Desktop audio sends sound from the programs on this computer to Virgil's Dante transmit channels, with no "
    "driver and no extra software. It works with PulseAudio and with PipeWire (pipewire-pulse), the sound "
    "systems of current Linux desktops.\n\n"
    "• Output device: everything that plays on that device (for example your speakers), captured like a "
    "recording of it. \"Default\" follows the output chosen in the system's sound settings.\n"
    "• App: one program, such as Firefox or Spotify, with every sound stream it opens. The list shows the "
    "apps that have a stream open now; you can also type a name (as the sound settings show it) or the "
    "program's file name.\n"
    "• All apps except Virgil: every app's streams, without what Virgil Control itself plays (no echo when "
    "you also listen to Dante on this computer).\n\n"
    "Each source goes to a pair of transmit channels (or one, mixed to mono); sources on the same channels are "
    "mixed. Route those channels in Dante Controller like any other.\n\n"
    "Dante receive channels can play on an output device too. Do not play them on a device you also capture: "
    "they would echo back into Dante.\n\n"
    "Captured audio still plays on its own device. To hear it only through Dante, let those apps play on an "
    "output you do not listen to, for example a virtual one made with:\n"
    "    pactl load-module module-null-sink sink_name=dante sink_properties=device.description=Dante\n\n"
    "The audio is converted to Virgil's sample rate and follows the small difference between the clock of the "
    "computer's sound hardware and the Dante clock. It adds roughly 60 ms of latency in each direction, so use "
    "it for music, video calls and system sounds, not for live monitoring.\n\n"
    "Desktop audio runs while Virgil Control runs (it starts at login) and pauses while Virgil is stopped. "
    "Settings are saved in ~/.config/virgil/desktop-audio.conf.";

namespace {

struct Inventory {
  bool ok = false;
  std::string why;
  std::vector<desk::Device> devices;
  std::vector<desk::App> apps;
};

// Devices and apps, listed on a worker thread (connecting to the sound
// server may take a moment). `done` runs on the GTK thread.
void load_inventory(desk::Engine& engine, std::function<void(std::shared_ptr<Inventory>)> done) {
  desk::Backend* be = &engine.backend();
  std::thread([be, done] {
    auto inv = std::make_shared<Inventory>();
    inv->ok = be->connect(&inv->why);
    if (inv->ok) {
      inv->devices = be->outputs();
      inv->apps = be->apps();
    }
    auto* cb = new std::function<void()>([done, inv] { done(inv); });
    g_idle_add(
        +[](gpointer p) -> gboolean {
          auto* f = static_cast<std::function<void()>*>(p);
          (*f)();
          delete f;
          return G_SOURCE_REMOVE;
        },
        cb);
  }).detach();
}

std::string fmtd(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
std::string gain_text(double db) {
  if (db <= vc::kMinGainDb) return "off";
  if (db == 0) return "0 dB";
  return fmtd(db == std::floor(db) ? "%+.0f dB" : "%+.1f dB", db);
}

float lin_to_pos(float lin) {
  if (!(lin > 0)) return 0;
  const float db = 20.f * std::log10(lin);
  return std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
}

std::string device_name(const std::string& id, const std::string& saved, const Inventory* inv) {
  if (id.empty() || id == "default") return "Default output device";
  if (inv)
    for (const auto& e : inv->devices)
      if (e.id == id) return e.name;
  return saved.empty() ? id : saved;
}

std::string app_name(const std::string& id, const Inventory* inv) {
  if (inv)
    for (const auto& a : inv->apps)
      if (a.id == id) return a.name;
  return id;
}

std::string source_label(const vc::DesktopAudioSource& s, const Inventory* inv) {
  switch (s.type) {
    case vc::DesktopAudioSource::kApp: return "App: " + app_name(s.app, inv);
    case vc::DesktopAudioSource::kAllApps: return "All apps except Virgil";
    default:
      return (s.device == "default" || s.device.empty() ? "" : "Output: ") + device_name(s.device, s.name, inv);
  }
}

std::string channels_short(unsigned first, bool mono) {
  char b[48];
  if (mono) std::snprintf(b, sizeof b, "%u (mono)", first);
  else std::snprintf(b, sizeof b, "%u + %u", first, first + 1);
  return b;
}

// Stereo pairs from every channel, then single channels (mono). Item id:
// "<first>:<mono>". A configured choice Virgil does not have (now) is kept.
void fill_channels(GtkComboBoxText* combo, unsigned count, const std::vector<std::string>& names, unsigned first,
                   bool mono) {
  gtk_combo_box_text_remove_all(combo);
  const bool known = count > 0;
  if (!known) count = 128;  // Virgil not reachable: anything goes
  auto name = [&](unsigned ch) -> std::string { return ch >= 1 && ch <= names.size() ? names[ch - 1] : std::string(); };
  std::string sel;
  auto add = [&](unsigned f, bool m, const std::string& extra) {
    std::string t = channels_short(f, m);
    if (!extra.empty()) t += "   " + extra;
    const std::string id = std::to_string(f) + ":" + (m ? "1" : "0");
    gtk_combo_box_text_append(combo, id.c_str(), t.c_str());
    if (f == first && m == mono) sel = id;
  };
  for (unsigned f = 1; f + 1 <= count; ++f) {
    const std::string a = name(f), b = name(f + 1);
    add(f, false, a.empty() && b.empty() ? "" : "(" + a + " / " + b + ")");
  }
  for (unsigned f = 1; f <= count; ++f) {
    const std::string a = name(f);
    add(f, true, a.empty() ? "" : "(" + a + ")");
  }
  if (sel.empty() && first >= 1) {
    add(first, mono, known ? "(not available)" : "");
    sel = std::to_string(first) + ":" + (mono ? "1" : "0");
  }
  if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), sel.c_str())) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

bool combo_channels(GtkComboBox* combo, unsigned* first, bool* mono) {
  const char* id = gtk_combo_box_get_active_id(combo);
  if (!id) return false;
  unsigned f = 0, m = 0;
  if (std::sscanf(id, "%u:%u", &f, &m) != 2 || f < 1) return false;
  *first = f;
  *mono = m != 0;
  return true;
}

// Output device combo: Default, then every device; a configured one that is
// not there stays, marked. Item id = device id.
void fill_devices(GtkComboBoxText* combo, const Inventory* inv, const std::string& cur, const std::string& cur_name) {
  gtk_combo_box_text_remove_all(combo);
  gtk_combo_box_text_append(combo, "default", "Default output device (follows the system)");
  bool found = cur.empty() || cur == "default";
  if (inv)
    for (const auto& e : inv->devices) {
      gtk_combo_box_text_append(combo, e.id.c_str(), e.name.c_str());
      if (e.id == cur) found = true;
    }
  if (!found)
    gtk_combo_box_text_append(combo, cur.c_str(),
                              ((cur_name.empty() ? cur : cur_name) + (inv ? "  (not connected)" : "")).c_str());
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), cur.empty() ? "default" : cur.c_str());
}

GtkWidget* hlabel(const char* text, float xalign = 0) {
  GtkWidget* l = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(l), xalign);
  return l;
}

void set_markup(GtkWidget* label, const std::string& text, const char* color) {
  char* m = color ? g_markup_printf_escaped("<span foreground='%s'>%s</span>", color, text.c_str())
                  : g_markup_escape_text(text.c_str(), -1);
  gtk_label_set_markup(GTK_LABEL(label), m);
  g_free(m);
}

const char* const kRed = "#D2553F";

GtkWidget* meter() {
  GtkWidget* m = gtk_level_bar_new_for_interval(0, 1);
  GtkLevelBar* b = GTK_LEVEL_BAR(m);
  gtk_level_bar_remove_offset_value(b, GTK_LEVEL_BAR_OFFSET_LOW);
  gtk_level_bar_remove_offset_value(b, GTK_LEVEL_BAR_OFFSET_HIGH);
  gtk_level_bar_remove_offset_value(b, GTK_LEVEL_BAR_OFFSET_FULL);
  // -60..0 dBFS: green, yellow above -18 dBFS, red above -6.
  gtk_level_bar_add_offset_value(b, "da-ok", 0.7);
  gtk_level_bar_add_offset_value(b, "da-warn", 0.9);
  gtk_level_bar_add_offset_value(b, "da-hot", 1.0);
  gtk_style_context_add_class(gtk_widget_get_style_context(m), "da-meter");
  gtk_widget_set_valign(m, GTK_ALIGN_CENTER);
  return m;
}

void install_css() {
  static bool done = false;
  if (done) return;
  done = true;
  GtkCssProvider* p = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      p,
      "levelbar.da-meter trough { min-height: 6px; padding: 0; border: none; background-color: #2A2E34; }"
      "levelbar.da-meter block { min-height: 6px; border: none; border-radius: 1px; }"
      "levelbar.da-meter block.empty { background-color: transparent; }"
      "levelbar.da-meter block.filled.da-ok { background-color: #4EA85A; }"
      "levelbar.da-meter block.filled.da-warn { background-color: #D9A93F; }"
      "levelbar.da-meter block.filled.da-hot { background-color: #D2553F; }"
      "list.da-list row { padding: 3px 6px; }",
      -1, nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(p);
}

// ---- the source dialog ------------------------------------------------------------------

struct SourceUi {
  GtkWidget *d, *dev, *devcombo, *app, *appcombo, *refresh, *apphint, *all, *ch, *gain, *mute, *err;
  vc::DesktopAudioSource src;
  std::shared_ptr<Inventory> inv;
  desk::Capabilities caps;
};

void source_enable(SourceUi& s) {
  const bool dev = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.dev));
  const bool app = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.app));
  gtk_widget_set_sensitive(s.devcombo, dev);
  gtk_widget_set_sensitive(s.appcombo, app && s.caps.app_capture);
  gtk_widget_set_sensitive(s.refresh, app && s.caps.app_capture);
  gtk_widget_set_sensitive(s.app, s.caps.app_capture || app);
  gtk_widget_set_sensitive(s.all, s.caps.all_apps || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.all)));
}

void source_fill_apps(SourceUi& s) {
  GtkComboBoxText* combo = GTK_COMBO_BOX_TEXT(s.appcombo);
  GtkEntry* entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s.appcombo)));
  const std::string keep = gtk_entry_get_text(entry);
  gtk_combo_box_text_remove_all(combo);
  std::string playing;
  if (s.inv)
    for (const auto& a : s.inv->apps) {
      gtk_combo_box_text_append(combo, a.id.c_str(), a.id.c_str());
      if (a.playing) playing += (playing.empty() ? "" : ", ") + a.name;
    }
  gtk_entry_set_text(entry, keep.c_str());
  std::string hint;
  if (!s.caps.app_capture) hint = s.caps.app_hint;
  else if (!s.inv) hint = "Looking for apps…";
  else if (!s.inv->ok) hint = s.inv->why;
  else
    hint = (playing.empty() ? std::string("No app is playing sound right now.") : "Playing now: " + playing + ".") +
           " Or type the app's name, e.g. Firefox.";
  gtk_label_set_text(GTK_LABEL(s.apphint), hint.c_str());
}

// Runs the (modal) source dialog; true and *src when OK was pressed.
// *inv: the device / app lists to start with, and the refreshed ones after.
bool edit_source_dialog(GtkWindow* parent, desk::Engine& engine, vc::DesktopAudioSource* src,
                        std::shared_ptr<Inventory>* inv, const ChannelInfo& ch) {
  auto s = std::make_shared<SourceUi>();
  s->src = *src;
  s->inv = *inv;
  s->caps = engine.backend().capabilities();
  GtkWidget* d = s->d = gtk_dialog_new_with_buttons("Desktop audio source", parent,
                                                    GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
  gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
  g_object_ref(d);  // it may be destroyed with its parent while it runs
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
  int row = 0;

  s->dev = gtk_radio_button_new_with_label(nullptr, "Everything an output device plays");
  gtk_grid_attach(GTK_GRID(grid), s->dev, 0, row++, 3, 1);
  s->devcombo = gtk_combo_box_text_new();
  gtk_widget_set_margin_start(s->devcombo, 22);
  fill_devices(GTK_COMBO_BOX_TEXT(s->devcombo), s->inv.get(), s->src.device, s->src.name);
  gtk_grid_attach(GTK_GRID(grid), s->devcombo, 0, row++, 3, 1);

  s->app = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s->dev), "One app (every stream it plays)");
  gtk_widget_set_margin_top(s->app, 6);
  gtk_grid_attach(GTK_GRID(grid), s->app, 0, row++, 3, 1);
  s->appcombo = gtk_combo_box_text_new_with_entry();
  gtk_widget_set_margin_start(s->appcombo, 22);
  gtk_widget_set_hexpand(s->appcombo, TRUE);
  GtkEntry* app_entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s->appcombo)));
  gtk_entry_set_max_length(app_entry, 120);
  gtk_entry_set_text(app_entry, s->src.app.c_str());
  gtk_entry_set_activates_default(app_entry, TRUE);
  gtk_grid_attach(GTK_GRID(grid), s->appcombo, 0, row, 2, 1);
  s->refresh = gtk_button_new_with_label("Refresh");
  gtk_grid_attach(GTK_GRID(grid), s->refresh, 2, row++, 1, 1);
  s->apphint = hlabel("");
  gtk_label_set_line_wrap(GTK_LABEL(s->apphint), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(s->apphint), 60);
  gtk_widget_set_margin_start(s->apphint, 22);
  gtk_style_context_add_class(gtk_widget_get_style_context(s->apphint), "dim-label");
  gtk_grid_attach(GTK_GRID(grid), s->apphint, 0, row++, 3, 1);

  s->all = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(s->dev),
                                                       "All apps except Virgil (no echo of what Virgil itself plays)");
  gtk_widget_set_margin_top(s->all, 6);
  gtk_grid_attach(GTK_GRID(grid), s->all, 0, row++, 3, 1);

  GtkWidget* chl = hlabel("Virgil transmit channels:");
  gtk_widget_set_margin_top(chl, 10);
  gtk_grid_attach(GTK_GRID(grid), chl, 0, row, 1, 1);
  s->ch = gtk_combo_box_text_new();
  gtk_widget_set_margin_top(s->ch, 10);
  fill_channels(GTK_COMBO_BOX_TEXT(s->ch), ch.tx, ch.tx_names, s->src.channel, s->src.mono);
  gtk_grid_attach(GTK_GRID(grid), s->ch, 1, row++, 2, 1);
  gtk_grid_attach(GTK_GRID(grid), hlabel("Gain (dB):"), 0, row, 1, 1);
  GtkWidget* gbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  s->gain = gtk_spin_button_new_with_range(vc::kMinGainDb, vc::kMaxGainDb, 0.5);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s->gain), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->gain), s->src.gain_db);
  gtk_entry_set_activates_default(GTK_ENTRY(s->gain), TRUE);
  s->mute = gtk_check_button_new_with_label("Mute");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->mute), s->src.mute);
  gtk_box_pack_start(GTK_BOX(gbox), s->gain, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(gbox), s->mute, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), gbox, 1, row++, 2, 1);
  s->err = hlabel("");
  gtk_label_set_line_wrap(GTK_LABEL(s->err), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(s->err), 60);
  gtk_grid_attach(GTK_GRID(grid), s->err, 0, row++, 3, 1);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);

  GtkWidget* radio = s->src.type == vc::DesktopAudioSource::kApp      ? s->app
                     : s->src.type == vc::DesktopAudioSource::kAllApps ? s->all
                                                                       : s->dev;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio), TRUE);
  source_fill_apps(*s);
  source_enable(*s);

  auto* sp = new std::shared_ptr<SourceUi>(s);
  g_signal_connect_data(
      d, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {}), sp,
      +[](gpointer p, GClosure*) { delete static_cast<std::shared_ptr<SourceUi>*>(p); }, GConnectFlags(0));
  auto on_toggle = +[](GtkToggleButton*, gpointer p) { source_enable(**static_cast<std::shared_ptr<SourceUi>*>(p)); };
  for (GtkWidget* r : {s->dev, s->app, s->all}) g_signal_connect(r, "toggled", G_CALLBACK(on_toggle), sp);
  g_signal_connect(s->devcombo, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer p) {
                     auto& u = **static_cast<std::shared_ptr<SourceUi>*>(p);
                     gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u.dev), TRUE);
                   }),
                   sp);
  // Weak: a refresh may come back after the dialog closed.
  std::weak_ptr<SourceUi> weak = s;
  desk::Engine* eng = &engine;
  auto refresh = [weak, eng] {
    if (auto u = weak.lock()) gtk_label_set_text(GTK_LABEL(u->apphint), "Looking for apps…");
    load_inventory(*eng, [weak](std::shared_ptr<Inventory> inv) {
      auto u = weak.lock();
      if (!u || !u->d) return;
      const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(u->devcombo));
      const std::string cur = id ? id : u->src.device;
      u->inv = inv;
      fill_devices(GTK_COMBO_BOX_TEXT(u->devcombo), inv.get(), cur, u->src.name);
      source_fill_apps(*u);
    });
  };
  auto* rf = new std::function<void()>(refresh);
  g_signal_connect_data(
      s->refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { (*static_cast<std::function<void()>*>(p))(); }),
      rf, +[](gpointer p, GClosure*) { delete static_cast<std::function<void()>*>(p); }, GConnectFlags(0));
  if (!s->inv) refresh();

  gtk_widget_show_all(d);
  bool ok = false;
  while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
    vc::DesktopAudioSource r = s->src;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->app))) {
      std::string a = gtk_entry_get_text(app_entry);
      while (!a.empty() && (a.back() == ' ' || a.back() == '"')) a.pop_back();
      while (!a.empty() && (a.front() == ' ' || a.front() == '"')) a.erase(a.begin());
      if (a.empty()) {
        set_markup(s->err, "Choose an app, or type its name (for example Firefox).", kRed);
        continue;
      }
      r.type = vc::DesktopAudioSource::kApp;
      r.app = a;
    } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->all))) {
      r.type = vc::DesktopAudioSource::kAllApps;
    } else {
      r.type = vc::DesktopAudioSource::kDevice;
      const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->devcombo));
      r.device = id ? id : "default";
      r.name = r.device == "default" ? std::string() : device_name(r.device, r.name, s->inv.get());
    }
    if (r.type != vc::DesktopAudioSource::kApp) r.app.clear();
    if (r.type != vc::DesktopAudioSource::kDevice) {
      r.device = "default";
      r.name.clear();
    }
    if (!combo_channels(GTK_COMBO_BOX(s->ch), &r.channel, &r.mono)) {
      set_markup(s->err, "Choose the transmit channels.", kRed);
      continue;
    }
    gtk_spin_button_update(GTK_SPIN_BUTTON(s->gain));
    r.gain_db = gtk_spin_button_get_value(GTK_SPIN_BUTTON(s->gain));
    r.mute = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->mute));
    *src = r;
    ok = true;
    break;
  }
  if (s->inv) *inv = s->inv;  // refreshed in there
  s->d = nullptr;
  gtk_widget_destroy(d);
  g_object_unref(d);
  return ok;
}

// ---- the main window --------------------------------------------------------------------

struct Row {
  GtkWidget *row, *label, *channels, *gain, *meter, *status;
  float level = 0;
  std::string shown;
};

struct Main {
  GtkWidget *w = nullptr, *enable, *status, *list, *add, *edit, *remove;
  GtkWidget *out_enable, *out_ch, *out_gain, *out_dev, *out_meter, *out_state;
  desk::Engine* engine = nullptr;
  ChannelSource channels;
  vc::DesktopAudioSettings cfg;
  std::shared_ptr<Inventory> inv;
  ChannelInfo ch;
  std::vector<Row> rows;
  GtkSizeGroup* cols[4];
  std::string shown_status, shown_out, save_error;
  float out_level = 0;
  gint64 last_tick = 0, last_channels = 0;
  bool filling = false;
  guint timer = 0;
};

Main* g_main = nullptr;  // the open window, if any

int selected(Main& m) {
  GtkListBoxRow* r = gtk_list_box_get_selected_row(GTK_LIST_BOX(m.list));
  return r ? gtk_list_box_row_get_index(r) : -1;
}

void update_buttons(Main& m) {
  const bool sel = selected(m) >= 0;
  gtk_widget_set_sensitive(m.edit, sel);
  gtk_widget_set_sensitive(m.remove, sel);
  gtk_widget_set_sensitive(m.add, m.cfg.sources.size() < vc::kMaxDesktopSources);
}

GtkWidget* cell(Main& m, int col, GtkWidget* w, bool expand = false) {
  gtk_size_group_add_widget(m.cols[col], w);
  gtk_widget_set_hexpand(w, expand);
  return w;
}

void fill_list(Main& m) {
  const int keep = selected(m);
  GList* kids = gtk_container_get_children(GTK_CONTAINER(m.list));
  for (GList* k = kids; k; k = k->next) gtk_widget_destroy(GTK_WIDGET(k->data));
  g_list_free(kids);
  m.rows.assign(m.cfg.sources.size(), Row());
  for (size_t i = 0; i < m.cfg.sources.size(); ++i) {
    const vc::DesktopAudioSource& s = m.cfg.sources[i];
    Row& r = m.rows[i];
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    r.label = cell(m, 0, hlabel(source_label(s, m.inv.get()).c_str()));
    gtk_label_set_ellipsize(GTK_LABEL(r.label), PANGO_ELLIPSIZE_END);
    r.channels = cell(m, 1, hlabel(channels_short(s.channel, s.mono).c_str()));
    r.gain = cell(m, 2, hlabel(s.mute ? "muted" : gain_text(s.gain_db).c_str()));
    r.meter = cell(m, 3, meter());
    r.status = hlabel("");
    gtk_label_set_ellipsize(GTK_LABEL(r.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(r.status, TRUE);
    for (GtkWidget* c : {r.label, r.channels, r.gain, r.meter, r.status}) gtk_box_pack_start(GTK_BOX(box), c, FALSE, TRUE, 0);
    gtk_widget_set_hexpand(r.status, TRUE);
    gtk_box_set_child_packing(GTK_BOX(box), r.status, TRUE, TRUE, 0, GTK_PACK_START);
    gtk_list_box_insert(GTK_LIST_BOX(m.list), box, -1);
    r.row = gtk_widget_get_parent(box);
  }
  gtk_widget_show_all(m.list);
  if (keep >= 0 && size_t(keep) < m.rows.size())
    gtk_list_box_select_row(GTK_LIST_BOX(m.list), GTK_LIST_BOX_ROW(m.rows[size_t(keep)].row));
  update_buttons(m);
}

void fill_output(Main& m) {
  m.filling = true;
  const vc::DesktopAudioOutput& o = m.cfg.output;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m.out_enable), o.enabled);
  fill_channels(GTK_COMBO_BOX_TEXT(m.out_ch), m.ch.rx, m.ch.rx_names, o.channel, o.mono);
  fill_devices(GTK_COMBO_BOX_TEXT(m.out_dev), m.inv.get(), o.device, o.name);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(m.out_gain), o.gain_db);
  m.filling = false;
}

// Applies the edited settings right away and saves them.
void commit(Main& m) {
  m.engine->apply(m.cfg);
  std::string err;
  m.save_error = desk::save_settings(desk::settings_path(), m.cfg, &err) ? std::string() : err;
  m.shown_status.clear();  // redone on the next tick
}

void edit_source(Main& m, int index) {
  vc::DesktopAudioSource src;
  if (index >= 0 && size_t(index) < m.cfg.sources.size()) {
    src = m.cfg.sources[size_t(index)];
  } else {
    index = -1;
    // A new source: the first odd stereo pair no other source uses.
    const unsigned count = m.ch.tx ? m.ch.tx : 128;
    src.channel = 1;
    for (unsigned first = 1; first + 1 <= count; first += 2) {
      bool clash = false;
      for (const auto& o : m.cfg.sources)
        if (first < o.channel + o.width() && o.channel < first + 2) clash = true;
      if (!clash) {
        src.channel = first;
        break;
      }
    }
  }
  std::shared_ptr<Inventory> inv = m.inv;
  Main* const was = &m;
  const bool ok = edit_source_dialog(GTK_WINDOW(m.w), *m.engine, &src, &inv, m.ch);
  if (g_main != was) return;  // the window was closed meanwhile
  m.inv = inv;
  if (!ok) return;
  if (index < 0) m.cfg.sources.push_back(src);
  else m.cfg.sources[size_t(index)] = src;
  // A new source means the user wants desktop audio on.
  if (index < 0 && !m.cfg.enabled) {
    m.cfg.enabled = true;
    m.filling = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m.enable), TRUE);
    m.filling = false;
  }
  commit(m);
  fill_list(m);
  if (index < 0 && !m.rows.empty()) gtk_list_box_select_row(GTK_LIST_BOX(m.list), GTK_LIST_BOX_ROW(m.rows.back().row));
}

void read_output(Main& m) {
  if (m.filling) return;
  vc::DesktopAudioOutput o = m.cfg.output;
  o.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m.out_enable));
  combo_channels(GTK_COMBO_BOX(m.out_ch), &o.channel, &o.mono);
  if (const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(m.out_dev))) {
    o.device = id;
    o.name = o.device == "default" ? std::string() : device_name(o.device, o.name, m.inv.get());
  }
  o.gain_db = gtk_spin_button_get_value(GTK_SPIN_BUTTON(m.out_gain));
  if (o == m.cfg.output) return;
  m.cfg.output = o;
  if (o.enabled && !m.cfg.enabled) {
    m.cfg.enabled = true;
    m.filling = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m.enable), TRUE);
    m.filling = false;
  }
  commit(m);
}

const char* phase_color(desk::Status::Phase p, bool err) {
  if (err) return kRed;
  switch (p) {
    case desk::Status::kRunning: return "#6DB476";
    case desk::Status::kWaiting: return "#D9A93F";
    default: return nullptr;
  }
}

gboolean tick(gpointer) {
  if (!g_main) return G_SOURCE_REMOVE;
  Main& m = *g_main;
  const gint64 now = g_get_monotonic_time();
  const float dt = m.last_tick ? std::min(0.25f, float(now - m.last_tick) / 1e6f) : 0.f;
  m.last_tick = now;
  const desk::Status st = m.engine->status();

  // Channel names and counts: Virgil's own while connected, else the
  // service's report. Combos follow when they change.
  if (now - m.last_channels > 1000000) {
    m.last_channels = now;
    ChannelInfo c;
    if (st.connected) {
      c.tx = st.tx;
      c.rx = st.rx;
      c.tx_names = st.tx_names;
      c.rx_names = st.rx_names;
    } else if (m.channels) {
      c = m.channels();
    }
    const bool changed = c.rx != m.ch.rx || c.rx_names != m.ch.rx_names;
    m.ch = c;
    if (changed) {
      m.filling = true;
      fill_channels(GTK_COMBO_BOX_TEXT(m.out_ch), m.ch.rx, m.ch.rx_names, m.cfg.output.channel, m.cfg.output.mono);
      m.filling = false;
    }
  }

  // Status line.
  const std::string line = m.save_error.empty() ? st.text : "Not saved: " + m.save_error;
  const bool err = st.phase == desk::Status::kError || !m.save_error.empty();
  const std::string key = line + (err ? "!" : "") + char('0' + st.phase);
  if (key != m.shown_status) {
    m.shown_status = key;
    set_markup(m.status, line, phase_color(st.phase, err));
  }

  // Sources: status text and meters.
  for (size_t i = 0; i < m.rows.size(); ++i) {
    const desk::SourceStatus ss = i < st.sources.size() ? st.sources[i] : desk::SourceStatus();
    Row& r = m.rows[i];
    const std::string t = (ss.error ? "⚠ " : "") + ss.text;
    if (t != r.shown) {
      r.shown = t;
      set_markup(r.status, t, ss.error ? kRed : nullptr);
      gtk_widget_set_tooltip_text(r.status, ss.text.c_str());
    }
    const float v = lin_to_pos(ss.peak);
    const float shown = std::max(v, r.level - dt * 0.6f);
    if (shown != r.level) {
      r.level = shown;
      gtk_level_bar_set_value(GTK_LEVEL_BAR(r.meter), std::max(0.f, shown));
    }
  }

  // Output.
  m.out_level = std::max(lin_to_pos(st.output.peak), m.out_level - dt * 0.6f);
  gtk_level_bar_set_value(GTK_LEVEL_BAR(m.out_meter), std::max(0.f, m.out_level));
  const std::string ot = m.cfg.output.enabled ? st.output.text : std::string();
  const std::string okey = ot + (st.output.error ? "!" : "");
  if (okey != m.shown_out) {
    m.shown_out = okey;
    set_markup(m.out_state, ot, st.output.error ? kRed : nullptr);
    gtk_widget_set_tooltip_text(m.out_state, ot.c_str());
  }
  return G_SOURCE_CONTINUE;
}

void reload_inventory(Main& m) {
  load_inventory(*m.engine, [](std::shared_ptr<Inventory> inv) {
    if (!g_main) return;
    g_main->inv = inv;
    fill_output(*g_main);
    fill_list(*g_main);  // device and app names
  });
}

void on_destroy(GtkWidget*, gpointer) {
  if (!g_main) return;
  if (g_main->timer) g_source_remove(g_main->timer);
  for (GtkSizeGroup* g : g_main->cols) g_object_unref(g);
  delete g_main;
  g_main = nullptr;
}

void show_help(GtkWindow* parent) {
  GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Desktop audio");
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", kHelpText);
  gtk_window_set_title(GTK_WINDOW(d), "Desktop audio");
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

GtkWidget* frame(const char* title, GtkWidget* child) {
  GtkWidget* f = gtk_frame_new(nullptr);
  GtkWidget* l = gtk_label_new(nullptr);
  char* mk = g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(l), mk);
  g_free(mk);
  gtk_frame_set_label_widget(GTK_FRAME(f), l);
  gtk_container_set_border_width(GTK_CONTAINER(child), 10);
  gtk_container_add(GTK_CONTAINER(f), child);
  return f;
}

}  // namespace

void show(GtkWindow* parent, GdkPixbuf* icon, desk::Engine& engine, ChannelSource channels) {
  if (g_main) {
    gtk_window_present(GTK_WINDOW(g_main->w));
    return;
  }
  install_css();
  g_main = new Main;
  Main& m = *g_main;
  m.engine = &engine;
  m.channels = std::move(channels);
  m.cfg = engine.settings();
  if (m.channels) m.ch = m.channels();
  m.last_channels = g_get_monotonic_time();
  for (GtkSizeGroup*& g : m.cols) g = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

  GtkWidget* w = m.w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(w), "Desktop audio");
  if (parent) gtk_window_set_transient_for(GTK_WINDOW(w), parent);
  if (icon) gtk_window_set_icon(GTK_WINDOW(w), icon);
  else gtk_window_set_icon_name(GTK_WINDOW(w), "virgil");
  gtk_window_set_default_size(GTK_WINDOW(w), 780, -1);
  g_signal_connect(w, "destroy", G_CALLBACK(on_destroy), nullptr);

  GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(v), 14);
  gtk_container_add(GTK_CONTAINER(w), v);

  m.enable = gtk_check_button_new_with_label("Send this computer’s audio to Dante (desktop audio)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m.enable), m.cfg.enabled);
  gtk_box_pack_start(GTK_BOX(v), m.enable, FALSE, FALSE, 0);
  m.status = hlabel("");
  gtk_label_set_line_wrap(GTK_LABEL(m.status), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(m.status), 90);
  gtk_box_pack_start(GTK_BOX(v), m.status, FALSE, FALSE, 0);

  // Sources.
  GtkWidget* sv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_start(head, 7);
  gtk_widget_set_margin_end(head, 7);
  const char* heads[] = {"Source", "Channels", "Gain", "Level"};
  const int widths[] = {230, 80, 60, 90};  // the size groups take the widest of header and rows
  for (int c = 0; c < 4; ++c) {
    GtkWidget* l = hlabel(heads[c]);
    gtk_widget_set_size_request(l, widths[c], -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
    gtk_box_pack_start(GTK_BOX(head), cell(m, c, l), FALSE, TRUE, 0);
  }
  GtkWidget* sh = hlabel("Status");
  gtk_style_context_add_class(gtk_widget_get_style_context(sh), "dim-label");
  gtk_box_pack_start(GTK_BOX(head), sh, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(sv), head, FALSE, FALSE, 0);
  m.list = gtk_list_box_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(m.list), "da-list");
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(m.list), FALSE);
  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
  gtk_widget_set_size_request(scroll, -1, 150);
  gtk_container_add(GTK_CONTAINER(scroll), m.list);
  gtk_box_pack_start(GTK_BOX(sv), scroll, TRUE, TRUE, 0);
  GtkWidget* bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  m.add = gtk_button_new_with_mnemonic("_Add…");
  m.edit = gtk_button_new_with_mnemonic("_Edit…");
  m.remove = gtk_button_new_with_mnemonic("_Remove");
  for (GtkWidget* b : {m.add, m.edit, m.remove}) gtk_box_pack_start(GTK_BOX(bb), b, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sv), bb, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), frame("This computer’s audio to Virgil’s transmit channels", sv), TRUE, TRUE, 0);

  // Output.
  GtkWidget* og = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(og), 6);
  gtk_grid_set_column_spacing(GTK_GRID(og), 10);
  m.out_enable = gtk_check_button_new_with_label("Play receive channels");
  gtk_grid_attach(GTK_GRID(og), m.out_enable, 0, 0, 1, 1);
  m.out_ch = gtk_combo_box_text_new();
  gtk_grid_attach(GTK_GRID(og), m.out_ch, 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(og), hlabel("Gain (dB):", 1), 2, 0, 1, 1);
  m.out_gain = gtk_spin_button_new_with_range(vc::kMinGainDb, vc::kMaxGainDb, 0.5);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(m.out_gain), 1);
  gtk_grid_attach(GTK_GRID(og), m.out_gain, 3, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(og), hlabel("on", 1), 0, 1, 1, 1);
  m.out_dev = gtk_combo_box_text_new();
  gtk_widget_set_hexpand(m.out_dev, TRUE);
  gtk_grid_attach(GTK_GRID(og), m.out_dev, 1, 1, 3, 1);
  m.out_meter = meter();
  gtk_widget_set_size_request(m.out_meter, 90, -1);
  gtk_grid_attach(GTK_GRID(og), m.out_meter, 0, 2, 1, 1);
  m.out_state = hlabel("");
  gtk_label_set_ellipsize(GTK_LABEL(m.out_state), PANGO_ELLIPSIZE_END);
  gtk_grid_attach(GTK_GRID(og), m.out_state, 1, 2, 3, 1);
  gtk_box_pack_start(GTK_BOX(v), frame("Dante receive channels to an output device", og), FALSE, FALSE, 0);

  GtkWidget* note = hlabel(
      "Captured audio still plays on the device it was sent to. To hear it only through Dante, play those apps on "
      "an output you do not listen to (Help explains how to make a virtual one).");
  gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(note), 100);
  gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
  gtk_box_pack_start(GTK_BOX(v), note, FALSE, FALSE, 0);

  GtkWidget* foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* help = gtk_button_new_with_mnemonic("_Help");
  GtkWidget* close = gtk_button_new_with_mnemonic("_Close");
  gtk_box_pack_start(GTK_BOX(foot), help, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(foot), close, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), foot, FALSE, FALSE, 0);

  // Signals.
  g_signal_connect(m.enable, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer) {
                     if (!g_main || g_main->filling) return;
                     g_main->cfg.enabled = gtk_toggle_button_get_active(b);
                     commit(*g_main);
                   }),
                   nullptr);
  g_signal_connect(m.add, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_main && g_main->cfg.sources.size() < vc::kMaxDesktopSources) edit_source(*g_main, -1);
                   }),
                   nullptr);
  g_signal_connect(m.edit, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_main && selected(*g_main) >= 0) edit_source(*g_main, selected(*g_main));
                   }),
                   nullptr);
  auto remove = +[](GtkButton*, gpointer) {
    if (!g_main) return;
    const int i = selected(*g_main);
    if (i < 0 || size_t(i) >= g_main->cfg.sources.size()) return;
    g_main->cfg.sources.erase(g_main->cfg.sources.begin() + i);
    commit(*g_main);
    fill_list(*g_main);
  };
  g_signal_connect(m.remove, "clicked", G_CALLBACK(remove), nullptr);
  g_signal_connect(m.list, "row-selected",
                   G_CALLBACK(+[](GtkListBox*, GtkListBoxRow*, gpointer) {
                     if (g_main) update_buttons(*g_main);
                   }),
                   nullptr);
  g_signal_connect(m.list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* r, gpointer) {
                     if (g_main && r) edit_source(*g_main, gtk_list_box_row_get_index(r));
                   }),
                   nullptr);
  g_signal_connect(m.list, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, gpointer) -> gboolean {
                     if (!g_main || e->keyval != GDK_KEY_Delete) return FALSE;
                     gtk_button_clicked(GTK_BUTTON(g_main->remove));
                     return TRUE;
                   }),
                   nullptr);
  auto out_changed = +[](GtkWidget*, gpointer) {
    if (g_main) read_output(*g_main);
  };
  g_signal_connect(m.out_enable, "toggled", G_CALLBACK(out_changed), nullptr);
  g_signal_connect(m.out_ch, "changed", G_CALLBACK(out_changed), nullptr);
  g_signal_connect(m.out_dev, "changed", G_CALLBACK(out_changed), nullptr);
  g_signal_connect(m.out_gain, "value-changed", G_CALLBACK(out_changed), nullptr);
  g_signal_connect(help, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_main) show_help(GTK_WINDOW(g_main->w));
                   }),
                   nullptr);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), w);

  fill_list(m);
  fill_output(m);
  tick(nullptr);
  m.timer = g_timeout_add(60, tick, nullptr);
  gtk_widget_show_all(w);
  reload_inventory(m);
}

}  // namespace dagtk
