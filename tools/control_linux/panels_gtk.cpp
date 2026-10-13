#include "panels_gtk.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "api.h"
#include "config_model.h"
#include "diagnostics_model.h"

namespace vcgtk {

namespace {

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

const char* const kRed = "#D2553F";

void install_css() {
  static bool done = false;
  if (done) return;
  done = true;
  GtkCssProvider* p = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      p,
      "label.vg-pill { border-radius: 999px; padding: 3px 12px; font-weight: bold; color: #15171A; }"
      "label.vg-pill.vg-ok { background-color: #4EA85A; }"
      "label.vg-pill.vg-info { background-color: #8C9097; }"
      "label.vg-pill.vg-warn { background-color: #D9A93F; }"
      "label.vg-pill.vg-error { background-color: #D2553F; }"
      "label.vg-field-error { color: #D2553F; font-size: smaller; }"
      "box.vg-events { padding: 4px 8px; }",
      -1, nullptr);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(p);
}

GtkWidget* hlabel(const char* text, float xalign = 0) {
  GtkWidget* l = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(l), xalign);
  return l;
}

GtkWidget* dim(GtkWidget* w) {
  gtk_style_context_add_class(gtk_widget_get_style_context(w), "dim-label");
  return w;
}

GtkWidget* wrapped(GtkWidget* l, int max_chars = 80) {
  gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(l), max_chars);
  return l;
}

void set_markup(GtkWidget* label, const std::string& text, const char* color) {
  char* m = color ? g_markup_printf_escaped("<span foreground='%s'>%s</span>", color, text.c_str())
                  : g_markup_escape_text(text.c_str(), -1);
  gtk_label_set_markup(GTK_LABEL(label), m);
  g_free(m);
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

GtkWidget* heading(const char* t) {
  GtkWidget* l = gtk_label_new(nullptr);
  char* m = g_markup_printf_escaped("<b>%s</b>", t);
  gtk_label_set_markup(GTK_LABEL(l), m);
  g_free(m);
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  return l;
}

GtkWidget* new_window(GtkWindow* parent, GdkPixbuf* icon, const char* title, int w, int h) {
  GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(win), title);
  if (parent) gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  if (icon) gtk_window_set_icon(GTK_WINDOW(win), icon);
  else gtk_window_set_icon_name(GTK_WINDOW(win), "virgil");
  gtk_window_set_default_size(GTK_WINDOW(win), w, h);
  return win;
}

void message(GtkWindow* parent, GtkMessageType type, const std::string& text) {
  GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", text.c_str());
  gtk_window_set_title(GTK_WINDOW(d), "Virgil");
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}

void clear_children(GtkWidget* box) {
  GList* kids = gtk_container_get_children(GTK_CONTAINER(box));
  for (GList* k = kids; k; k = k->next) gtk_widget_destroy(GTK_WIDGET(k->data));
  g_list_free(kids);
}

void set_rgb(cairo_t* cr, unsigned c, double a = 1) {
  cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
}

std::string hex(unsigned c) {
  char b[16];
  std::snprintf(b, sizeof b, "#%06X", c & 0xFFFFFFu);
  return b;
}

const char* severity_class(vc::Severity s) {
  switch (s) {
    case vc::Severity::Ok: return "vg-ok";
    case vc::Severity::Warn: return "vg-warn";
    case vc::Severity::Error: return "vg-error";
    default: return "vg-info";
  }
}
const char* severity_color(vc::Severity s) {
  switch (s) {
    case vc::Severity::Ok: return "#4EA85A";
    case vc::Severity::Warn: return "#D9A93F";
    case vc::Severity::Error: return "#D2553F";
    default: return "#8C9097";
  }
}

// ---- Dropouts and diagnostics -------------------------------------------------------------

struct Diag {
  GtkWidget *w = nullptr, *pill, *since, *chart, *hints, *events, *err, *save, *clear;
  unsigned port = 0;
  vc::Diagnostics d;
  bool have = false, loading = false, events_built = false;
  guint timer = 0;
  std::string hints_sig, events_sig, pill_class;
};
std::shared_ptr<Diag> g_diag;

void diag_apply(Diag& s) {
  const vc::Diagnostics& d = s.d;
  GtkStyleContext* pc = gtk_widget_get_style_context(s.pill);
  if (!s.pill_class.empty()) gtk_style_context_remove_class(pc, s.pill_class.c_str());
  s.pill_class = severity_class(d.headline_severity);
  gtk_style_context_add_class(pc, s.pill_class.c_str());
  gtk_label_set_text(GTK_LABEL(s.pill), d.headline.c_str());
  gtk_widget_show(s.pill);
  gtk_label_set_text(GTK_LABEL(s.since), d.since_text.c_str());
  gtk_widget_queue_draw(s.chart);
  gtk_widget_trigger_tooltip_query(s.chart);

  // Hints and events are rebuilt only when they change (keeps the scroll
  // position and any selection while the window refreshes).
  std::string hs;
  for (const auto& h : d.hints) hs += h + '\n';
  if (hs != s.hints_sig) {
    s.hints_sig = hs;
    clear_children(s.hints);
    for (const auto& h : d.hints) {
      GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget* bullet = hlabel("•");
      gtk_widget_set_valign(bullet, GTK_ALIGN_START);
      gtk_box_pack_start(GTK_BOX(row), bullet, FALSE, FALSE, 0);
      GtkWidget* t = wrapped(hlabel(h.c_str()), 90);
      gtk_widget_set_hexpand(t, TRUE);
      gtk_box_pack_start(GTK_BOX(row), t, TRUE, TRUE, 0);
      gtk_widget_show_all(row);
      gtk_box_pack_start(GTK_BOX(s.hints), row, FALSE, FALSE, 0);
    }
    gtk_widget_set_visible(s.hints, !d.hints.empty());
  }

  const size_t n = std::min(d.events.size(), vc::kDiagShownEvents);
  std::string es;
  for (size_t i = 0; i < n; ++i) {
    const auto& e = d.events[i];
    es += e.time_text + '|' + std::to_string(int(e.severity)) + '|' + e.text + '|' + e.duration_text + '\n';
  }
  if (es != s.events_sig || !s.events_built) {
    s.events_sig = es;
    s.events_built = true;
    clear_children(s.events);
    if (!n) {
      gtk_box_pack_start(GTK_BOX(s.events), dim(hlabel("Nothing yet.")), FALSE, FALSE, 0);
    }
    for (size_t i = 0; i < n; ++i) {
      const auto& e = d.events[i];
      GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget* t = hlabel(nullptr);
      char* m = g_markup_printf_escaped("<span font_family='monospace'>%s</span>", e.time_text.c_str());
      gtk_label_set_markup(GTK_LABEL(t), m);
      g_free(m);
      gtk_widget_set_valign(t, GTK_ALIGN_START);
      gtk_box_pack_start(GTK_BOX(row), dim(t), FALSE, FALSE, 0);
      GtkWidget* dot = hlabel(nullptr);
      set_markup(dot, "●", severity_color(e.severity));
      gtk_widget_set_valign(dot, GTK_ALIGN_START);
      gtk_box_pack_start(GTK_BOX(row), dot, FALSE, FALSE, 0);
      std::string text = e.text;
      if (!e.duration_text.empty()) text += " (" + e.duration_text + ")";
      GtkWidget* tl = wrapped(hlabel(text.c_str()), 100);
      gtk_label_set_selectable(GTK_LABEL(tl), TRUE);
      gtk_widget_set_can_focus(tl, FALSE);
      gtk_widget_set_hexpand(tl, TRUE);
      gtk_box_pack_start(GTK_BOX(row), tl, TRUE, TRUE, 0);
      gtk_box_pack_start(GTK_BOX(s.events), row, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(s.events);
  }
}

void diag_refresh(const std::shared_ptr<Diag>& s) {
  if (!s->w || s->loading) return;
  s->loading = true;
  struct Fetched {
    bool ok = false;
    vc::Diagnostics d;
    std::string error;
  };
  const unsigned port = s->port;
  run_async<Fetched>(
      [port] {
        Fetched f;
        f.ok = vc::fetch_diagnostics(port, &f.d, &f.error);
        return f;
      },
      [s](const Fetched& f) {
        s->loading = false;
        if (!s->w) return;
        if (!f.ok) {
          set_markup(s->err, f.error.empty() ? "The Virgil service is not running." : f.error, kRed);
          gtk_widget_show(s->err);
          return;
        }
        gtk_widget_hide(s->err);
        s->d = f.d;
        s->have = true;
        diag_apply(*s);
      });
}

gboolean diag_draw(GtkWidget* w, cairo_t* cr, gpointer) {
  const std::shared_ptr<Diag> s = g_diag;
  const double W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
  set_rgb(cr, 0x8C9097, 0.10);
  cairo_rectangle(cr, 0, 0, W, H);
  cairo_fill(cr);
  set_rgb(cr, 0x8C9097, 0.35);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, 0, H - 0.5);
  cairo_line_to(cr, W, H - 0.5);
  cairo_stroke(cr);
  if (!s || !s->have) return TRUE;
  for (const vc::DiagBar& b : vc::diag_chart_layout(s->d, float(W), float(H))) {
    set_rgb(cr, vc::diag_category_color(b.category));
    cairo_rectangle(cr, b.x, b.y, b.w, b.h);
    cairo_fill(cr);
  }
  return TRUE;
}

gboolean diag_tooltip(GtkWidget* w, gint x, gint, gboolean keyboard, GtkTooltip* tip, gpointer) {
  const std::shared_ptr<Diag> s = g_diag;
  if (!s || !s->have || keyboard) return FALSE;
  const int i = vc::diag_chart_minute_at(float(x), float(gtk_widget_get_allocated_width(w)));
  if (i < 0 || size_t(i) >= s->d.minutes.size()) return FALSE;
  gtk_tooltip_set_text(tip, s->d.minutes[size_t(i)].tooltip.c_str());
  return TRUE;
}

void diag_save_report(const std::shared_ptr<Diag>& s) {
  gtk_widget_set_sensitive(s->save, FALSE);
  using Result = std::pair<bool, std::string>;  // ok, report text or error
  const unsigned port = s->port;
  run_async<Result>(
      [port] {
        std::string text, error;
        const bool ok = vc::fetch_report(port, &text, &error);
        return Result(ok, ok ? text : error);
      },
      [s](const Result& r) {
        if (!s->w) return;
        gtk_widget_set_sensitive(s->save, TRUE);
        GtkWindow* win = GTK_WINDOW(s->w);
        if (!r.first) {
          message(win, GTK_MESSAGE_WARNING, "Could not get the report: " + r.second);
          return;
        }
        GtkWidget* fc = gtk_file_chooser_dialog_new("Save report", win, GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel",
                                                    GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), vc::default_report_file_name().c_str());
        GtkFileFilter* ff = gtk_file_filter_new();
        gtk_file_filter_set_name(ff, "Text files");
        gtk_file_filter_add_pattern(ff, "*.txt");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), ff);
        if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
          char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
          GError* err = nullptr;
          if (path && !g_file_set_contents(path, r.second.data(), gssize(r.second.size()), &err)) {
            message(win, GTK_MESSAGE_WARNING, std::string("Could not save the report: ") + err->message);
            g_error_free(err);
          }
          g_free(path);
        }
        gtk_widget_destroy(fc);
      });
}

void diag_clear(const std::shared_ptr<Diag>& s) {
  gtk_widget_set_sensitive(s->clear, FALSE);
  using Result = std::pair<bool, std::string>;
  const unsigned port = s->port;
  run_async<Result>(
      [port] {
        std::string error;
        const bool ok = vc::reset_diagnostics(port, &error);
        return Result(ok, error);
      },
      [s](const Result& r) {
        if (!s->w) return;
        gtk_widget_set_sensitive(s->clear, TRUE);
        if (!r.first) {
          set_markup(s->err, r.second, kRed);
          gtk_widget_show(s->err);
          return;
        }
        diag_refresh(s);
      });
}

// ---- Channel names ----------------------------------------------------------------------

struct Names {
  GtkWidget *w = nullptr, *save, *err;
  std::vector<GtkWidget*> entries[2], errs[2];  // [0] = transmit, [1] = receive
  unsigned port = 0;
  vc::ChannelNames base, edited;
  bool saving = false;
};
std::shared_ptr<Names> g_names;
bool g_names_loading = false;

void names_validate(Names& s) {
  const auto errors = vc::validate_channel_names(s.edited);
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    for (size_t i = 0; i < s.entries[dir].size(); ++i) {
      const std::string e = vc::name_error(errors, tx, unsigned(i));
      GtkStyleContext* c = gtk_widget_get_style_context(s.entries[dir][i]);
      if (e.empty()) gtk_style_context_remove_class(c, "error");
      else gtk_style_context_add_class(c, "error");
      gtk_label_set_text(GTK_LABEL(s.errs[dir][i]), e.c_str());
      gtk_widget_set_visible(s.errs[dir][i], !e.empty());
    }
  }
  if (!s.saving) {
    gtk_widget_set_sensitive(s.save, errors.empty());
    if (errors.empty()) gtk_label_set_text(GTK_LABEL(s.err), "");
    else set_markup(s.err, "Correct the marked names to save.", kRed);
  }
}

GtkWidget* names_column(Names& s, bool tx, const std::vector<std::string>& live) {
  const int dir = tx ? 0 : 1;
  const unsigned count = tx ? s.edited.tx : s.edited.rx;
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  if (!count) gtk_grid_attach(GTK_GRID(grid), dim(hlabel(tx ? "No transmit channels." : "No receive channels.")), 0, 0, 2, 1);
  for (unsigned i = 0; i < count; ++i) {
    GtkWidget* num = dim(hlabel(std::to_string(i + 1).c_str(), 1));
    gtk_widget_set_size_request(num, 28, -1);
    gtk_grid_attach(GTK_GRID(grid), num, 0, int(2 * i), 1, 1);
    GtkWidget* e = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(e), int(vc::kMaxNameLength));
    gtk_entry_set_width_chars(GTK_ENTRY(e), 24);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_entry_set_text(GTK_ENTRY(e), s.edited.names(tx)[i].c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), vc::channel_name_placeholder(live, tx, i).c_str());
    g_object_set_data(G_OBJECT(e), "vg-dir", GINT_TO_POINTER(dir));
    g_object_set_data(G_OBJECT(e), "vg-index", GINT_TO_POINTER(int(i)));
    g_signal_connect(e, "changed", G_CALLBACK(+[](GtkEditable* ed, gpointer) {
                       const std::shared_ptr<Names> s = g_names;
                       if (!s || !s->w) return;
                       const bool tx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ed), "vg-dir")) == 0;
                       const size_t i = size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ed), "vg-index")));
                       auto& v = s->edited.names(tx);
                       if (i < v.size()) v[i] = gtk_entry_get_text(GTK_ENTRY(ed));
                       names_validate(*s);
                     }),
                     nullptr);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_grid_attach(GTK_GRID(grid), e, 1, int(2 * i), 1, 1);
    GtkWidget* el = wrapped(hlabel(""), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(el), "vg-field-error");
    gtk_widget_set_no_show_all(el, TRUE);
    gtk_grid_attach(GTK_GRID(grid), el, 1, int(2 * i + 1), 1, 1);
    s.entries[dir].push_back(e);
    s.errs[dir].push_back(el);
  }
  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 2);
  gtk_container_add(GTK_CONTAINER(scroll), grid);
  char title[64];
  std::snprintf(title, sizeof title, tx ? "Transmit (%u)" : "Receive (%u)", count);
  return frame(title, scroll);
}

void names_save(const std::shared_ptr<Names>& s) {
  if (s->saving) return;
  if (!vc::validate_channel_names(s->edited).empty()) return;
  s->saving = true;
  gtk_widget_set_sensitive(s->save, FALSE);
  gtk_label_set_text(GTK_LABEL(s->err), "Saving…");
  using Result = std::pair<bool, std::string>;
  const unsigned port = s->port;
  const vc::ChannelNames base = s->base, edited = s->edited;
  run_async<Result>(
      [port, base, edited] {
        std::string error;
        const bool ok = vc::save_channel_names(port, base, edited, &error);
        return Result(ok, error);
      },
      [s](const Result& r) {
        s->saving = false;
        if (!s->w) return;
        if (r.first) {
          gtk_widget_destroy(s->w);
          return;
        }
        names_validate(*s);
        set_markup(s->err, r.second, kRed);
      });
}

void build_names(GtkWindow* parent, GdkPixbuf* icon, unsigned port, const vc::ConfigText& conf,
                 const std::vector<std::string>& tx_live, const std::vector<std::string>& rx_live) {
  install_css();
  auto s = std::make_shared<Names>();
  s->port = port;
  s->base = vc::load_channel_names(conf.text);
  s->edited = s->base;
  GtkWidget* w = s->w = new_window(parent, icon, "Channel names", 680, -1);
  GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(v), 14);
  gtk_container_add(GTK_CONTAINER(w), v);
  GtkWidget* help = wrapped(hlabel("Leave a field empty for the default (TX 1, RX 1, …). Names changed in Dante "
                                   "Controller appear here too. Up to 31 characters, unique per direction, "
                                   "without = . @ or \"."),
                            90);
  gtk_box_pack_start(GTK_BOX(v), dim(help), FALSE, FALSE, 0);
  GtkWidget* cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_set_homogeneous(GTK_BOX(cols), TRUE);
  gtk_box_pack_start(GTK_BOX(cols), names_column(*s, true, tx_live), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(cols), names_column(*s, false, rx_live), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(v), cols, TRUE, TRUE, 0);
  s->err = wrapped(hlabel(""), 90);
  gtk_box_pack_start(GTK_BOX(v), s->err, FALSE, FALSE, 0);
  GtkWidget* foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  s->save = gtk_button_new_with_mnemonic("_Save");
  gtk_style_context_add_class(gtk_widget_get_style_context(s->save), "suggested-action");
  gtk_widget_set_can_default(s->save, TRUE);
  GtkWidget* cancel = gtk_button_new_with_mnemonic("_Cancel");
  gtk_box_pack_end(GTK_BOX(foot), s->save, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(foot), cancel, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), foot, FALSE, FALSE, 0);

  g_signal_connect(s->save, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_names && g_names->w) names_save(g_names);
                   }),
                   nullptr);
  g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_widget_destroy), w);
  g_signal_connect(w, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer) -> gboolean {
                     return g_names && g_names->saving;  // closing waits for Save to finish
                   }),
                   nullptr);
  g_signal_connect(w, "destroy", G_CALLBACK(+[](GtkWidget* win, gpointer) {
                     if (g_names && g_names->w == win) {
                       g_names->w = nullptr;
                       g_names.reset();
                     }
                   }),
                   nullptr);
  g_names = s;
  names_validate(*s);
  gtk_widget_show_all(w);
  gtk_widget_grab_default(s->save);
  if (!s->entries[0].empty()) gtk_widget_grab_focus(s->entries[0][0]);
  else if (!s->entries[1].empty()) gtk_widget_grab_focus(s->entries[1][0]);
}

// ---- Edit configuration -------------------------------------------------------------------

struct Conf {
  GtkWidget *w = nullptr, *view, *status, *apply, *reload;
  unsigned port = 0;
  std::string version;
  bool busy = false;
};
std::shared_ptr<Conf> g_conf;

void conf_buttons(Conf& s) {
  gtk_widget_set_sensitive(s.apply, !s.busy);
  gtk_widget_set_sensitive(s.reload, !s.busy);
}

// Loads the file into the editor; `note` (if any) stays in the status line.
void conf_load(const std::shared_ptr<Conf>& s, const std::string& note) {
  s->busy = true;
  conf_buttons(*s);
  if (note.empty()) gtk_label_set_text(GTK_LABEL(s->status), "Loading…");
  struct Fetched {
    bool ok = false;
    vc::ConfigText c;
    std::string error;
  };
  const unsigned port = s->port;
  run_async<Fetched>(
      [port] {
        Fetched f;
        f.ok = vc::fetch_config(port, &f.c, &f.error);
        return f;
      },
      [s, note](const Fetched& f) {
        s->busy = false;
        if (!s->w) return;
        conf_buttons(*s);
        if (!f.ok) {
          set_markup(s->status, f.error.empty() ? "The Virgil service is not running." : f.error, kRed);
          return;
        }
        GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->view));
        const std::string text = vc::valid_utf8(f.c.text);
        gtk_text_buffer_set_text(b, text.c_str(), -1);
        gtk_text_buffer_set_modified(b, FALSE);
        GtkTextIter start;
        gtk_text_buffer_get_start_iter(b, &start);
        gtk_text_buffer_place_cursor(b, &start);
        s->version = f.c.version;
        gtk_label_set_text(GTK_LABEL(s->status), note.c_str());
      });
}

bool conf_confirm_discard(Conf& s, const char* question) {
  GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s.view));
  if (!gtk_text_buffer_get_modified(b)) return true;
  GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(s.w), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                        "%s", question);
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "Your changes to the text are lost.");
  gtk_dialog_add_buttons(GTK_DIALOG(d), "_Keep editing", GTK_RESPONSE_CANCEL, "_Discard", GTK_RESPONSE_ACCEPT,
                         nullptr);
  gtk_window_set_title(GTK_WINDOW(d), "Virgil");
  const int r = gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
  return r == GTK_RESPONSE_ACCEPT;
}

void conf_apply(const std::shared_ptr<Conf>& s) {
  if (s->busy) return;
  GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->view));
  GtkTextIter a, z;
  gtk_text_buffer_get_bounds(b, &a, &z);
  char* t = gtk_text_buffer_get_text(b, &a, &z, FALSE);
  const std::string text = t ? t : "";
  g_free(t);
  s->busy = true;
  conf_buttons(*s);
  gtk_label_set_text(GTK_LABEL(s->status), "Saving…");
  const unsigned port = s->port;
  const std::string version = s->version;
  run_async<vc::SaveResult>([port, text, version] { return vc::save_config_text(port, text, version); },
                            [s](const vc::SaveResult& r) {
                              s->busy = false;
                              if (!s->w) return;
                              conf_buttons(*s);
                              if (r.ok) {
                                conf_load(s, vc::saved_message(r));
                                return;
                              }
                              if (r.conflict) {
                                set_markup(s->status, "The configuration was changed elsewhere since it was loaded.",
                                           kRed);
                                GtkWidget* d = gtk_message_dialog_new(
                                    GTK_WINDOW(s->w), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                    "The configuration was changed elsewhere");
                                gtk_message_dialog_format_secondary_text(
                                    GTK_MESSAGE_DIALOG(d),
                                    "Dante Controller, Stop or another editor changed it after it was "
                                    "loaded here. Reload it? Your edits here are lost.");
                                gtk_dialog_add_buttons(GTK_DIALOG(d), "_Keep editing", GTK_RESPONSE_CANCEL, "_Reload",
                                                       GTK_RESPONSE_ACCEPT, nullptr);
                                gtk_window_set_title(GTK_WINDOW(d), "Virgil");
                                const int resp = gtk_dialog_run(GTK_DIALOG(d));
                                gtk_widget_destroy(d);
                                if (resp == GTK_RESPONSE_ACCEPT && s->w) conf_load(s, "");
                                return;
                              }
                              set_markup(s->status, r.error.empty() ? "Could not save the configuration." : r.error,
                                         kRed);
                            });
}

}  // namespace

void show_diagnostics(GtkWindow* parent, GdkPixbuf* icon, unsigned port) {
  if (g_diag && g_diag->w) {
    gtk_window_present(GTK_WINDOW(g_diag->w));
    diag_refresh(g_diag);
    return;
  }
  install_css();
  auto s = std::make_shared<Diag>();
  s->port = port;
  GtkWidget* w = s->w = new_window(parent, icon, "Dropouts and diagnostics", 680, 600);
  GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(v), 14);
  gtk_container_add(GTK_CONTAINER(w), v);

  GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  s->pill = hlabel("");
  gtk_style_context_add_class(gtk_widget_get_style_context(s->pill), "vg-pill");
  gtk_widget_set_no_show_all(s->pill, TRUE);
  gtk_widget_set_valign(s->pill, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(head), s->pill, FALSE, FALSE, 0);
  s->since = dim(hlabel("Loading…"));
  gtk_label_set_ellipsize(GTK_LABEL(s->since), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start(GTK_BOX(head), s->since, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(v), head, FALSE, FALSE, 0);

  s->chart = gtk_drawing_area_new();
  gtk_widget_set_size_request(s->chart, 480, 64);
  gtk_widget_set_has_tooltip(s->chart, TRUE);
  g_signal_connect(s->chart, "draw", G_CALLBACK(diag_draw), nullptr);
  g_signal_connect(s->chart, "query-tooltip", G_CALLBACK(diag_tooltip), nullptr);
  gtk_box_pack_start(GTK_BOX(v), s->chart, FALSE, FALSE, 0);

  GtkWidget* legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_pack_start(GTK_BOX(legend), dim(hlabel("last 60 minutes, per minute:")), FALSE, FALSE, 0);
  for (int c = 0; c < vc::kDiagCategories; ++c) {
    const auto cat = vc::DiagCategory(c);
    GtkWidget* l = hlabel(nullptr);
    char* m = g_markup_printf_escaped("<span foreground='%s'>■</span> %s", hex(vc::diag_category_color(cat)).c_str(),
                                      vc::diag_category_label(cat));
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_box_pack_start(GTK_BOX(legend), l, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(v), legend, FALSE, FALSE, 0);

  s->hints = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_no_show_all(s->hints, TRUE);
  gtk_box_pack_start(GTK_BOX(v), s->hints, FALSE, FALSE, 0);

  GtkWidget* evh = heading("Events");
  gtk_widget_set_margin_top(evh, 4);
  gtk_box_pack_start(GTK_BOX(v), evh, FALSE, FALSE, 0);
  s->events = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_style_context_add_class(gtk_widget_get_style_context(s->events), "vg-events");
  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
  gtk_widget_set_size_request(scroll, -1, 160);
  gtk_container_add(GTK_CONTAINER(scroll), s->events);
  gtk_box_pack_start(GTK_BOX(v), scroll, TRUE, TRUE, 0);

  s->err = wrapped(hlabel(""), 90);
  gtk_widget_set_no_show_all(s->err, TRUE);
  gtk_box_pack_start(GTK_BOX(v), s->err, FALSE, FALSE, 0);

  GtkWidget* foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  s->save = gtk_button_new_with_mnemonic("_Save report…");
  s->clear = gtk_button_new_with_mnemonic("C_lear");
  gtk_widget_set_tooltip_text(s->clear, "Forget the dropouts and events counted so far");
  GtkWidget* close = gtk_button_new_with_mnemonic("_Close");
  gtk_box_pack_start(GTK_BOX(foot), s->save, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(foot), s->clear, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(foot), close, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), foot, FALSE, FALSE, 0);

  g_signal_connect(s->save, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_diag && g_diag->w) diag_save_report(g_diag);
                   }),
                   nullptr);
  g_signal_connect(s->clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_diag && g_diag->w) diag_clear(g_diag);
                   }),
                   nullptr);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), w);
  g_signal_connect(w, "destroy", G_CALLBACK(+[](GtkWidget* win, gpointer) {
                     if (g_diag && g_diag->w == win) {
                       if (g_diag->timer) g_source_remove(g_diag->timer);
                       g_diag->w = nullptr;
                       g_diag.reset();
                     }
                   }),
                   nullptr);
  // Every 2 s while the window is on screen (not while minimised or hidden).
  s->timer = g_timeout_add(2000, +[](gpointer) -> gboolean {
    const std::shared_ptr<Diag> d = g_diag;
    if (!d || !d->w) return G_SOURCE_REMOVE;
    GdkWindow* gw = gtk_widget_get_window(d->w);
    const bool iconified = gw && (gdk_window_get_state(gw) & GDK_WINDOW_STATE_ICONIFIED);
    if (gtk_widget_get_mapped(d->w) && !iconified) diag_refresh(d);
    return G_SOURCE_CONTINUE;
  }, nullptr);
  g_diag = s;
  gtk_widget_show_all(w);
  diag_refresh(s);
}

void show_channel_names(GtkWindow* parent, GdkPixbuf* icon, unsigned port, const std::vector<std::string>& tx_live,
                        const std::vector<std::string>& rx_live) {
  if (g_names && g_names->w) {
    gtk_window_present(GTK_WINDOW(g_names->w));
    return;
  }
  if (g_names_loading) return;
  g_names_loading = true;
  // A fresh copy each time: Dante Controller renames land in the file too.
  using Fetched = std::pair<std::string, vc::ConfigText>;  // error, config
  run_async<Fetched>(
      [port] {
        Fetched f;
        if (!vc::fetch_config(port, &f.second, &f.first) && f.first.empty())
          f.first = "The Virgil service is not running.";
        return f;
      },
      [parent, icon, port, tx_live, rx_live](const Fetched& f) {
        g_names_loading = false;
        if (g_names && g_names->w) {
          gtk_window_present(GTK_WINDOW(g_names->w));
          return;
        }
        if (!f.first.empty()) {
          message(parent, GTK_MESSAGE_INFO, "Channel names cannot be changed now: " + f.first);
          return;
        }
        build_names(parent, icon, port, f.second, tx_live, rx_live);
      });
}

void show_config_editor(GtkWindow* parent, GdkPixbuf* icon, unsigned port) {
  if (g_conf && g_conf->w) {
    gtk_window_present(GTK_WINDOW(g_conf->w));
    return;
  }
  install_css();
  auto s = std::make_shared<Conf>();
  s->port = port;
  GtkWidget* w = s->w = new_window(parent, icon, "Virgil configuration", 760, 580);
  GtkWidget* v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(v), 14);
  gtk_container_add(GTK_CONTAINER(w), v);
  gtk_box_pack_start(GTK_BOX(v), dim(hlabel("See virgil.conf.example for every option.")), FALSE, FALSE, 0);
  GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
  s->view = gtk_text_view_new();
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(s->view), TRUE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(s->view), 6);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(s->view), 4);
  gtk_container_add(GTK_CONTAINER(scroll), s->view);
  gtk_box_pack_start(GTK_BOX(v), scroll, TRUE, TRUE, 0);
  s->status = wrapped(hlabel(""), 100);
  gtk_label_set_selectable(GTK_LABEL(s->status), TRUE);
  gtk_widget_set_can_focus(s->status, FALSE);
  gtk_box_pack_start(GTK_BOX(v), s->status, FALSE, FALSE, 0);
  GtkWidget* foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  s->reload = gtk_button_new_with_mnemonic("_Reload");
  s->apply = gtk_button_new_with_mnemonic("_Apply & restart");
  gtk_style_context_add_class(gtk_widget_get_style_context(s->apply), "suggested-action");
  gtk_widget_set_tooltip_text(s->apply, "Save the file; the audio engine restarts with it");
  GtkWidget* close = gtk_button_new_with_mnemonic("_Close");
  gtk_box_pack_start(GTK_BOX(foot), s->reload, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(foot), s->apply, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(foot), close, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), foot, FALSE, FALSE, 0);

  g_signal_connect(s->apply, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     if (g_conf && g_conf->w) conf_apply(g_conf);
                   }),
                   nullptr);
  g_signal_connect(s->reload, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
                     const std::shared_ptr<Conf> s = g_conf;
                     if (s && s->w && !s->busy && conf_confirm_discard(*s, "Reload the configuration?") && s->w)
                       conf_load(s, "");
                   }),
                   nullptr);
  auto close_fn = +[](GtkWidget*, gpointer) {
    const std::shared_ptr<Conf> s = g_conf;
    if (!s || !s->w) return;
    if (s->busy) return;  // wait for the save or load to finish
    if (conf_confirm_discard(*s, "Close without applying?") && s->w) gtk_widget_destroy(s->w);
  };
  g_signal_connect(close, "clicked", G_CALLBACK(close_fn), nullptr);
  g_signal_connect(w, "delete-event", G_CALLBACK(+[](GtkWidget* win, GdkEvent*, gpointer) -> gboolean {
                     const std::shared_ptr<Conf> s = g_conf;
                     if (!s || s->w != win) return FALSE;
                     if (s->busy) return TRUE;
                     return !conf_confirm_discard(*s, "Close without applying?");
                   }),
                   nullptr);
  g_signal_connect(w, "destroy", G_CALLBACK(+[](GtkWidget* win, gpointer) {
                     if (g_conf && g_conf->w == win) {
                       g_conf->w = nullptr;
                       g_conf.reset();
                     }
                   }),
                   nullptr);
  g_conf = s;
  gtk_widget_show_all(w);
  conf_load(s, "");
}

}  // namespace vcgtk
