// Virgil Control for Linux (desktop PCs and Raspberry Pi): tray icon, status
// window with level meters and a settings dialog, drawn like the Windows app.
// It talks to virgild over its local control API (127.0.0.1:8480).
//
//   virgil-control          open the window (and the tray icon)
//   virgil-control --tray   start in the tray only (used at login)
//   virgil-control --desktop-audio   open the Desktop audio window
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
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "desktop_audio_engine.h"
#include "desktop_audio_gtk.h"
#include "panels_gtk.h"
#include "settings_model.h"
#include "view.h"

#ifndef VIRGIL_VERSION
#define VIRGIL_VERSION "dev"
#endif

namespace {

// ---- state -------------------------------------------------------------------------------
// Everything here is touched on the GTK main thread only, except g_latest
// (g_mutex), the atomics and g_port (set before the poll thread starts).

unsigned g_port = 8480;
GtkApplication* g_app;
GtkWidget* g_window;  // null once destroyed
GtkWidget* g_area;    // null once destroyed
GtkWidget* g_log_button;  // "Show log" over the offline view
vc::StatusView g_view;
std::mutex g_mutex;
vc::Status g_latest;  // guarded by g_mutex
// /api/status "remote": the control panel served to other computers.
struct RemoteInfo {
  bool listening = false;
  std::vector<std::string> urls;
};
RemoteInfo g_remote;  // guarded by g_mutex
std::atomic<bool> g_visible{false};
std::atomic<bool> g_quit{false};
bool g_started = false;
bool g_have_tray = false;
bool g_start_hidden = false;
bool g_open_desktop = false;  // --desktop-audio
GdkPixbuf* g_logo;
GtkWidget* g_autostart_item;
GtkWidget* g_settings_item;
GtkWidget* g_restart_item;
GtkWidget* g_run_item;  // Stop Virgil / Start Virgil
GtkWidget* g_context_menu;
int g_busy = 0;  // service requests in flight (controls are disabled meanwhile)
// Desktop audio (this computer's apps <-> Dante), running for as long as this app.
std::unique_ptr<desk::Engine> g_desktop;

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

// Child processes go through g_spawn (no allocation between fork and exec,
// which matters with the poll thread running).
std::vector<char*> c_argv(const std::vector<std::string>& argv) {
  std::vector<char*> a;
  for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
  a.push_back(nullptr);
  return a;
}

// Run a command without a shell; returns its exit status (or -1). `out`, if
// given, gets its output (stdout, then stderr).
int run(const std::vector<std::string>& argv, std::string* out = nullptr) {
  std::vector<char*> a = c_argv(argv);
  gchar *so = nullptr, *se = nullptr;
  int st = 0;
  GError* err = nullptr;
  const GSpawnFlags flags = GSpawnFlags(G_SPAWN_SEARCH_PATH | (out ? 0 : G_SPAWN_STDOUT_TO_DEV_NULL));
  if (!g_spawn_sync(nullptr, a.data(), nullptr, flags, nullptr, nullptr, out ? &so : nullptr, out ? &se : nullptr,
                    &st, &err)) {
    if (out) *out = err->message;
    g_error_free(err);
    return -1;
  }
  if (out) *out = std::string(so ? so : "") + (se ? se : "");
  g_free(so);
  g_free(se);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
// Start a program in its own session and forget about it (GLib reaps it).
void spawn_detached(const std::vector<std::string>& argv) {
  std::vector<char*> a = c_argv(argv);
  GError* err = nullptr;
  if (!g_spawn_async(nullptr, a.data(), nullptr, G_SPAWN_SEARCH_PATH, +[](gpointer) { setsid(); }, nullptr, nullptr,
                     &err)) {
    g_warning("cannot start %s: %s", a[0], err->message);
    g_error_free(err);
  }
}

// Run `work` on a worker thread and `done(result)` back on the GTK main thread.
template <class R>
void run_async(std::function<R()> work, std::function<void(const R&)> done) {
  std::thread([work = std::move(work), done = std::move(done)]() {
    auto* cb = new std::function<void()>([r = work(), done]() { done(r); });
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

void update_controls();
// A user action that talks to the service: false (do nothing) while another is pending.
bool begin_action() {
  if (g_busy) return false;
  ++g_busy;
  update_controls();
  return true;
}
void end_action() {
  if (g_busy) --g_busy;
  update_controls();
}

void quit_app() {
  g_quit = true;
  g_application_quit(G_APPLICATION(g_app));
}

GtkWindow* parent_window() {
  return g_window && gtk_widget_get_visible(g_window) ? GTK_WINDOW(g_window) : nullptr;
}

void message(GtkMessageType type, const std::string& text) {
  GtkWidget* d =
      gtk_message_dialog_new(parent_window(), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", text.c_str());
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
  CairoCanvas(cairo_t* cr, int scale) : cr_(cr), scale_(scale < 1 ? 1 : scale) {}

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
    // Scaled once per size and device scale (sharp on HiDPI), not every frame.
    static cairo_surface_t* cached;
    static int cw, ch, cs;
    if (!g_logo) return;
    const int w = int(r.r - r.l), h = int(r.b - r.t);
    if (w <= 0 || h <= 0) return;
    if (!cached || cw != w || ch != h || cs != scale_) {
      GdkPixbuf* s = gdk_pixbuf_scale_simple(g_logo, w * scale_, h * scale_, GDK_INTERP_HYPER);
      if (!s) return;
      if (cached) cairo_surface_destroy(cached);
      cached = gdk_cairo_surface_create_from_pixbuf(s, scale_, nullptr);
      g_object_unref(s);
      cw = w, ch = h, cs = scale_;
    }
    cairo_set_source_surface(cr_, cached, r.l, r.t);
    cairo_paint(cr_);
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
  int scale_;
};

// ---- window ------------------------------------------------------------------------------

void resize_to_view() {
  if (!g_window || !g_area) return;
  const int w = int(g_view.width()), h = int(g_view.height());
  gtk_widget_set_size_request(g_area, w, h);
  gtk_window_resize(GTK_WINDOW(g_window), w, h);
}

gboolean on_draw(GtkWidget* w, cairo_t* cr, gpointer) {
  CairoCanvas c(cr, gtk_widget_get_scale_factor(w));
  g_view.paint(c, float(gtk_widget_get_allocated_width(w)), float(gtk_widget_get_allocated_height(w)));
  return TRUE;
}

void set_cursor() {
  if (!g_area) return;
  GdkWindow* win = gtk_widget_get_window(g_area);
  if (!win) return;
  const char* name = g_busy ? "progress" : g_view.over_button() ? "pointer" : nullptr;
  GdkCursor* cur = name ? gdk_cursor_new_from_name(gdk_window_get_display(win), name) : nullptr;
  gdk_window_set_cursor(win, cur);
  if (cur) g_object_unref(cur);
}

void show_settings();
void restart_engine();
void start_service();
void open_diagnostics();
void open_channel_names();
void open_config_editor();
void stop_virgil();
void start_virgil();
void show_log();
void show_desktop_audio();

gboolean on_motion(GtkWidget*, GdkEventMotion* e, gpointer) {
  if (g_view.hover(float(e->x), float(e->y))) {
    set_cursor();
    gtk_widget_queue_draw(g_area);
  }
  return TRUE;
}
gboolean on_leave(GtkWidget*, GdkEventCrossing*, gpointer) {
  if (g_view.hover(-1e6f, -1e6f)) {
    set_cursor();
    gtk_widget_queue_draw(g_area);
  }
  return FALSE;
}
gboolean on_press(GtkWidget*, GdkEventButton* e, gpointer) {
  if (e->type != GDK_BUTTON_PRESS) return TRUE;
  if (e->button == 3 && g_context_menu) {
    gtk_menu_popup_at_pointer(GTK_MENU(g_context_menu), reinterpret_cast<GdkEvent*>(e));
    return TRUE;
  }
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
  if (a == vc::Action::Diagnostics) open_diagnostics();
  if (a == vc::Action::Stop) stop_virgil();
  if (a == vc::Action::Start) start_virgil();
  if (a == vc::Action::DesktopAudio) show_desktop_audio();
  return TRUE;
}

gboolean on_delete(GtkWidget* w, GdkEvent*, gpointer) {
  if (!g_have_tray) {  // no tray to bring it back from: closing quits
    quit_app();
    return TRUE;
  }
  gtk_widget_hide(w);
  g_visible = false;
  return TRUE;
}

void on_destroy(GtkWidget*, gpointer) {
  g_window = g_area = g_log_button = nullptr;
  g_visible = false;
}

void build_window();
void show_window() {
  if (!g_window) build_window();
  resize_to_view();
  gtk_widget_show_all(g_window);
  gtk_window_present(GTK_WINDOW(g_window));
  g_visible = true;
  update_controls();
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
  g_signal_connect(g_area, "leave-notify-event", G_CALLBACK(on_leave), nullptr);
  g_signal_connect(g_area, "button-press-event", G_CALLBACK(on_press), nullptr);
  g_signal_connect(g_area, "button-release-event", G_CALLBACK(on_release), nullptr);
  g_signal_connect(g_window, "delete-event", G_CALLBACK(on_delete), nullptr);
  g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), nullptr);
  // The offline view says "check the log": a button for it, below "Start service".
  GtkWidget* overlay = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(overlay), g_area);
  // Drawn in the view's own colours: the window is dark whatever the GTK theme.
  g_log_button = gtk_button_new();
  GtkWidget* log_label = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(log_label), "<span foreground='#8C9097' underline='single'>Show log</span>");
  gtk_container_add(GTK_CONTAINER(g_log_button), log_label);
  gtk_widget_show(log_label);
  gtk_button_set_relief(GTK_BUTTON(g_log_button), GTK_RELIEF_NONE);
  gtk_widget_set_halign(g_log_button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(g_log_button, GTK_ALIGN_START);
  gtk_widget_set_margin_top(g_log_button, 76 + 182);  // view: header + below the Start service button
  gtk_widget_set_no_show_all(g_log_button, TRUE);
  g_signal_connect(g_log_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { show_log(); }), nullptr);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), g_log_button);
  gtk_container_add(GTK_CONTAINER(g_window), overlay);
  resize_to_view();
}

// Menu items, the cursor and the log button follow the status and pending requests.
void update_controls() {
  const vc::Status& s = g_view.status();
  const bool idle = g_busy == 0;
  if (g_run_item) {
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_run_item), s.state == "stopped" ? "Start Virgil" : "Stop Virgil");
    gtk_widget_set_sensitive(g_run_item, s.reachable && idle);
  }
  if (g_restart_item) gtk_widget_set_sensitive(g_restart_item, s.reachable && s.state != "stopped" && idle);
  if (g_settings_item) gtk_widget_set_sensitive(g_settings_item, idle);
  if (g_log_button) gtk_widget_set_visible(g_log_button, !s.reachable);
  set_cursor();
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
  if (g_visible && g_area) gtk_widget_queue_draw(g_area);
  update_controls();
  if (event == 1) notify("Virgil service stopped", "Audio to and from the Dante network has stopped.");
  if (event == 2) notify("Dante clock lost", "Virgil is running on its own clock; check the network connection.");
  if (event == 3) {
    const vc::ClockNotice n = vc::clock_notice(s);
    if (n.show) {
      std::string body = n.title + ".";
      for (const std::string& h : n.hints) body += "\n" + h;
      notify(n.headline, body);
    }
  }
  return G_SOURCE_REMOVE;
}

void poll_thread() {
  // Starts false: no "stopped" notification when the service is not up yet at login.
  bool was_reachable = false;
  int failures = 0;  // consecutive failed polls
  std::string was_clock;
  uint64_t lost_since = 0;
  bool lost_told = false;
  bool no_leader_told = false;  // once per run of the engine
  while (!g_quit) {
    const auto r = vc::http(g_port, "GET", "/api/status");
    vc::Status s;
    const bool ok = r.status == 200 && vc::parse_status(r.body, &s);
    failures = ok ? 0 : failures + 1;
    // One missed poll (a busy daemon, a restart of the engine) is not "not running":
    // keep showing the last status until three in a row fail.
    if (!ok && was_reachable && failures < 3) {
      std::this_thread::sleep_for(std::chrono::milliseconds(g_visible ? 80 : 1000));
      continue;
    }
    if (!ok) s = vc::Status();
    RemoteInfo remote;
    vc::Json j;
    if (ok && vc::parse_json(r.body, &j)) {
      const vc::Json& rj = j["remote"];
      remote.listening = rj["listening"].boolean();
      for (size_t i = 0; i < rj["urls"].size(); ++i)
        if (!rj["urls"][i].str().empty()) remote.urls.push_back(rj["urls"][i].str());
    }
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_latest = s;
      g_remote = remote;
    }
    int event = 0;
    if (was_reachable && !s.reachable) event = 1;
    // Not when Virgil was stopped on purpose (or is restarting).
    if (s.reachable && s.state == "running" && was_clock == "ptp-locked" && s.clock != "ptp-locked" && !lost_since)
      lost_since = now_ms();
    if (s.state != "running") lost_since = 0;
    if (s.clock == "ptp-locked") lost_since = 0, lost_told = false;
    if (lost_since && !lost_told && now_ms() - lost_since > 5000) {
      lost_told = true;
      event = 2;
    }
    // No Dante clock leader heard at all (a lost lock is the case above).
    if ((s.reachable && s.state != "running") || s.clock == "ptp-locked") no_leader_told = false;
    if (!event && !no_leader_told && !lost_since && vc::clock_notice(s).show) {
      no_leader_told = true;
      event = 3;
    }
    was_reachable = s.reachable;
    was_clock = s.clock;
    g_idle_add(apply_status, GINT_TO_POINTER(event));
    std::this_thread::sleep_for(std::chrono::milliseconds(g_visible ? 80 : 1000));
  }
}

// ---- actions -----------------------------------------------------------------------------
// Requests to the service run on a worker thread so the window keeps drawing.

void post_async(const char* path, const char* failure) {
  if (!begin_action()) return;
  const unsigned port = g_port;
  const std::string p = path, msg = failure;
  run_async<int>([port, p] { return vc::http(port, "POST", p).status; },
                 [msg](const int& status) {
                   end_action();
                   if (status != 200) message(GTK_MESSAGE_WARNING, msg);
                 });
}

void stop_virgil() {
  if (g_busy) return;
  GtkWidget* d = gtk_message_dialog_new(parent_window(), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                        "Stop Virgil?");
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(d),
      "Audio to and from the Dante network stops, Virgil leaves the network and apps cannot use its "
      "soundcard until you start it again. It stays stopped after a restart of the computer.");
  gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, "_Stop", GTK_RESPONSE_ACCEPT, nullptr);
  gtk_window_set_title(GTK_WINDOW(d), "Virgil");
  const int r = gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
  if (r != GTK_RESPONSE_ACCEPT) return;
  post_async("/api/stop", "The Virgil service is not running.");
}

void start_virgil() { post_async("/api/start", "The Virgil service is not running."); }

void toggle_running() {
  if (g_view.status().state == "stopped") start_virgil();
  else stop_virgil();
}

void restart_engine() {
  post_async("/api/restart", "The Virgil service is not running, so the audio engine cannot be restarted.");
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
  // Installed: the systemd service. pkexec asks for the password graphically
  // (and can take a while), so it runs off the main thread.
  if (!begin_action()) return;
  run_async<int>([] { return run({"pkexec", "systemctl", "start", "virgild"}); },
                 [](const int& rc) {
                   end_action();
                   if (rc != 0)
                     message(GTK_MESSAGE_WARNING,
                             "Could not start the Virgil service.\n\nIn a terminal: sudo systemctl start virgild\n"
                             "and see why with: journalctl -u virgild -n 50");
                 });
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
    run({"journalctl", "-u", "virgild", "-n", "400", "--no-pager"}, &text);
  }
  if (text.empty()) text = "No log available. In a terminal: journalctl -u virgild -n 100";
  GtkWidget* d = gtk_dialog_new_with_buttons("Virgil log", parent_window(), GTK_DIALOG_DESTROY_WITH_PARENT,
                                             "_Close", GTK_RESPONSE_CLOSE, nullptr);
  gtk_window_set_default_size(GTK_WINDOW(d), 860, 520);
  GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
  GtkWidget* tv = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
  GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  if (!g_utf8_validate(text.c_str(), gssize(text.size()), nullptr)) {
    gchar* v = g_utf8_make_valid(text.c_str(), gssize(text.size()));
    text = v;
    g_free(v);
  }
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

// Dropouts and diagnostics, the channel name editor and the configuration
// file editor: native windows (panels_gtk.cpp).
void open_diagnostics() { vcgtk::show_diagnostics(parent_window(), g_logo, g_port); }
void open_channel_names() {
  std::vector<std::string> tx, rx;  // the names on the network now, for the placeholders
  {
    std::lock_guard<std::mutex> l(g_mutex);
    tx = g_latest.tx_names;
    rx = g_latest.rx_names;
  }
  vcgtk::show_channel_names(parent_window(), g_logo, g_port, tx, rx);
}
void open_config_editor() { vcgtk::show_config_editor(parent_window(), g_logo, g_port); }

// Virgil's channels as the service reports them, for the Desktop audio window.
dagtk::ChannelInfo desktop_channels() {
  std::lock_guard<std::mutex> l(g_mutex);
  dagtk::ChannelInfo c;
  if (!g_latest.reachable) return c;
  c.tx = g_latest.tx;
  c.rx = g_latest.rx;
  c.tx_names = g_latest.tx_names;
  c.rx_names = g_latest.rx_names;
  return c;
}

void show_desktop_audio() {
  if (g_desktop) dagtk::show(parent_window(), g_logo, *g_desktop, desktop_channels);
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
  g_mkdir_with_parents(u.substr(0, u.find_last_of('/')).c_str(), 0755);
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

constexpr int kResponseNames = 1;
constexpr int kResponseDiag = 2;
constexpr int kResponseConfig = 3;

// The open settings dialog (one at a time). Shared with an Apply in flight,
// which may finish after the dialog was closed.
struct SettingsUi {
  GtkWidget *d = nullptr, *name, *iface, *rate, *txch, *rxch, *rxl, *txl, *clock, *master, *remote, *password, *err;
  std::vector<GtkWidget*> more;  // Dropouts and diagnostics…, Channel names…, Edit configuration…
  vc::SettingsForm f;
  std::string conf;
  bool applying = false;
};
std::shared_ptr<SettingsUi> g_settings;

void settings_buttons(SettingsUi& ui, bool on) {
  for (int r : {int(GTK_RESPONSE_CANCEL), int(GTK_RESPONSE_APPLY)})
    gtk_dialog_set_response_sensitive(GTK_DIALOG(ui.d), r, on);
  for (GtkWidget* b : ui.more) gtk_widget_set_sensitive(b, on);
}

void on_settings_response(GtkDialog*, int response, gpointer) {
  const std::shared_ptr<SettingsUi> ui = g_settings;
  if (!ui || !ui->d || ui->applying) return;  // closing waits for Apply to finish
  if (response != GTK_RESPONSE_APPLY) {
    gtk_widget_destroy(ui->d);
    if (response == kResponseNames) open_channel_names();
    if (response == kResponseDiag) open_diagnostics();
    if (response == kResponseConfig) open_config_editor();
    return;
  }
  vc::SettingsForm& f = ui->f;
  // Take a number typed but not yet committed (Enter pressed in the field).
  gtk_spin_button_update(GTK_SPIN_BUTTON(ui->txch));
  gtk_spin_button_update(GTK_SPIN_BUTTON(ui->rxch));
  f.name = gtk_entry_get_text(GTK_ENTRY(ui->name));
  f.iface_index = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->iface));
  f.rate_index = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->rate));
  f.tx_channels = unsigned(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->txch)));
  f.rx_channels = unsigned(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->rxch)));
  f.rx_latency_ms = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(ui->rxl))));
  f.tx_latency_ms = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(ui->txl))));
  f.local_clock = gtk_combo_box_get_active(GTK_COMBO_BOX(ui->clock)) == 1;
  f.master_capable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->master));
  f.remote_access = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->remote));
  f.remote_password = gtk_entry_get_text(GTK_ENTRY(ui->password));
  const std::string problem = vc::remote_access_problem(f);
  if (!problem.empty()) {
    char* m = g_markup_printf_escaped("<span foreground='#D2553F'>%s</span>", problem.c_str());
    gtk_label_set_markup(GTK_LABEL(ui->err), m);
    g_free(m);
    gtk_widget_grab_focus(ui->password);
    return;
  }
  gtk_label_set_text(GTK_LABEL(ui->err), "Applying…");
  ui->applying = true;
  settings_buttons(*ui, false);
  ++g_busy;
  update_controls();
  const unsigned port = g_port;
  const std::string conf = vc::apply_settings(ui->conf, f);
  using Result = std::pair<bool, std::string>;
  run_async<Result>(
      [port, conf] {
        std::string e;
        const bool ok = vc::save_settings(port, conf, &e);
        return Result(ok, e);
      },
      [ui](const Result& r) {
        end_action();
        ui->applying = false;
        if (!ui->d) return;
        if (r.first) {
          gtk_widget_destroy(ui->d);
          return;
        }
        settings_buttons(*ui, true);
        char* m = g_markup_printf_escaped("<span foreground='#D2553F'>%s</span>", r.second.c_str());
        gtk_label_set_markup(GTK_LABEL(ui->err), m);
        g_free(m);
      });
}

void build_settings(const std::string& conf, const std::vector<vc::Interface>& ifs) {
  auto ui = std::make_shared<SettingsUi>();
  ui->conf = conf;
  ui->f = vc::load_settings(conf, ifs);
  const vc::SettingsForm& f = ui->f;

  GtkWidget* d = ui->d = gtk_dialog_new_with_buttons(
      "Virgil Settings", parent_window(), GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Cancel", GTK_RESPONSE_CANCEL, "_Apply", GTK_RESPONSE_APPLY, nullptr);
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
  GtkWidget* name = ui->name = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(name), 31);
  gtk_entry_set_text(GTK_ENTRY(name), f.name.c_str());
  gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
  gtk_grid_attach(GTK_GRID(grid), label("Name in Dante Controller:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), name, 1, row++, 1, 1);

  GtkWidget* iface = ui->iface = gtk_combo_box_text_new();
  for (const auto& l : f.iface_labels()) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(iface), l.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(iface), f.iface_index);
  gtk_grid_attach(GTK_GRID(grid), label("Network interface:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), iface, 1, row++, 1, 1);

  GtkWidget* rate = ui->rate = gtk_combo_box_text_new();
  for (const char* l : vc::kRateLabels) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rate), l);
  gtk_combo_box_set_active(GTK_COMBO_BOX(rate), f.rate_index);
  gtk_grid_attach(GTK_GRID(grid), label("Sample rate:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), rate, 1, row++, 1, 1);

  // 0 is allowed in one direction (the service insists on at least one channel).
  GtkWidget* chbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* txch = ui->txch = gtk_spin_button_new_with_range(0, 128, 1);
  GtkWidget* rxch = ui->rxch = gtk_spin_button_new_with_range(0, 128, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(txch), f.tx_channels);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rxch), f.rx_channels);
  gtk_box_pack_start(GTK_BOX(chbox), txch, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chbox), gtk_label_new("/"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chbox), rxch, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), label("Channels out / in:"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), chbox, 1, row++, 1, 1);

  heading("Latency");
  GtkWidget* rxl = ui->rxl = gtk_combo_box_text_new_with_entry();
  for (const char* v : vc::kRxLatencies) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rxl), v);
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(rxl))), f.rx_latency_ms.c_str());
  gtk_grid_attach(GTK_GRID(grid), label("Receive (ms):"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), rxl, 1, row++, 1, 1);
  GtkWidget* txl = ui->txl = gtk_combo_box_text_new_with_entry();
  for (const char* v : vc::kTxLatencies) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(txl), v);
  gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(txl))), f.tx_latency_ms.c_str());
  gtk_grid_attach(GTK_GRID(grid), label("Transmit (ms, 3.5 minimum):"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), txl, 1, row++, 1, 1);

  heading("Clock");
  GtkWidget* clock = ui->clock = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(clock), "Follow the Dante clock leader (PTP)");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(clock), "Local clock only (testing)");
  gtk_combo_box_set_active(GTK_COMBO_BOX(clock), f.local_clock ? 1 : 0);
  gtk_grid_attach(GTK_GRID(grid), clock, 0, row++, 2, 1);
  GtkWidget* master = ui->master =
      gtk_check_button_new_with_label("Become clock leader when no Dante device provides one");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(master), f.master_capable);
  gtk_grid_attach(GTK_GRID(grid), master, 0, row++, 2, 1);

  // The web control panel from other computers (only this computer may change it).
  heading("Remote access");
  GtkWidget* remote = ui->remote =
      gtk_check_button_new_with_label("Allow remote access from other computers (web browser)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(remote), f.remote_access);
  gtk_grid_attach(GTK_GRID(grid), remote, 0, row++, 2, 1);
  GtkWidget* password = ui->password = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
  gtk_entry_set_input_purpose(GTK_ENTRY(password), GTK_INPUT_PURPOSE_PASSWORD);
  gtk_entry_set_placeholder_text(GTK_ENTRY(password), "at least 8 characters");
  gtk_entry_set_text(GTK_ENTRY(password), f.remote_password.c_str());
  gtk_entry_set_activates_default(GTK_ENTRY(password), TRUE);
  gtk_widget_set_sensitive(password, f.remote_access);
  GtkWidget* password_label = label("Password:");
  gtk_widget_set_sensitive(password_label, f.remote_access);
  g_object_set_data(G_OBJECT(remote), "vg-password-label", password_label);
  g_signal_connect(remote, "toggled", G_CALLBACK(+[](GtkToggleButton* b, gpointer entry) {
                     const bool on = gtk_toggle_button_get_active(b);
                     gtk_widget_set_sensitive(GTK_WIDGET(entry), on);
                     gtk_widget_set_sensitive(GTK_WIDGET(g_object_get_data(G_OBJECT(b), "vg-password-label")), on);
                   }),
                   password);
  gtk_grid_attach(GTK_GRID(grid), password_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), password, 1, row++, 1, 1);
  RemoteInfo ri;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    ri = g_remote;
  }
  if (ri.listening) {
    std::string text = "Open from another computer:";
    for (const auto& u : ri.urls) text += "\n" + u;
    GtkWidget* urls = gtk_label_new(text.c_str());
    gtk_label_set_selectable(GTK_LABEL(urls), TRUE);
    gtk_widget_set_can_focus(urls, FALSE);
    gtk_label_set_xalign(GTK_LABEL(urls), 0);
    gtk_grid_attach(GTK_GRID(grid), urls, 0, row++, 2, 1);
    GtkWidget* plain = gtk_label_new("Plain HTTP: the password crosses the network unencrypted.");
    gtk_label_set_line_wrap(GTK_LABEL(plain), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(plain), 60);
    gtk_label_set_xalign(GTK_LABEL(plain), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(plain), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), plain, 0, row++, 2, 1);
  }

  // The other windows (they close this dialog, which is modal, and open on their own).
  GtkWidget* more = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(more, 8);
  const std::pair<const char*, int> more_buttons[] = {{"_Dropouts and diagnostics…", kResponseDiag},
                                                      {"Channel _names…", kResponseNames},
                                                      {"_Edit configuration…", kResponseConfig}};
  for (const auto& mb : more_buttons) {
    GtkWidget* b = gtk_button_new_with_mnemonic(mb.first);
    g_object_set_data(G_OBJECT(b), "vg-response", GINT_TO_POINTER(mb.second));
    g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer dialog) {
                       gtk_dialog_response(GTK_DIALOG(dialog),
                                           GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "vg-response")));
                     }),
                     d);
    gtk_box_pack_start(GTK_BOX(more), b, FALSE, FALSE, 0);
    ui->more.push_back(b);
  }
  gtk_grid_attach(GTK_GRID(grid), more, 0, row++, 2, 1);

  GtkWidget* err = ui->err = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(err), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(err), 60);
  gtk_widget_set_halign(err, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), err, 0, row++, 2, 1);

  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
  g_signal_connect(d, "response", G_CALLBACK(on_settings_response), nullptr);
  g_signal_connect(d, "destroy", G_CALLBACK(+[](GtkWidget* w, gpointer) {
                     if (g_settings && g_settings->d == w) {
                       g_settings->d = nullptr;
                       g_settings.reset();
                     }
                   }),
                   nullptr);
  g_settings = ui;
  gtk_widget_show_all(d);
}

void show_settings() {
  if (g_settings && g_settings->d) {  // already open: bring it up
    gtk_window_present(GTK_WINDOW(g_settings->d));
    return;
  }
  if (!begin_action()) return;
  struct Loaded {
    int status = 0;
    std::string conf;
    std::vector<vc::Interface> ifs;
  };
  const unsigned port = g_port;
  run_async<Loaded>(
      [port] {
        Loaded l;
        const auto c = vc::http(port, "GET", "/api/config");
        l.status = c.status;
        l.conf = c.body;
        if (c.status == 200) l.ifs = vc::parse_interfaces(vc::http(port, "GET", "/api/interfaces").body);
        return l;
      },
      [](const Loaded& l) {
        end_action();
        if (l.status != 200) {
          message(GTK_MESSAGE_INFO, "The Virgil service is not running, so its settings cannot be changed now.");
          return;
        }
        if (g_settings && g_settings->d) gtk_window_present(GTK_WINDOW(g_settings->d));
        else build_settings(l.conf, l.ifs);
      });
}

// ---- tray --------------------------------------------------------------------------------

GtkWidget* g_menu;

GtkWidget* menu_item(GtkWidget* menu, const char* text, void (*fn)()) {
  GtkWidget* i = gtk_menu_item_new_with_label(text);
  g_signal_connect_swapped(i, "activate", G_CALLBACK(+[](gpointer f) { reinterpret_cast<void (*)()>(f)(); }),
                           reinterpret_cast<gpointer>(fn));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
  return i;
}
GtkWidget* menu_item(const char* text, void (*fn)()) { return menu_item(g_menu, text, fn); }
void menu_sep() { gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), gtk_separator_menu_item_new()); }

void build_menu() {
  g_menu = gtk_menu_new();
  menu_item("Open Virgil", show_window);
  g_settings_item = menu_item("Settings…", show_settings);
  menu_item("Desktop audio…", show_desktop_audio);
  g_restart_item = menu_item("Restart audio engine", restart_engine);
  g_run_item = gtk_menu_item_new_with_label("Stop Virgil");
  g_signal_connect(g_run_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer) { toggle_running(); }), nullptr);
  gtk_menu_shell_append(GTK_MENU_SHELL(g_menu), g_run_item);
  menu_sep();
  menu_item("Dropouts and diagnostics…", open_diagnostics);
  menu_item("Channel names…", open_channel_names);
  menu_item("Edit configuration…", open_config_editor);
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
  menu_item("Quit Virgil Control", quit_app);
  gtk_widget_show_all(g_menu);

  // Right click in the window (the only menu when there is no tray icon).
  g_context_menu = gtk_menu_new();
  menu_item(g_context_menu, "Show log", show_log);
  menu_item(g_context_menu, "Dropouts and diagnostics…", open_diagnostics);
  menu_item(g_context_menu, "About Virgil", show_about);
  gtk_menu_shell_append(GTK_MENU_SHELL(g_context_menu), gtk_separator_menu_item_new());
  menu_item(g_context_menu, "Quit Virgil Control", quit_app);
  gtk_widget_show_all(g_context_menu);
  update_controls();
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
                     if (g_visible && g_window) {
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
  if (g_started) {  // second launch: bring the window up
    show_window();
    return;
  }
  g_started = true;
  g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
  g_logo = load_logo();
  // Desktop audio runs in this app (it starts at login, in the user's
  // session, where the sound server is).
  g_desktop.reset(new desk::Engine(desk::make_system_backend()));
  g_desktop->start(desk::load_settings(desk::settings_path(), nullptr));
  g_view.set_desktop_audio(true);
  build_window();
  build_menu();
  g_have_tray = try_appindicator() || try_status_icon();
  g_application_hold(G_APPLICATION(g_app));  // keep running with the window hidden
  // Not joined: main() leaves with _exit while it may still be in a request.
  std::thread(poll_thread).detach();
  // The portable folder has no service: start virgild from it.
  if (portable()) {
    const unsigned port = g_port;
    run_async<int>([port] { return vc::http(port, "GET", "/api/status").status; },
                   [](const int& status) {
                     if (status != 200) start_service();
                   });
  }
  if (!(g_start_hidden && g_have_tray)) show_window();
  if (g_open_desktop) show_desktop_audio();
}

int on_command_line(GApplication* app, GApplicationCommandLine* cl, gpointer) {
  int argc = 0;
  gchar** argv = g_application_command_line_get_arguments(cl, &argc);
  bool tray = false, desktop = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--tray")) tray = true;
    if (!std::strcmp(argv[i], "--desktop-audio")) desktop = true;
    // The poll thread reads the port: only the first launch sets it.
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
      const unsigned p = unsigned(std::atoi(argv[++i]));
      if (!g_started && p > 0 && p < 65536) g_port = p;
    }
  }
  g_strfreev(argv);
  if (!g_started) g_start_hidden = tray;
  if (desktop) {
    if (g_started) show_desktop_audio();  // already running: just the window
    else g_open_desktop = true;
    if (g_started) return 0;
  }
  if (g_started && tray) return 0;  // already running; --tray from login: nothing to do
  g_application_activate(app);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      std::printf("usage: virgil-control [--tray] [--desktop-audio] [--port N]\n"
                  "Status window, level meters and settings for the Virgil virtual soundcard.\n"
                  "  --tray            start in the tray only (used at login)\n"
                  "  --desktop-audio   open the Desktop audio window\n");
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
  if (g_desktop) g_desktop->shutdown();  // releases its Virgil playback slot
  // The poll thread (and any request worker) may still be running: leave
  // without static destructors, as the macOS app does.
  g_quit = true;
  std::fflush(nullptr);
  _exit(rc);
}
