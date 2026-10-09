// Virgil Control for Linux (desktop PCs and Raspberry Pi): tray icon, status
// window with level meters and a settings dialog, drawn like the Windows app.
// It talks to virgild over its local control API (127.0.0.1:8480).
//
//   virgil-control          open the window (and the tray icon)
//   virgil-control --tray   start in the tray only (used at login)
//
// The tray icon uses AppIndicator (StatusNotifierItem, loaded at run time, so
// the package needs no extra dependency) and falls back to the classic
// system-tray icon. Without either, closing the window quits.
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "settings_model.h"
#include "view.h"

#ifndef VIRGIL_VERSION
#define VIRGIL_VERSION "dev"
#endif

namespace {

// ---- state -------------------------------------------------------------------------------

unsigned g_port = 8480;
GtkApplication* g_app;
GtkWidget* g_window;
GtkWidget* g_area;
vc::StatusView g_view;
std::mutex g_mutex;
vc::Status g_latest;  // guarded by g_mutex
std::atomic<bool> g_visible{false};
std::atomic<bool> g_quit{false};
bool g_have_tray = false;
bool g_start_hidden = false;
GdkPixbuf* g_logo;
GtkWidget* g_autostart_item;

uint64_t now_ms() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string exe_path() {
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return "virgil-control";
  buf[n] = 0;
  return buf;
}
std::string exe_dir() {
  const std::string p = exe_path();
  return p.substr(0, p.find_last_of('/'));
}
bool exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}
bool portable() { return exe_dir().rfind("/usr/", 0) != 0 && exists(exe_dir() + "/virgild"); }

// Run a command without a shell; returns its exit status (or -1).
int run(const std::vector<std::string>& argv) {
  const pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    std::vector<char*> a;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
void spawn_detached(const std::vector<std::string>& argv) {
  const pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (fork() != 0) _exit(0);
    std::vector<char*> a;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
}

void message(GtkMessageType type, const std::string& text) {
  GtkWidget* d = gtk_message_dialog_new(g_window && gtk_widget_get_visible(g_window) ? GTK_WINDOW(g_window) : nullptr,
                                        GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", text.c_str());
  gtk_window_set_title(GTK_WINDOW(d), "Virgil");
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

void notify(const std::string& title, const std::string& body) {
  GNotification* n = g_notification_new(title.c_str());
  g_notification_set_body(n, body.c_str());
  g_application_send_notification(G_APPLICATION(g_app), "virgil-status", n);
  g_object_unref(n);
}

// ---- Cairo canvas ------------------------------------------------------------------------

class CairoCanvas : public vc::Canvas {
 public:
  explicit CairoCanvas(cairo_t* cr) : cr_(cr) {}

  void fill(const vc::Rect& r, unsigned c, float a) override {
    set(c, a);
    cairo_rectangle(cr_, r.l, r.t, r.r - r.l, r.b - r.t);
    cairo_fill(cr_);
  }
  void fill_round(const vc::Rect& r, float rad, unsigned c, float a) override {
    round_path(r, rad);
    set(c, a);
    cairo_fill(cr_);
  }
  void stroke_round(const vc::Rect& r, float rad, unsigned c) override {
    round_path(vc::Rect{r.l + 0.5f, r.t + 0.5f, r.r - 0.5f, r.b - 0.5f}, rad);
    set(c, 1);
    cairo_set_line_width(cr_, 1);
    cairo_stroke(cr_);
  }
  void hline(float x0, float x1, float y, unsigned c) override {
    set(c, 1);
    cairo_set_line_width(cr_, 1);
    const double yy = std::floor(y) + 0.5;
    cairo_move_to(cr_, x0, yy);
    cairo_line_to(cr_, x1, yy);
    cairo_stroke(cr_);
  }
  void dot(float cx, float cy, float rad, unsigned c) override {
    set(c, 1);
    cairo_arc(cr_, cx, cy, rad, 0, 2 * G_PI);
    cairo_fill(cr_);
  }
  void text(const std::string& s, vc::Font f, const vc::Rect& r, unsigned c, vc::Align a) override {
    if (s.empty()) return;
    PangoLayout* l = layout(s, f);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    double x = r.l;
    if (a == vc::Align::Center) x = r.l + (r.r - r.l - w) / 2;
    if (a == vc::Align::Right) x = r.r - w;
    const double y = r.t + (r.b - r.t - h) / 2;
    cairo_save(cr_);
    cairo_rectangle(cr_, r.l, r.t, r.r - r.l, r.b - r.t);
    cairo_clip(cr_);
    set(c, 1);
    cairo_move_to(cr_, x, y);
    pango_cairo_show_layout(cr_, l);
    cairo_restore(cr_);
    g_object_unref(l);
  }
  float text_width(const std::string& s, vc::Font f) override {
    if (s.empty()) return 0;
    PangoLayout* l = layout(s, f);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(l, &w, &h);
    g_object_unref(l);
    return float(w);
  }
  void logo(const vc::Rect& r) override {
    if (!g_logo) return;
    const int w = int(r.r - r.l), h = int(r.b - r.t);
    GdkPixbuf* s = gdk_pixbuf_scale_simple(g_logo, w, h, GDK_INTERP_BILINEAR);
    if (!s) return;
    gdk_cairo_set_source_pixbuf(cr_, s, r.l, r.t);
    cairo_paint(cr_);
    g_object_unref(s);
  }

 private:
  void set(unsigned c, float a) {
    cairo_set_source_rgba(cr_, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
  }
  void round_path(const vc::Rect& r, float rad) {
    const double x = r.l, y = r.t, w = r.r - r.l, h = r.b - r.t;
    cairo_new_sub_path(cr_);
    cairo_arc(cr_, x + w - rad, y + rad, rad, -G_PI / 2, 0);
    cairo_arc(cr_, x + w - rad, y + h - rad, rad, 0, G_PI / 2);
    cairo_arc(cr_, x + rad, y + h - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cr_, x + rad, y + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr_);
  }
  PangoLayout* layout(const std::string& s, vc::Font f) {
    PangoLayout* l = pango_cairo_create_layout(cr_);
    const char* desc = "Sans 9";
    switch (f) {
      case vc::Font::Title: desc = "Sans Semi-Bold 12.75"; break;
      case vc::Font::Big: desc = "Sans Semi-Bold 11.25"; break;
      case vc::Font::Small: desc = "Sans 7.9"; break;
      case vc::Font::Mono: desc = "Monospace 8.25"; break;
      default: desc = "Sans 9"; break;
    }
    PangoFontDescription* fd = pango_font_description_from_string(desc);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, s.c_str(), -1);
    return l;
  }
  cairo_t* cr_;
};

// ---- window ------------------------------------------------------------------------------

void resize_to_view() {
  if (!g_window) return;
  const int w = int(g_view.width()), h = int(g_view.height());
  gtk_widget_set_size_request(g_area, w, h);
  gtk_window_resize(GTK_WINDOW(g_window), w, h);
}

gboolean on_draw(GtkWidget* w, cairo_t* cr, gpointer) {
  CairoCanvas c(cr);
  g_view.paint(c, float(gtk_widget_get_allocated_width(w)), float(gtk_widget_get_allocated_height(w)));
  return TRUE;
}

void set_cursor() {
  GdkWindow* win = gtk_widget_get_window(g_area);
  if (!win) return;
  GdkCursor* cur = g_view.over_button()
                       ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer")
                       : nullptr;
  gdk_window_set_cursor(win, cur);
  if (cur) g_object_unref(cur);
}

void show_settings();
void restart_engine();
void start_service();

gboolean on_motion(GtkWidget*, GdkEventMotion* e, gpointer) {
  if (g_view.hover(float(e->x), float(e->y))) {
    set_cursor();
    gtk_widget_queue_draw(g_area);
  }
  return TRUE;
}
gboolean on_press(GtkWidget*, GdkEventButton* e, gpointer) {
  if (e->button == 1 && g_view.press(float(e->x), float(e->y))) gtk_widget_queue_draw(g_area);
  return TRUE;
}
gboolean on_release(GtkWidget*, GdkEventButton* e, gpointer) {
  if (e->button != 1) return TRUE;
  const vc::Action a = g_view.release(float(e->x), float(e->y));
  gtk_widget_queue_draw(g_area);
  if (a == vc::Action::Settings) show_settings();
  if (a == vc::Action::Restart) restart_engine();
  if (a == vc::Action::StartService) start_service();
  return TRUE;
}

gboolean on_delete(GtkWidget* w, GdkEvent*, gpointer) {
  if (!g_have_tray) return FALSE;  // no tray: closing quits
  gtk_widget_hide(w);
  g_visible = false;
  return TRUE;
}

void show_window() {
  resize_to_view();
  gtk_widget_show_all(g_window);
  gtk_window_present(GTK_WINDOW(g_window));
  g_visible = true;
}

GdkPixbuf* load_logo() {
  const std::vector<std::string> paths = {
      exe_dir() + "/virgil.png",
      exe_dir() + "/../share/icons/hicolor/256x256/apps/virgil.png",
      "/usr/share/icons/hicolor/256x256/apps/virgil.png",
      "/usr/local/share/icons/hicolor/256x256/apps/virgil.png",
  };
  for (const auto& p : paths)
    if (exists(p))
      if (GdkPixbuf* pb = gdk_pixbuf_new_from_file(p.c_str(), nullptr)) return pb;
  return nullptr;
}

void build_window() {
  g_window = gtk_application_window_new(g_app);
  gtk_window_set_title(GTK_WINDOW(g_window), "Virgil");
  gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
  if (g_logo) gtk_window_set_icon(GTK_WINDOW(g_window), g_logo);
  else gtk_window_set_icon_name(GTK_WINDOW(g_window), "virgil");
  g_area = gtk_drawing_area_new();
  gtk_widget_add_events(g_area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                    GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(g_area, "draw", G_CALLBACK(on_draw), nullptr);
  g_signal_connect(g_area, "motion-notify-event", G_CALLBACK(on_motion), nullptr);
  g_signal_connect(g_area, "button-press-event", G_CALLBACK(on_press), nullptr);
  g_signal_connect(g_area, "button-release-event", G_CALLBACK(on_release), nullptr);
  g_signal_connect(g_window, "delete-event", G_CALLBACK(on_delete), nullptr);
  gtk_container_add(GTK_CONTAINER(g_window), g_area);
  resize_to_view();
}

// ---- polling -----------------------------------------------------------------------------

gboolean apply_status(gpointer data) {
  const int event = GPOINTER_TO_INT(data);
  vc::Status s;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    s = g_latest;
  }
  const unsigned old_tx = g_view.status().tx, old_rx = g_view.status().rx;
  g_view.update(s, now_ms());
  if (s.tx != old_tx || s.rx != old_rx) resize_to_view();
  if (g_visible) gtk_widget_queue_draw(g_area);
  if (event == 1) notify("Virgil service stopped", "Audio to and from the Dante network has stopped.");
  if (event == 2) notify("Dante clock lost", "Virgil is running on its own clock; check the network connection.");
  return G_SOURCE_REMOVE;
}

void poll_thread() {
  bool was_reachable = true;
  std::string was_clock;
  uint64_t lost_since = 0;
  bool lost_told = false;
  while (!g_quit) {
    const auto r = vc::http(g_port, "GET", "/api/status");
    vc::Status s;
    if (r.status != 200 || !vc::parse_status(r.body, &s)) s = vc::Status();
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_latest = s;
    }
    int event = 0;
    if (was_reachable && !s.reachable) event = 1;
    if (s.reachable && was_clock == "ptp-locked" && s.clock != "ptp-locked" && !lost_since) lost_since = now_ms();
    if (s.clock == "ptp-locked") lost_since = 0, lost_told = false;
    if (lost_since && !lost_told && now_ms() - lost_since > 5000) {
      lost_told = true;
      event = 2;
    }
    was_reachable = s.reachable;
    was_clock = s.clock;
    g_idle_add(apply_status, GINT_TO_POINTER(event));
    std::this_thread::sleep_for(std::chrono::milliseconds(g_visible ? 80 : 1000));
  }
}

// ---- actions -----------------------------------------------------------------------------

void restart_engine() {
  if (vc::http(g_port, "POST", "/api/restart").status != 200)
    message(GTK_MESSAGE_WARNING, "The Virgil service is not running, so the audio engine cannot be restarted.");
}

void start_service() {
  if (portable()) {
    const std::string dir = exe_dir();
    std::vector<std::string> argv = {dir + "/virgild", "--log", dir + "/virgild.log", "--control-port",
                                     std::to_string(g_port)};
    if (exists(dir + "/virgil.conf")) argv.insert(argv.end(), {"-c", dir + "/virgil.conf"});
    spawn_detached(argv);
    return;
  }
  // Installed: the systemd service. pkexec asks for the password graphically.
  if (run({"pkexec", "systemctl", "start", "virgild"}) != 0)
    message(GTK_MESSAGE_WARNING,
            "Could not start the Virgil service.\n\nIn a terminal: sudo systemctl start virgild\n"
            "and see why with: journalctl -u virgild -n 50");
}

std::string read_command(const std::string& cmd) {
  std::string out;
  if (FILE* p = popen(cmd.c_str(), "r")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
  }
  return out;
}

void show_log() {
  std::string text;
  if (portable()) {
    std::ifstream f(exe_dir() + "/virgild.log");
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    if (text.size() > 200000) text = text.substr(text.size() - 200000);
  } else {
    text = read_command("journalctl -u virgild -n 400 --no-pager 2>&1");
  }
  if (text.empty()) text = "No log available. In a terminal: journalctl -u virgild -n 100";
  GtkWidget* d = gtk_dialog_new_with_buttons("Virgil log", g_window && gtk_widget_get_visible(g_window)
                                                               ? GTK_WINDOW(g_window) : nullptr,
                                             GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, nullptr);
  gtk_window_set_default_size(GTK_WINDOW(d), 860, 520);
  GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
  GtkWidget* tv = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
  GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  gtk_text_buffer_set_text(buf, text.c_str(), -1);
  gtk_container_add(GTK_CONTAINER(sw), tv);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), sw, TRUE, TRUE, 0);
  gtk_widget_show_all(d);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(buf, &end);
  GtkTextMark* m = gtk_text_buffer_create_mark(buf, nullptr, &end, FALSE);
  gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(tv), m, 0, FALSE, 0, 1);
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

void open_browser() {
  spawn_detached({"xdg-open", "http://127.0.0.1:" + std::to_string(g_port) + "/"});
}

void show_about() {
  GtkWidget* d = gtk_about_dialog_new();
  GtkAboutDialog* a = GTK_ABOUT_DIALOG(d);
  gtk_about_dialog_set_program_name(a, "Virgil");
  gtk_about_dialog_set_version(a, VIRGIL_VERSION);
  gtk_about_dialog_set_comments(
      a, "Virtual Interface Routing Gateway for Inferno-based Low-latency audio.\n\n"
         "A virtual soundcard for Dante® networks, built on Inferno. Independent project: not affiliated "
         "with or endorsed by Audinate. Dante is a registered trademark of Audinate Pty Ltd.");
  gtk_about_dialog_set_license_type(a, GTK_LICENSE_GPL_3_0);
  gtk_about_dialog_set_website(a, "https://github.com/Audbol/Virgil-Virtual-Soundcard");
  if (g_logo) gtk_about_dialog_set_logo(a, g_logo);
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

// ---- start at login ----------------------------------------------------------------------
// The package installs /etc/xdg/autostart/virgil-control.desktop; a user file
// with the same name overrides it (Hidden=true turns it off).

std::string user_autostart() {
  const char* x = std::getenv("XDG_CONFIG_HOME");
  std::string base = x && *x ? x : std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.config";
  return base + "/autostart/virgil-control.desktop";
}
bool autostart_enabled() {
  const std::string u = user_autostart();
  if (exists(u)) {
    std::ifstream f(u);
    std::string line;
    while (std::getline(f, line))
      if (line.rfind("Hidden=", 0) == 0) return line.substr(7) != "true";
    return true;
  }
  return exists("/etc/xdg/autostart/virgil-control.desktop");
}
void set_autostart(bool on) {
  const std::string u = user_autostart();
  run({"mkdir", "-p", u.substr(0, u.find_last_of('/'))});
  std::ofstream f(u, std::ios::trunc);
  f << "[Desktop Entry]\nType=Application\nName=Virgil Control\nIcon=virgil\n"
    << "Exec=\"" << exe_path() << "\" --tray\nX-GNOME-Autostart-enabled=" << (on ? "true" : "false")
    << "\nHidden=" << (on ? "false" : "true") << "\n";
}

// ---- settings dialog ---------------------------------------------------------------------

GtkWidget* label(const char* text) {
  GtkWidget* l = gtk_label_new(text);
  gtk_widget_set_halign(l, GTK_ALIGN_END);
  return l;
}

void show_settings() {
  const auto c = vc::http(g_port, "GET", "/api/config");
  if (c.status != 200) {
    message(GTK_MESSAGE_INFO, "The Virgil service is not running, so its settings cannot be changed now.");
    return;
  }
  const auto ifs = vc::parse_interfaces(vc::http(g_port, "GET", "/api/interfaces").body);
  vc::SettingsForm f = vc::load_settings(c.body, ifs);

  GtkWidget* d = gtk_dialog_new_with_buttons(
      "Virgil Settings", g_window && gtk_widget_get_visible(g_window) ? GTK_WINDOW(g_window) : nullptr,
      GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Cancel", GTK_RESPONSE_CANCEL, "_Apply",
      GTK_RESPONSE_APPLY, nullptr);
  gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY);
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
  int row = 0;
  auto heading = [&](const char* t) {
    GtkWidget* l = gtk_label_new(nullptr);
    char* m = g_markup_printf_escaped("<b>%s</b>", t);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_set_margin_top(l, row ? 8 : 0);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row++, 2, 1);
  };

  heading("Device");
  GtkWidget* name = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(name), 31);
  gtk_entry_set_text(GTK_ENTRY(name), f.name.c_str());
  gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
  gtk_grid_attach(GTK_GRID(grid), label("Name in Dante Controller:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), name, 1, row++, 1, 1);

  GtkWidget* iface = gtk_combo_box_text_new();
  for (const auto& l : f.iface_labels()) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(iface), l.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(iface), f.iface_index);
  gtk_grid_attach(GTK_GRID(grid), label("Network interface:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), iface, 1, row++, 1, 1);

  GtkWidget* rate = gtk_combo_box_text_new();
  for (const char* l : vc::kRateLabels) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rate), l);
  gtk_combo_box_set_active(GTK_COMBO_BOX(rate), f.rate_index);
  gtk_grid_attach(GTK_GRID(grid), label("Sample rate:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), rate, 1, row++, 1, 1);

  GtkWidget* chbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* txch = gtk_spin_button_new_with_range(1, 64, 1);
  GtkWidget* rxch = gtk_spin_button_new_with_range(1, 64, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(txch), f.tx_channels);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rxch), f.rx_channels);
  gtk_box_pack_start(GTK_BOX(chbox), txch, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chbox), gtk_label_new("/"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chbox), rxch, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), label("Channels out / in:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), chbox, 1, row++, 1, 1);

  heading("Latency");
  GtkWidget* rxl = gtk_combo_box_text_new_with_entry();
  for (const char* v : vc::kRxLatencies) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rxl), v);
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(rxl))), f.rx_latency_ms.c_str());
  gtk_grid_attach(GTK_GRID(grid), label("Receive (ms):"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), rxl, 1, row++, 1, 1);
  GtkWidget* txl = gtk_combo_box_text_new_with_entry();
  for (const char* v : vc::kTxLatencies) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(txl), v);
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(txl))), f.tx_latency_ms.c_str());
  gtk_grid_attach(GTK_GRID(grid), label("Transmit (ms, 3.5 minimum):"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), txl, 1, row++, 1, 1);

  heading("Clock");
  GtkWidget* clock = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(clock), "Follow the Dante clock leader (PTP)");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(clock), "Local clock only (testing)");
  gtk_combo_box_set_active(GTK_COMBO_BOX(clock), f.local_clock ? 1 : 0);
  gtk_grid_attach(GTK_GRID(grid), clock, 0, row++, 2, 1);
  GtkWidget* master = gtk_check_button_new_with_label("Become clock leader when no Dante device provides one");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(master), f.master_capable);
  gtk_grid_attach(GTK_GRID(grid), master, 0, row++, 2, 1);

  GtkWidget* err = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(err), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(err), 60);
  gtk_widget_set_halign(err, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), err, 0, row++, 2, 1);

  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
  gtk_widget_show_all(d);

  while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_APPLY) {
    f.name = gtk_entry_get_text(GTK_ENTRY(name));
    f.iface_index = gtk_combo_box_get_active(GTK_COMBO_BOX(iface));
    f.rate_index = gtk_combo_box_get_active(GTK_COMBO_BOX(rate));
    f.tx_channels = unsigned(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(txch)));
    f.rx_channels = unsigned(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(rxch)));
    f.rx_latency_ms = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(rxl))));
    f.tx_latency_ms = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(txl))));
    f.local_clock = gtk_combo_box_get_active(GTK_COMBO_BOX(clock)) == 1;
    f.master_capable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(master));
    gtk_label_set_text(GTK_LABEL(err), "Applying…");
    while (gtk_events_pending()) gtk_main_iteration();
    std::string e;
    if (vc::save_settings(g_port, vc::apply_settings(c.body, f), &e)) break;
    char* m = g_markup_printf_escaped("<span foreground='#D2553F'>%s</span>", e.c_str());
    gtk_label_set_markup(GTK_LABEL(err), m);
    g_free(m);
  }
  gtk_widget_destroy(d);
}

// ---- tray --------------------------------------------------------------------------------

GtkWidget* g_menu;

void menu_item(const char* text, void (*fn)()) {
  GtkWidget* i = gtk_menu_item_new_with_label(text);
  g_signal_connect_swapped(i, "activate", G_CALLBACK(+[](gpointer f) { reinterpret_cast<void (*)()>(f)(); }),
                           reinterpret_cast<gpointer>(fn));
  gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), i);
}
void menu_sep() { gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), gtk_separator_menu_item_new()); }

void build_menu() {
  g_menu = gtk_menu_new();
  menu_item("Open Virgil", show_window);
  menu_item("Settings…", show_settings);
  menu_item("Restart audio engine", restart_engine);
  menu_sep();
  menu_item("Open control panel in browser", open_browser);
  menu_item("Show log", show_log);
  g_autostart_item = gtk_check_menu_item_new_with_label("Start at login");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_autostart_item), autostart_enabled());
  g_signal_connect(g_autostart_item, "toggled", G_CALLBACK(+[](GtkCheckMenuItem* i, gpointer) {
                     set_autostart(gtk_check_menu_item_get_active(i));
                   }),
                   nullptr);
  gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), g_autostart_item);
  menu_item("About Virgil", show_about);
  menu_sep();
  menu_item("Quit Virgil Control", [] {
    g_quit = true;
    g_application_quit(G_APPLICATION(g_app));
  });
  gtk_widget_show_all(g_menu);
}

// AppIndicator, resolved at run time (Ayatana first, then the original).
using AppNewFn = void* (*)(const char*, const char*, int);
using AppSetIntFn = void (*)(void*, int);
using AppSetMenuFn = void (*)(void*, GtkMenu*);
using AppSetStrFn = void (*)(void*, const char*);
using AppSetStr2Fn = void (*)(void*, const char*, const char*);

bool try_appindicator() {
  void* lib = nullptr;
  for (const char* n : {"libayatana-appindicator3.so.1", "libappindicator3.so.1"})
    if ((lib = dlopen(n, RTLD_NOW | RTLD_GLOBAL))) break;
  if (!lib) return false;
  auto app_new = reinterpret_cast<AppNewFn>(dlsym(lib, "app_indicator_new"));
  auto set_status = reinterpret_cast<AppSetIntFn>(dlsym(lib, "app_indicator_set_status"));
  auto set_menu = reinterpret_cast<AppSetMenuFn>(dlsym(lib, "app_indicator_set_menu"));
  auto set_title = reinterpret_cast<AppSetStrFn>(dlsym(lib, "app_indicator_set_title"));
  auto set_theme = reinterpret_cast<AppSetStrFn>(dlsym(lib, "app_indicator_set_icon_theme_path"));
  if (!app_new || !set_status || !set_menu) return false;
  // Category 0 = application status; status 1 = active.
  void* ind = app_new("virgil-control", "virgil", 0);
  if (!ind) return false;
  if (set_theme && portable()) set_theme(ind, exe_dir().c_str());
  if (set_title) set_title(ind, "Virgil");
  set_menu(ind, GTK_MENU(g_menu));
  set_status(ind, 1);
  return true;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
GtkStatusIcon* g_status_icon;
gboolean update_tooltip(gpointer) {
  if (g_status_icon) {
    vc::Status s;
    {
      std::lock_guard<std::mutex> l(g_mutex);
      s = g_latest;
    }
    gtk_status_icon_set_tooltip_text(g_status_icon, vc::tray_tooltip(s).c_str());
  }
  return G_SOURCE_CONTINUE;
}
bool try_status_icon() {
  // Only useful on X11 desktops with a system-tray area.
  GdkDisplay* dpy = gdk_display_get_default();
  if (!dpy || std::strcmp(G_OBJECT_TYPE_NAME(dpy), "GdkX11Display") != 0) return false;
  g_status_icon = g_logo ? gtk_status_icon_new_from_pixbuf(g_logo) : gtk_status_icon_new_from_icon_name("virgil");
  gtk_status_icon_set_title(g_status_icon, "Virgil");
  g_signal_connect(g_status_icon, "activate", G_CALLBACK(+[](GtkStatusIcon*, gpointer) {
                     if (g_visible) {
                       gtk_widget_hide(g_window);
                       g_visible = false;
                     } else {
                       show_window();
                     }
                   }),
                   nullptr);
  g_signal_connect(g_status_icon, "popup-menu", G_CALLBACK(+[](GtkStatusIcon*, guint, guint, gpointer) {
                     gtk_menu_popup_at_pointer(GTK_MENU(g_menu), nullptr);
                   }),
                   nullptr);
  g_timeout_add_seconds(2, update_tooltip, nullptr);
  return true;
}
G_GNUC_END_IGNORE_DEPRECATIONS

// ---- application -------------------------------------------------------------------------

void on_activate(GtkApplication*, gpointer) {
  if (g_window) {  // second launch: bring the window up
    show_window();
    return;
  }
  g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
  g_logo = load_logo();
  build_window();
  build_menu();
  g_have_tray = try_appindicator() || try_status_icon();
  g_application_hold(G_APPLICATION(g_app));  // keep running with the window hidden
  std::thread(poll_thread).detach();
  // The portable folder has no service: start virgild from it.
  if (portable() && vc::http(g_port, "GET", "/api/status").status != 200) start_service();
  if (!(g_start_hidden && g_have_tray)) show_window();
}

int on_command_line(GApplication* app, GApplicationCommandLine* cl, gpointer) {
  int argc = 0;
  gchar** argv = g_application_command_line_get_arguments(cl, &argc);
  bool tray = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--tray")) tray = true;
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) g_port = unsigned(std::atoi(argv[++i]));
  }
  g_strfreev(argv);
  if (!g_window) g_start_hidden = tray;
  if (g_window && tray) return 0;  // already running; --tray from login: nothing to do
  g_application_activate(app);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      std::printf("usage: virgil-control [--tray] [--port N]\n"
                  "Status window, level meters and settings for the Virgil virtual soundcard.\n");
      return 0;
    }
    if (!std::strcmp(argv[i], "--version")) {
      std::printf("virgil-control %s\n", VIRGIL_VERSION);
      return 0;
    }
  }
  g_app = gtk_application_new("org.virgil.Control", G_APPLICATION_HANDLES_COMMAND_LINE);
  g_signal_connect(g_app, "activate", G_CALLBACK(on_activate), nullptr);
  g_signal_connect(g_app, "command-line", G_CALLBACK(on_command_line), nullptr);
  const int rc = g_application_run(G_APPLICATION(g_app), argc, argv);
  g_quit = true;
  g_object_unref(g_app);
  return rc;
}
