// Aether UI — GTK4 backend for Aether
// Port of aether-ui-gtk4 (Rust) to C, calling the GTK4 C API directly.
//
// This file is compiled separately and linked with Aether programs via:
//   ae build app.ae --extra aether_ui_gtk4.c $(pkg-config --cflags --libs gtk4)

#include "aether_ui_backend.h"
#include "aether_ui_system_extras.h"
#include "aether_ui_sni.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef AEUI_HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

// ---------------------------------------------------------------------------
// Closure struct — must match Aether codegen's _AeClosure layout.
// box_closure() returns a malloc'd copy of this struct.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

// ---------------------------------------------------------------------------
// GTK4 initialization — must be called before creating any widgets.
// ---------------------------------------------------------------------------
static int gtk_initialized = 0;

static void ensure_gtk_init(void) {
    if (!gtk_initialized) {
        gtk_init();
        gtk_initialized = 1;
    }
}

// ---------------------------------------------------------------------------
// AETHER_UI_HEADLESS contract — set by CI, widget smoke tests, or any
// caller that wants to exercise the backend without a user. Every API
// that would otherwise show a modal dialog (alert, file_open, …)
// returns without UI when this flag is set. Without this, modal
// dialogs can block waiting for interaction that never comes.
// ---------------------------------------------------------------------------
static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

// ---------------------------------------------------------------------------
// Widget registry — flat array of GtkWidget*, 1-based handles.
//
// Slots self-NULL when their widget is finalized (weak ref below), so a
// handle held past window teardown resolves to NULL instead of a dangling
// pointer. Every consumer of aether_ui_get_widget() already NULL-checks;
// without this, the vg live-refresh timer's canvas_redraw between window
// destroy and main-loop exit called gtk_widget_queue_draw on freed memory
// (a Gtk-CRITICAL spray on exit, and a latent use-after-free).
// ---------------------------------------------------------------------------
static GtkWidget** widgets = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

static void on_widget_finalized(gpointer data, GObject* where_the_object_was) {
    (void)where_the_object_was;
    int idx = GPOINTER_TO_INT(data);          // index, not pointer: realloc-safe
    if (idx >= 0 && idx < widget_count) widgets[idx] = NULL;
}

int aether_ui_register_widget(void* widget) {
    if (widget_count >= widget_capacity) {
        widget_capacity = widget_capacity == 0 ? 64 : widget_capacity * 2;
        widgets = realloc(widgets, sizeof(GtkWidget*) * widget_capacity);
    }
    widgets[widget_count] = (GtkWidget*)widget;
    g_object_weak_ref(G_OBJECT(widget), on_widget_finalized,
                      GINT_TO_POINTER(widget_count));
    widget_count++;
    return widget_count; // 1-based
}

void* aether_ui_get_widget(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return widgets[handle - 1];
}

// ---------------------------------------------------------------------------
// Reactive state — flat array of doubles, 1-based handles.
// ---------------------------------------------------------------------------
typedef struct {
    int state_handle;
    int text_handle;
    char* prefix;
    char* suffix;
} TextBinding;

static double* state_values = NULL;
static int state_count = 0;
static int state_capacity = 0;

static TextBinding* text_bindings = NULL;
static int text_binding_count = 0;
static int text_binding_capacity = 0;

int aether_ui_state_create(double initial) {
    if (state_count >= state_capacity) {
        state_capacity = state_capacity == 0 ? 32 : state_capacity * 2;
        state_values = realloc(state_values, sizeof(double) * state_capacity);
    }
    state_values[state_count] = initial;
    state_count++;
    return state_count; // 1-based
}

double aether_ui_state_get(int handle) {
    if (handle < 1 || handle > state_count) return 0.0;
    return state_values[handle - 1];
}

static void update_text_bindings(int state_handle);

void aether_ui_state_set(int handle, double value) {
    if (handle < 1 || handle > state_count) return;
    state_values[handle - 1] = value;
    update_text_bindings(handle);
}

void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix) {
    if (text_binding_count >= text_binding_capacity) {
        text_binding_capacity = text_binding_capacity == 0 ? 32 : text_binding_capacity * 2;
        text_bindings = realloc(text_bindings, sizeof(TextBinding) * text_binding_capacity);
    }
    TextBinding* b = &text_bindings[text_binding_count++];
    b->state_handle = state_handle;
    b->text_handle = text_handle;
    b->prefix = prefix ? strdup(prefix) : strdup("");
    b->suffix = suffix ? strdup(suffix) : strdup("");

    double val = aether_ui_state_get(state_handle);
    char buf[256];
    if (val == (int)val) {
        snprintf(buf, sizeof(buf), "%s%d%s", b->prefix, (int)val, b->suffix);
    } else {
        snprintf(buf, sizeof(buf), "%s%.2f%s", b->prefix, val, b->suffix);
    }
    aether_ui_text_set_string(text_handle, buf);
}

static void update_text_bindings(int state_handle) {
    double val = aether_ui_state_get(state_handle);
    for (int i = 0; i < text_binding_count; i++) {
        TextBinding* b = &text_bindings[i];
        if (b->state_handle != state_handle) continue;
        char buf[256];
        if (val == (int)val) {
            snprintf(buf, sizeof(buf), "%s%d%s", b->prefix, (int)val, b->suffix);
        } else {
            snprintf(buf, sizeof(buf), "%s%.2f%s", b->prefix, val, b->suffix);
        }
        aether_ui_text_set_string(b->text_handle, buf);
    }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
typedef struct {
    char* title;
    int width;
    int height;
    int root_handle;
    GtkApplication* gtk_app;
} AppEntry;

static AppEntry* apps = NULL;
static int app_count = 0;
static int app_capacity = 0;

// ---------------------------------------------------------------------------
// Surface table — "DSL with Scope" surfaces (window / render_to / record).
//
// A surface is the ambient destination a drawing/widget block populates.
// Children attach to the surface's content container (the handle pushed as
// _ctx); the surface row carries the rest: kind, the owning app handle (for
// `window`, so its builder body can run the loop after the block), and a
// collected diagnostic count (interactive verbs used on a bounded surface).
//
// Keyed by the content-container handle so a child verb's _ctx maps back to
// the surface. Single-window-at-a-time is fine — surfaces don't nest.
// ---------------------------------------------------------------------------
#define AUI_SURFACE_WINDOW 0
#define AUI_SURFACE_RENDER 1   // render_to (bounded; flush to target)
#define AUI_SURFACE_RECORD 2   // record    (bounded; capture for tests)

typedef struct {
    int container_handle;  // the _ctx children attach to (key)
    int kind;              // AUI_SURFACE_*
    int app_handle;        // window: owning app (0 for bounded surfaces)
    int diag_count;        // # interactive-verb-on-bounded-surface diagnostics
} SurfaceEntry;

static SurfaceEntry* surfaces = NULL;
static int surface_count = 0;
static int surface_capacity = 0;

static SurfaceEntry* surface_for_container(int container_handle) {
    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i].container_handle == container_handle) return &surfaces[i];
    }
    return NULL;
}

static SurfaceEntry* surface_add(int container_handle, int kind, int app_handle) {
    if (surface_count >= surface_capacity) {
        surface_capacity = surface_capacity == 0 ? 4 : surface_capacity * 2;
        surfaces = realloc(surfaces, sizeof(SurfaceEntry) * surface_capacity);
    }
    SurfaceEntry* s = &surfaces[surface_count++];
    s->container_handle = container_handle;
    s->kind = kind;
    s->app_handle = app_handle;
    s->diag_count = 0;
    return s;
}

// ---------------------------------------------------------------------------
// Deferred-flush registry — the AeVG `vg { … }` block records its shapes
// without drawing (so a shape's trailing fill()/stroke() is known before the
// backend call), then registers a boxed Aether closure that flushes the
// recorded scene. The enclosing surface body (window / render_to / record)
// drains this registry after its block closes, BEFORE the window's event loop
// blocks — so the scene's command buffer is built with final colours. This
// keeps the layering clean: aether_ui owns the registry of opaque closures and
// invokes them; only the vg layer knows what a scene is.
// ---------------------------------------------------------------------------
static AeClosure** deferred_flushes = NULL;
static int deferred_flush_count = 0;
static int deferred_flush_capacity = 0;

void aether_ui_register_deferred_flush_impl(void* boxed_closure) {
    if (!boxed_closure) return;
    if (deferred_flush_count >= deferred_flush_capacity) {
        deferred_flush_capacity = deferred_flush_capacity == 0 ? 4
                                  : deferred_flush_capacity * 2;
        deferred_flushes = realloc(deferred_flushes,
                                   sizeof(AeClosure*) * deferred_flush_capacity);
    }
    deferred_flushes[deferred_flush_count++] = (AeClosure*)boxed_closure;
}

// Invoke every registered flush closure, then clear the registry (each scene
// flushes exactly once per surface). Called by the surface bodies.
void aether_ui_surface_flush_deferred_impl(void) {
    for (int i = 0; i < deferred_flush_count; i++) {
        AeClosure* c = deferred_flushes[i];
        if (c && c->fn) {
            ((void(*)(void*))c->fn)(c->env);
        }
    }
    deferred_flush_count = 0;
}

// Create a surface container of the given kind: a detached root vstack the
// block's children attach to (pushed as _ctx). No app/loop is created here —
// the zero-arg `with` factory runs BEFORE the block and doesn't know the
// window title/size. The `window` builder BODY (which has title/w/h and the
// now-populated container) creates the app + runs the loop afterward, via
// aether_ui_surface_run_impl. This matches the builder "configure then
// execute" lifecycle: children fill the container, then the body acts.
int aether_ui_surface_container_new_impl(int kind) {
    int container = aether_ui_vstack_create(0);
    surface_add(container, kind, 0);
    return container;
}

// window builder body: create the app, mount the populated container, run
// the loop. Called after the block. Only meaningful for a WINDOW surface.
void aether_ui_surface_run_impl(int container_handle,
                                const char* title, int width, int height) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s || s->kind != AUI_SURFACE_WINDOW) return;
    // Flush any deferred AeVG scenes recorded during the block, so their
    // command buffers are built with final colours before the loop starts.
    aether_ui_surface_flush_deferred_impl();
    int app = aether_ui_app_create(title, width, height);
    s->app_handle = app;
    aether_ui_app_set_body(app, container_handle);
    aether_ui_app_run_raw(app);
}

// Record an interactive-verb-on-bounded-surface diagnostic; returns 1 if the
// container is a bounded surface (so the caller knows the verb is inert), 0
// if it's a live window (verb is fine) or unknown.
int aether_ui_surface_note_interactive_impl(int container_handle) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s) return 0;
    if (s->kind == AUI_SURFACE_WINDOW) return 0;
    s->diag_count++;
    return 1;
}

int aether_ui_surface_diag_count_impl(int container_handle) {
    SurfaceEntry* s = surface_for_container(container_handle);
    return s ? s->diag_count : 0;
}

static void on_activate(GtkApplication* gtk_app, gpointer user_data) {
    AppEntry* entry = (AppEntry*)user_data;
    GtkWidget* window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(window), entry->title);
    gtk_window_set_default_size(GTK_WINDOW(window), entry->width, entry->height);

    if (entry->root_handle > 0) {
        GtkWidget* root = aether_ui_get_widget(entry->root_handle);
        if (root) {
            gtk_window_set_child(GTK_WINDOW(window), root);
        }
    }

    // Honor AETHER_UI_HEADLESS for CI and unattended scenarios. The window
    // is realized, the event loop still pumps, and the test server still
    // responds — but the window is never presented. Matches the Win32
    // backend's SW_HIDE semantics.
    const char* headless = getenv("AETHER_UI_HEADLESS");
    int is_headless = headless && headless[0] && headless[0] != '0';
    if (is_headless) {
        gtk_widget_realize(window);
    } else {
        gtk_window_present(GTK_WINDOW(window));
    }
}

int aether_ui_app_create(const char* title, int width, int height) {
    if (app_count >= app_capacity) {
        app_capacity = app_capacity == 0 ? 4 : app_capacity * 2;
        apps = realloc(apps, sizeof(AppEntry) * app_capacity);
    }
    AppEntry* e = &apps[app_count];
    e->title = strdup(title);
    e->width = width;
    e->height = height;
    e->root_handle = 0;
    e->gtk_app = NULL;
    app_count++;
    return app_count; // 1-based
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    apps[app_handle - 1].root_handle = root_handle;
}

void aether_ui_app_run_raw(int app_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    AppEntry* e = &apps[app_handle - 1];
    e->gtk_app = gtk_application_new("dev.aether.ui",
        G_APPLICATION_DEFAULT_FLAGS | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(e->gtk_app, "activate", G_CALLBACK(on_activate), e);
    g_application_run(G_APPLICATION(e->gtk_app), 0, NULL);
    g_object_unref(e->gtk_app);
}

// ---------------------------------------------------------------------------
// Widget creation
// ---------------------------------------------------------------------------

int aether_ui_text_create(const char* text) {
    ensure_gtk_init();
    GtkWidget* label = gtk_label_new(text ? text : "");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return aether_ui_register_widget(label);
}

void aether_ui_text_set_string(int handle, const char* text) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_LABEL(w)) {
        gtk_label_set_text(GTK_LABEL(w), text ? text : "");
    }
}

void aether_ui_button_set_label(int handle, const char* label) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_BUTTON(w)) {
        gtk_button_set_label(GTK_BUTTON(w), label ? label : "");
    }
}

// ---------------------------------------------------------------------------
// Right-click context menus.
//
// Two surfaces, chosen at runtime:
//
//   * DEFAULT: a GtkPopover (GtkListBox of rows) parented to the owner. This
//     is the correct native menu on every mainstream Linux desktop — it
//     positions at the pointer, carries the theme's menu styling, and
//     auto-dismisses. Verified working on GNOME/KDE (Wayland) and X11.
//
//   * SOMMELIER FALLBACK: a borderless top-level window. On ChromeOS
//     Crostini's sommelier compositor, GTK4 popovers (xdg_popup subsurfaces)
//     report mapped=1 but never DISPLAY — verified a stock GtkMenuButton /
//     GtkDropDown is equally invisible there. A top-level window maps fine.
//     The trade-off (no pointer-anchored placement, plainer chrome) is
//     accepted only where the popover doesn't work at all. Detected via
//     $SOMMELIER_VERSION (set by sommelier, absent elsewhere).
//     KNOWN WART: sommelier sometimes doesn't present this transient
//     top-level on the first right-click (menu appears on the second tap, or
//     the window can land behind the app) — a sommelier window-management
//     quirk, not a logic bug (each open logs mapped=1). Left as-is; the menu
//     is reachable. A create-once hide/show window may help if it recurs.
//
// The trigger is a GtkEventControllerLegacy filtering for button 3, NOT a
// GtkGestureClick: on sommelier a two-finger tap delivers button-3
// press/release that the click gesture silently drops (verified). The legacy
// controller sees the raw button events on every backend, so it is used
// unconditionally.
//
// Each context_menu_item call records {label, closure}; the surface is built
// on first right-click. Item activation fires the closure and closes.
// ---------------------------------------------------------------------------
typedef struct {
    GtkWidget* owner;
    GtkWidget* popover;             // popover surface (default path)
    GtkWidget* menu_win;            // window surface (sommelier fallback)
    GPtrArray* labels;              // char* (owned copies)
    GPtrArray* closures;            // AeClosure* (boxed, owned by Aether side)
} CtxMenu;

static int aeui_ctx_debug(void) {
    const char* v = getenv("AETHER_UI_DEBUG");
    return v && v[0] && v[0] != '0';
}

// True when GTK4 popovers don't display and we must fall back to a window.
// sommelier (ChromeOS Crostini) is the only known case; it exports
// $SOMMELIER_VERSION. $AETHER_UI_MENU=window forces the fallback for testing.
static int aeui_ctx_use_window(void) {
    const char* force = getenv("AETHER_UI_MENU");
    if (force && strcmp(force, "window") == 0) return 1;
    if (force && strcmp(force, "popover") == 0) return 0;
    return getenv("SOMMELIER_VERSION") != NULL;
}

static void ctx_menu_close(CtxMenu* cm) {
    if (cm->popover) { gtk_popover_popdown(GTK_POPOVER(cm->popover)); }
    if (cm->menu_win) {
        gtk_window_destroy(GTK_WINDOW(cm->menu_win));
        cm->menu_win = NULL;
    }
}

// A menu item fired (popover row or window button) — close, then run closure.
static void ctx_menu_fire(CtxMenu* cm, AeClosure* c) {
    ctx_menu_close(cm);
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
}

static void on_ctx_menu_row_activated(GtkListBox* lb, GtkListBoxRow* row,
                                      gpointer data) {
    (void)lb;
    ctx_menu_fire((CtxMenu*)data, g_object_get_data(G_OBJECT(row), "aeui-closure"));
}

static void on_ctx_menu_item_clicked(GtkButton* btn, gpointer data) {
    ctx_menu_fire((CtxMenu*)data, g_object_get_data(G_OBJECT(btn), "aeui-closure"));
}

static gboolean on_ctx_menu_key(GtkEventControllerKey* k, guint keyval,
                                guint code, GdkModifierType st, gpointer data) {
    (void)k; (void)code; (void)st;
    if (keyval == GDK_KEY_Escape) { ctx_menu_close((CtxMenu*)data); return TRUE; }
    return FALSE;
}

// Window-fallback: losing focus (click elsewhere) closes it.
static void on_ctx_menu_win_active(GObject* win, GParamSpec* ps, gpointer data) {
    (void)ps;
    if (!gtk_window_is_active(GTK_WINDOW(win))) ctx_menu_close((CtxMenu*)data);
}

// --- Default surface: a GtkPopover anchored at (x, y) in owner-local px. ---
static void ctx_menu_open_popover(CtxMenu* cm, double x, double y) {
    ctx_menu_close(cm);
    if (!cm->popover) {
        GtkWidget* list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        g_signal_connect(list, "row-activated",
                         G_CALLBACK(on_ctx_menu_row_activated), cm);
        for (guint i = 0; i < cm->labels->len; i++) {
            GtkWidget* lbl =
                gtk_label_new((const char*)g_ptr_array_index(cm->labels, i));
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_widget_set_margin_start(lbl, 8);  gtk_widget_set_margin_end(lbl, 8);
            gtk_widget_set_margin_top(lbl, 4);    gtk_widget_set_margin_bottom(lbl, 4);
            GtkWidget* row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
            g_object_set_data(G_OBJECT(row), "aeui-closure",
                              g_ptr_array_index(cm->closures, i));
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
        cm->popover = gtk_popover_new();
        gtk_popover_set_has_arrow(GTK_POPOVER(cm->popover), FALSE);
        gtk_popover_set_child(GTK_POPOVER(cm->popover), list);
        gtk_widget_set_parent(cm->popover, cm->owner);
    }
    GdkRectangle at = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(cm->popover), &at);
    gtk_popover_popup(GTK_POPOVER(cm->popover));
    if (aeui_ctx_debug()) {
        fprintf(stderr, "[aeui-ctxmenu] popover at %.0f,%.0f mapped=%d\n",
                x, y, gtk_widget_get_mapped(cm->popover));
    }
}

// --- Fallback surface: a borderless top-level window (sommelier). ---
static void ctx_menu_open_window(CtxMenu* cm) {
    ctx_menu_close(cm);
    GtkWidget* win = gtk_window_new();
    cm->menu_win = win;
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    GtkRoot* owner_root = gtk_widget_get_root(cm->owner);
    if (GTK_IS_WINDOW(owner_root))
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(owner_root));
    gtk_widget_add_css_class(win, "aui-context-menu");

    GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (guint i = 0; i < cm->labels->len; i++) {
        GtkWidget* item =
            gtk_button_new_with_label((const char*)g_ptr_array_index(cm->labels, i));
        gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
        GtkWidget* item_label = gtk_button_get_child(GTK_BUTTON(item));
        if (item_label && GTK_IS_LABEL(item_label))
            gtk_label_set_xalign(GTK_LABEL(item_label), 0.0);
        g_object_set_data(G_OBJECT(item), "aeui-closure",
                          g_ptr_array_index(cm->closures, i));
        g_signal_connect(item, "clicked",
                         G_CALLBACK(on_ctx_menu_item_clicked), cm);
        gtk_box_append(GTK_BOX(list), item);
    }
    gtk_window_set_child(GTK_WINDOW(win), list);

    GtkEventController* key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_ctx_menu_key), cm);
    gtk_widget_add_controller(win, key);
    g_signal_connect(win, "notify::is-active",
                     G_CALLBACK(on_ctx_menu_win_active), cm);
    gtk_window_present(GTK_WINDOW(win));
    if (aeui_ctx_debug())
        fprintf(stderr, "[aeui-ctxmenu] menu window mapped=%d\n",
                gtk_widget_get_mapped(win));
}

// Open the menu at (x, y) owner-local. Routes to the popover (default) or the
// window fallback (sommelier).
static void ctx_menu_open_local(CtxMenu* cm, double x, double y) {
    if (aeui_ctx_use_window()) ctx_menu_open_window(cm);
    else ctx_menu_open_popover(cm, x, y);
}

// Legacy controller: fire on button-3 RELEASE. A GtkGestureClick set to
// button 3 silently drops the two-finger-tap button-3 events on sommelier;
// the legacy controller sees them on every backend. Release (not press) is
// the context-menu convention.
//
// MUST return gboolean: the legacy "event" signal treats a truthy return as
// "handled — stop propagating". This handler was originally declared void,
// so it returned register garbage (usually the nonzero result of the last
// call) and silently ATE every button event for any controller attached
// after it — on the treemap canvas that killed all left-clicks the moment
// the canvas grew a context menu. Return TRUE only when the menu opens.
static gboolean on_ctx_menu_legacy_event(GtkEventControllerLegacy* c,
                                         GdkEvent* ev, gpointer data) {
    (void)c;
    CtxMenu* cm = (CtxMenu*)data;
    GdkEventType t = gdk_event_get_event_type(ev);
    if ((t == GDK_BUTTON_PRESS || t == GDK_BUTTON_RELEASE) && aeui_ctx_debug()) {
        fprintf(stderr, "[aeui-ctxmenu] RAW %s button=%u\n",
                t == GDK_BUTTON_PRESS ? "press" : "release",
                gdk_button_event_get_button(ev));
    }
    if (t == GDK_BUTTON_RELEASE && gdk_button_event_get_button(ev) == 3) {
        double x = 0, y = 0;
        gdk_event_get_position(ev, &x, &y);
        ctx_menu_open_local(cm, x, y);
        return TRUE;
    }
    return FALSE;
}

// Whichever surface is in use, is it currently mapped?
static int ctx_menu_is_mapped(CtxMenu* cm) {
    if (cm->menu_win) return gtk_widget_get_mapped(cm->menu_win) ? 1 : 0;
    if (cm->popover)  return gtk_widget_get_mapped(cm->popover)  ? 1 : 0;
    return 0;
}

// AetherUIDriver hooks — drive and OBSERVE the context menu from the HTTP
// test server (run on the GTK thread via idle callback).
//   aeui_ctx_menu_present:  -1 no menu attached, else item count
//   aeui_ctx_menu_open:     open at the widget's centre; 1 if it mapped
//   aeui_ctx_menu_mapped:   1 if the menu surface is currently mapped
//   aeui_ctx_menu_activate: fire item[idx]'s closure (as an item click does)
int aeui_ctx_menu_present(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return -1;
    return (int)cm->labels->len;
}

int aeui_ctx_menu_open(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return -1;
    ctx_menu_open_local(cm, gtk_widget_get_width(w) / 2.0,
                            gtk_widget_get_height(w) / 2.0);
    return ctx_menu_is_mapped(cm);
}

int aeui_ctx_menu_mapped(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return 0;
    return ctx_menu_is_mapped(cm);
}

int aeui_ctx_menu_activate(int handle, int idx) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return -1;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) return -1;
    if (idx < 0 || idx >= (int)cm->closures->len) return -2;
    ctx_menu_close(cm);
    AeClosure* c = (AeClosure*)g_ptr_array_index(cm->closures, idx);
    if (c && c->fn) ((void(*)(void*))c->fn)(c->env);
    return 0;
}

static void on_ctx_menu_owner_destroy(GtkWidget* owner, gpointer data) {
    (void)owner;
    CtxMenu* cm = (CtxMenu*)data;
    ctx_menu_close(cm);
    // The popover is parented to the owner; unparent it before the owner is
    // disposed or GTK warns about a live child.
    if (cm->popover) { gtk_widget_unparent(cm->popover); cm->popover = NULL; }
}

void aether_ui_context_menu_item_impl(int handle, const char* label,
                                      void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    CtxMenu* cm = g_object_get_data(G_OBJECT(w), "aeui-ctxmenu");
    if (!cm) {
        cm = g_new0(CtxMenu, 1);
        cm->owner = w;
        cm->labels = g_ptr_array_new_with_free_func(g_free);
        cm->closures = g_ptr_array_new();
        GtkEventController* legacy = gtk_event_controller_legacy_new();
        g_signal_connect(legacy, "event",
                         G_CALLBACK(on_ctx_menu_legacy_event), cm);
        gtk_widget_add_controller(w, legacy);
        g_signal_connect(w, "destroy", G_CALLBACK(on_ctx_menu_owner_destroy), cm);
        g_object_set_data(G_OBJECT(w), "aeui-ctxmenu", cm);
    }
    g_ptr_array_add(cm->labels, g_strdup(label ? label : ""));
    g_ptr_array_add(cm->closures, boxed_closure);
}

static void on_button_clicked(GtkButton* btn, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
}

// Plain button — no callback. Use aether_ui_set_onclick to add one later.
int aether_ui_button_create_plain(const char* label) {
    ensure_gtk_init();
    GtkWidget* btn = gtk_button_new_with_label(label ? label : "");
    return aether_ui_register_widget(btn);
}

// Set click handler on an existing button (or any widget via GestureClick).
void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    if (GTK_IS_BUTTON(w)) {
        g_signal_connect(w, "clicked", G_CALLBACK(on_button_clicked), boxed_closure);
    }
}

// Styling functions that accept _ctx (void*) instead of int handle.
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aether_ui_set_bg_color((int)(intptr_t)ctx, r, g, b, a);
}

void aether_ui_set_text_color_ctx(void* ctx, double r, double g, double b) {
    aether_ui_set_text_color((int)(intptr_t)ctx, r, g, b);
}

void aether_ui_set_font_size_ctx(void* ctx, double size) {
    aether_ui_set_font_size((int)(intptr_t)ctx, size);
}

void aether_ui_set_font_bold_ctx(void* ctx, int bold) {
    aether_ui_set_font_bold((int)(intptr_t)ctx, bold);
}

void aether_ui_set_corner_radius_ctx(void* ctx, double radius) {
    aether_ui_set_corner_radius((int)(intptr_t)ctx, radius);
}

void aether_ui_set_opacity_ctx(void* ctx, double opacity) {
    aether_ui_set_opacity((int)(intptr_t)ctx, opacity);
}

void aether_ui_set_enabled_ctx(void* ctx, int enabled) {
    aether_ui_set_enabled((int)(intptr_t)ctx, enabled);
}

void aether_ui_set_tooltip_ctx(void* ctx, const char* text) {
    aether_ui_set_tooltip((int)(intptr_t)ctx, text);
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* btn = gtk_button_new_with_label(label ? label : "");
    if (boxed_closure) {
        g_signal_connect(btn, "clicked", G_CALLBACK(on_button_clicked), boxed_closure);
    }
    return aether_ui_register_widget(btn);
}

int aether_ui_vstack_create(int spacing) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    return aether_ui_register_widget(box);
}

int aether_ui_hstack_create(int spacing) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    return aether_ui_register_widget(box);
}

int aether_ui_spacer_create(void) {
    ensure_gtk_init();
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_widget_set_hexpand(spacer, TRUE);
    return aether_ui_register_widget(spacer);
}

int aether_ui_divider_create(void) {
    ensure_gtk_init();
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    return aether_ui_register_widget(sep);
}

// ---------------------------------------------------------------------------
// Input widgets (Group 2)
// ---------------------------------------------------------------------------

// Textfield — single-line text input with on_change callback.
// Callback receives the new text: ((void(*)(void*, const char*))fn)(env, text)
static void on_entry_changed(GtkEditable* editable, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        const char* text = gtk_editable_get_text(editable);
        ((void(*)(void*, const char*))c->fn)(c->env, text ? text : "");
    }
}

int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* entry = gtk_entry_new();
    if (placeholder && *placeholder) {
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    }
    if (boxed_closure) {
        g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), boxed_closure);
    }
    return aether_ui_register_widget(entry);
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_ENTRY(w)) {
        GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
        gtk_entry_buffer_set_text(buf, text ? text : "", -1);
    }
}

const char* aether_ui_textfield_get_text(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_ENTRY(w)) {
        GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
        return gtk_entry_buffer_get_text(buf);
    }
    return "";
}

// Securefield — password entry (masked input).
int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    if (placeholder && *placeholder) {
        // GtkPasswordEntry doesn't have placeholder API directly;
        // set via the internal GtkText child
        GtkWidget* text_widget = gtk_widget_get_first_child(entry);
        if (text_widget && GTK_IS_TEXT(text_widget)) {
            gtk_text_set_placeholder_text(GTK_TEXT(text_widget), placeholder);
        }
    }
    if (boxed_closure) {
        g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), boxed_closure);
    }
    return aether_ui_register_widget(entry);
}

// Toggle — checkbox with label.
// Callback receives 1 (active) or 0 (inactive).
static void on_toggle_changed(GtkCheckButton* btn, GParamSpec* pspec, gpointer data) {
    (void)pspec;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        int active = gtk_check_button_get_active(btn) ? 1 : 0;
        ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)active);
    }
}

int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* check = gtk_check_button_new_with_label(label ? label : "");
    if (boxed_closure) {
        g_signal_connect(check, "notify::active", G_CALLBACK(on_toggle_changed), boxed_closure);
    }
    return aether_ui_register_widget(check);
}

void aether_ui_toggle_set_active(int handle, int active) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_CHECK_BUTTON(w)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w), active != 0);
    }
}

int aether_ui_toggle_get_active(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_CHECK_BUTTON(w)) {
        return gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? 1 : 0;
    }
    return 0;
}

// Put a toggle in a mutual-exclusion group with another: GTK4 renders
// grouped check buttons as RADIO buttons (round indicator) and enforces
// exactly-one-active itself. Chain each new member to the group's first
// toggle. Radio groups beat a dropdown here: GtkDropDown's option list is a
// popover, which never displays under sommelier (ChromeOS).
void aether_ui_toggle_set_group(int handle, int group_with) {
    GtkWidget* w = aether_ui_get_widget(handle);
    GtkWidget* g = aether_ui_get_widget(group_with);
    if (w && g && GTK_IS_CHECK_BUTTON(w) && GTK_IS_CHECK_BUTTON(g)) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(w), GTK_CHECK_BUTTON(g));
    }
}

// Slider — horizontal scale with min/max/initial and on_change callback.
// Callback receives the new double value.
static void on_slider_changed(GtkRange* range, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        double val = gtk_range_get_value(range);
        ((void(*)(void*, double))c->fn)(c->env, val);
    }
}

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure) {
    ensure_gtk_init();
    GtkAdjustment* adj = gtk_adjustment_new(initial, min_val, max_val,
                                             1.0, 10.0, 0.0);
    GtkWidget* scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_widget_set_hexpand(scale, TRUE);
    if (boxed_closure) {
        g_signal_connect(scale, "value-changed", G_CALLBACK(on_slider_changed), boxed_closure);
    }
    return aether_ui_register_widget(scale);
}

void aether_ui_slider_set_value(int handle, double value) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_SCALE(w)) {
        gtk_range_set_value(GTK_RANGE(w), value);
    }
}

double aether_ui_slider_get_value(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_SCALE(w)) {
        return gtk_range_get_value(GTK_RANGE(w));
    }
    return 0.0;
}

// Picker — dropdown / combo box.
// Callback receives the selected index (int).
static void on_picker_changed(GtkDropDown* dropdown, GParamSpec* pspec, gpointer data) {
    (void)pspec;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        guint idx = gtk_drop_down_get_selected(dropdown);
        ((void(*)(void*, intptr_t))c->fn)(c->env, (intptr_t)idx);
    }
}

int aether_ui_picker_create(void* boxed_closure) {
    ensure_gtk_init();
    GtkStringList* model = gtk_string_list_new(NULL);
    GtkWidget* dropdown = gtk_drop_down_new(G_LIST_MODEL(model), NULL);
    if (boxed_closure) {
        g_signal_connect(dropdown, "notify::selected",
                         G_CALLBACK(on_picker_changed), boxed_closure);
    }
    return aether_ui_register_widget(dropdown);
}

void aether_ui_picker_add_item(int handle, const char* item) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_DROP_DOWN(w)) {
        GListModel* model = gtk_drop_down_get_model(GTK_DROP_DOWN(w));
        if (GTK_IS_STRING_LIST(model)) {
            gtk_string_list_append(GTK_STRING_LIST(model), item ? item : "");
        }
    }
}

void aether_ui_picker_set_selected(int handle, int index) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_DROP_DOWN(w)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w), (guint)index);
    }
}

int aether_ui_picker_get_selected(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_DROP_DOWN(w)) {
        return (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
    }
    return 0;
}

// Textarea — multi-line text input in a scrolled window.
// Callback receives the full text content.
static void on_textbuffer_changed(GtkTextBuffer* buffer, gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buffer, &start);
        gtk_text_buffer_get_end_iter(buffer, &end);
        char* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        ((void(*)(void*, const char*))c->fn)(c->env, text ? text : "");
        g_free(text);
    }
}

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    ensure_gtk_init();
    GtkWidget* textview = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(textview), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(textview), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(textview), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(textview), 4);

    // Wrap in a scrolled window
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 80);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), textview);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(scrolled, TRUE);

    if (boxed_closure) {
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview));
        g_signal_connect(buf, "changed", G_CALLBACK(on_textbuffer_changed), boxed_closure);
    }

    // Store the textview handle separately so set/get text works on it
    // Register the scrolled window as the visible widget
    int scroll_handle = aether_ui_register_widget(scrolled);
    // Also register the textview for direct text access
    aether_ui_register_widget(textview);
    // The textview handle is scroll_handle + 1
    return scroll_handle;
}

void aether_ui_textarea_set_text(int handle, const char* text) {
    // Textarea handle points to scrolled window; textview is handle+1
    GtkWidget* w = aether_ui_get_widget(handle + 1);
    if (w && GTK_IS_TEXT_VIEW(w)) {
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
        gtk_text_buffer_set_text(buf, text ? text : "", -1);
    }
}

char* aether_ui_textarea_get_text(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle + 1);
    if (w && GTK_IS_TEXT_VIEW(w)) {
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_end_iter(buf, &end);
        return gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    }
    return strdup("");
}

// Scrollview container
int aether_ui_scrollview_create(void) {
    ensure_gtk_init();
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(scrolled, TRUE);
    return aether_ui_register_widget(scrolled);
}

// Progressbar
int aether_ui_progressbar_create(double fraction) {
    ensure_gtk_init();
    GtkWidget* bar = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), fraction);
    gtk_widget_set_hexpand(bar, TRUE);
    return aether_ui_register_widget(bar);
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_PROGRESS_BAR(w)) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), fraction);
    }
}

// ---------------------------------------------------------------------------
// Layout containers (Group 3)
// ---------------------------------------------------------------------------

// ZStack — overlay container where children stack on top of each other.
int aether_ui_zstack_create(void) {
    ensure_gtk_init();
    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_widget_set_hexpand(overlay, TRUE);
    return aether_ui_register_widget(overlay);
}

// Form — vertical box with padding, suitable for form layouts.
int aether_ui_form_create(void) {
    ensure_gtk_init();
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_top(box, 20);
    gtk_widget_set_margin_bottom(box, 20);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    return aether_ui_register_widget(box);
}

// Form section — frame with title and inner content box.
int aether_ui_form_section_create(const char* title) {
    ensure_gtk_init();
    GtkWidget* frame = gtk_frame_new(title);
    GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(inner, 8);
    gtk_widget_set_margin_bottom(inner, 8);
    gtk_widget_set_margin_start(inner, 8);
    gtk_widget_set_margin_end(inner, 8);
    gtk_frame_set_child(GTK_FRAME(frame), inner);
    // Register both: frame for tree, inner for child appending
    int frame_handle = aether_ui_register_widget(frame);
    aether_ui_register_widget(inner);
    return frame_handle;
}

// NavStack — page stack with slide transitions.
static int navstack_page_counts[64] = {0};

int aether_ui_navstack_create(void) {
    ensure_gtk_init();
    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(stack), 250);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_set_hexpand(stack, TRUE);
    int handle = aether_ui_register_widget(stack);
    if (handle <= 64) navstack_page_counts[handle - 1] = 0;
    return handle;
}

void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    GtkWidget* stack = aether_ui_get_widget(handle);
    GtkWidget* body = aether_ui_get_widget(body_handle);
    if (!stack || !body || !GTK_IS_STACK(stack)) return;
    int idx = (handle <= 64) ? navstack_page_counts[handle - 1]++ : 0;
    char name[32];
    snprintf(name, sizeof(name), "page_%d", idx);
    gtk_stack_add_named(GTK_STACK(stack), body, name);
    gtk_stack_set_visible_child(GTK_STACK(stack), body);
    (void)title;
}

void aether_ui_navstack_pop(int handle) {
    GtkWidget* stack = aether_ui_get_widget(handle);
    if (!stack || !GTK_IS_STACK(stack)) return;
    if (handle > 64) return;
    int count = navstack_page_counts[handle - 1];
    if (count <= 1) return;
    char name[32];
    snprintf(name, sizeof(name), "page_%d", count - 1);
    GtkWidget* page = gtk_stack_get_child_by_name(GTK_STACK(stack), name);
    if (page) {
        // Show previous page
        char prev_name[32];
        snprintf(prev_name, sizeof(prev_name), "page_%d", count - 2);
        GtkWidget* prev = gtk_stack_get_child_by_name(GTK_STACK(stack), prev_name);
        if (prev) gtk_stack_set_visible_child(GTK_STACK(stack), prev);
        gtk_stack_remove(GTK_STACK(stack), page);
        navstack_page_counts[handle - 1]--;
    }
}

// ---------------------------------------------------------------------------
// Styling (Group 4)
// ---------------------------------------------------------------------------

// Apply CSS scoped to a single widget via a unique class name.
// Each widget gets "aui-{handle}" as a CSS class, and the CSS rule
// targets that class specifically — no global side effects.
// Single global CSS provider — accumulates all styling rules.
static char* global_css_buf = NULL;
static int global_css_len = 0;
static int global_css_cap = 0;
static GtkCssProvider* global_css_provider = NULL;

static void global_css_append(const char* rule) {
    int rlen = (int)strlen(rule);
    if (global_css_len + rlen + 2 > global_css_cap) {
        global_css_cap = (global_css_cap == 0) ? 4096 : global_css_cap * 2;
        global_css_buf = realloc(global_css_buf, global_css_cap);
    }
    memcpy(global_css_buf + global_css_len, rule, rlen);
    global_css_len += rlen;
    global_css_buf[global_css_len++] = '\n';
    global_css_buf[global_css_len] = '\0';
}

static void global_css_reload(void) {
    if (!global_css_buf) return;
    ensure_gtk_init();
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    if (!global_css_provider) {
        global_css_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(global_css_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER);
    }
    gtk_css_provider_load_from_data(global_css_provider, global_css_buf, -1);
}

static void aether_ui_apply_css(int handle, GtkWidget* widget, const char* property_css) {
    char class_name[32];
    snprintf(class_name, sizeof(class_name), "aui-%d", handle);
    gtk_widget_add_css_class(widget, class_name);

    char css[512];
    snprintf(css, sizeof(css), ".%s { %s }", class_name, property_css);
    global_css_append(css);
    global_css_reload();
}

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;

    char cls[32];
    snprintf(cls, sizeof(cls), "aui-bg-%d", handle);
    gtk_widget_add_css_class(w, cls);
    if (GTK_IS_BUTTON(w)) gtk_widget_add_css_class(w, "flat");

    char css[512];
    int ri = (int)(r * 255), gi = (int)(g * 255), bi = (int)(b * 255);
    if (GTK_IS_BUTTON(w)) {
        snprintf(css, sizeof(css),
            "button.flat.%s { background-color: rgb(%d,%d,%d); background-image: none; }\n"
            "button.flat.%s:hover { background-color: rgb(%d,%d,%d); background-image: none; }",
            cls, ri, gi, bi, cls, ri, gi, bi);
    } else {
        snprintf(css, sizeof(css),
            ".%s { background: rgba(%d,%d,%d,%.2f); }",
            cls, ri, gi, bi, a);
    }
    global_css_append(css);
    global_css_reload();
}

void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2,
                               int vertical) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    const char* dir = vertical ? "to bottom" : "to right";
    char prop[256];
    snprintf(prop, sizeof(prop),
        "background: linear-gradient(%s, rgb(%d,%d,%d), rgb(%d,%d,%d));",
        dir,
        (int)(r1 * 255), (int)(g1 * 255), (int)(b1 * 255),
        (int)(r2 * 255), (int)(g2 * 255), (int)(b2 * 255));
    aether_ui_apply_css(handle, w, prop);
}

void aether_ui_set_text_color(int handle, double r, double g, double b) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    if (GTK_IS_LABEL(w)) {
        PangoAttrList* attrs = pango_attr_list_new();
        PangoAttribute* attr = pango_attr_foreground_new(
            (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535));
        pango_attr_list_insert(attrs, attr);
        gtk_label_set_attributes(GTK_LABEL(w), attrs);
        pango_attr_list_unref(attrs);
    } else {
        char prop[128];
        snprintf(prop, sizeof(prop), "color: rgb(%d,%d,%d);",
            (int)(r * 255), (int)(g * 255), (int)(b * 255));
        aether_ui_apply_css(handle, w, prop);
    }
}

void aether_ui_set_font_size(int handle, double size) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return;
    PangoAttrList* attrs = gtk_label_get_attributes(GTK_LABEL(w));
    if (!attrs) attrs = pango_attr_list_new();
    else attrs = pango_attr_list_copy(attrs);
    PangoAttribute* attr = pango_attr_size_new((int)(size * PANGO_SCALE));
    pango_attr_list_insert(attrs, attr);
    gtk_label_set_attributes(GTK_LABEL(w), attrs);
    pango_attr_list_unref(attrs);
}

void aether_ui_set_font_bold(int handle, int bold) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_LABEL(w)) return;
    PangoAttrList* attrs = gtk_label_get_attributes(GTK_LABEL(w));
    if (!attrs) attrs = pango_attr_list_new();
    else attrs = pango_attr_list_copy(attrs);
    PangoAttribute* attr = pango_attr_weight_new(
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_attr_list_insert(attrs, attr);
    gtk_label_set_attributes(GTK_LABEL(w), attrs);
    pango_attr_list_unref(attrs);
}

void aether_ui_set_corner_radius(int handle, double radius) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char prop[64];
    snprintf(prop, sizeof(prop), "border-radius: %dpx;", (int)radius);
    aether_ui_apply_css(handle, w, prop);
}

void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char prop[128];
    snprintf(prop, sizeof(prop), "padding: %dpx %dpx %dpx %dpx;",
        (int)top, (int)right, (int)bottom, (int)left);
    aether_ui_apply_css(handle, w, prop);
}

void aether_ui_set_width(int handle, int width) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_size_request(w, width, -1);
}

void aether_ui_set_height(int handle, int height) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    char prop[128];
    snprintf(prop, sizeof(prop), "min-height: %dpx; max-height: %dpx;", height, height);
    aether_ui_apply_css(handle, w, prop);
    gtk_widget_set_vexpand(w, FALSE);
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
}

void aether_ui_set_opacity(int handle, double opacity) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_opacity(w, opacity);
}

void aether_ui_set_enabled(int handle, int enabled) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_sensitive(w, enabled != 0);
}

void aether_ui_set_tooltip(int handle, const char* text) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_tooltip_text(w, text);
}

void aether_ui_set_distribution(int handle, int distribution) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_BOX(w)) {
        gtk_box_set_homogeneous(GTK_BOX(w), distribution == 1);
    }
}

void aether_ui_set_alignment(int handle, int alignment) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !GTK_IS_BOX(w)) return;
    int is_vertical = (gtk_orientable_get_orientation(GTK_ORIENTABLE(w))
                       == GTK_ORIENTATION_VERTICAL);
    GtkAlign align;
    if (is_vertical) {
        switch (alignment) {
            case 5: align = GTK_ALIGN_START; break;   // Leading
            case 9: align = GTK_ALIGN_CENTER; break;  // CenterX
            case 7: align = GTK_ALIGN_FILL; break;    // Fill
            default: align = GTK_ALIGN_FILL; break;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c;
             c = gtk_widget_get_next_sibling(c)) {
            gtk_widget_set_halign(c, align);
        }
    } else {
        switch (alignment) {
            case 3: align = GTK_ALIGN_START; break;   // Top
            case 12: align = GTK_ALIGN_CENTER; break; // CenterY
            case 4: align = GTK_ALIGN_END; break;     // Bottom
            default: align = GTK_ALIGN_FILL; break;
        }
        for (GtkWidget* c = gtk_widget_get_first_child(w); c;
             c = gtk_widget_get_next_sibling(c)) {
            gtk_widget_set_valign(c, align);
        }
    }
}

void aether_ui_match_parent_width(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) {
        gtk_widget_set_hexpand(w, TRUE);
        gtk_widget_set_halign(w, GTK_ALIGN_FILL);
    }
}

void aether_ui_match_parent_height(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) {
        gtk_widget_set_vexpand(w, TRUE);
        gtk_widget_set_valign(w, GTK_ALIGN_FILL);
    }
}

void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    gtk_widget_set_margin_top(w, top);
    gtk_widget_set_margin_end(w, right);
    gtk_widget_set_margin_bottom(w, bottom);
    gtk_widget_set_margin_start(w, left);
}

// _ctx variant: the ambient builder-context handle arrives as a void*.
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aether_ui_set_margin((int)(intptr_t)ctx, top, right, bottom, left);
}

// Enable the AetherUIDriver test server against the ambient root (_ctx).
void aether_ui_enable_test_server_ctx(int port, void* ctx) {
    aether_ui_enable_test_server_impl(port, (int)(intptr_t)ctx);
}

// ---------------------------------------------------------------------------
// System integration (Group 5)
// ---------------------------------------------------------------------------

// Alert dialog — modal message box with OK button.
void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) return;  // modal dialog would block on CI
    ensure_gtk_init();
    GtkWidget* dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", message ? message : "");
    if (title) gtk_window_set_title(GTK_WINDOW(dialog), title);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_window_destroy), NULL);
    gtk_widget_show(dialog);
}

// File open dialog — blocks and returns the selected path, or "" if cancelled.
char* aether_ui_file_open(const char* title) {
    ensure_gtk_init();
    // GtkFileChooserNative is the GTK 4.8-compatible way to do this.
    // However, it is still async-oriented. For now, we stub it as requested.
    fprintf(stderr, "Warning: aether_ui_file_open is not yet implemented for GTK 4.8\n");
    return strdup("");
}

// Clipboard read/write
void aether_ui_clipboard_write_impl(const char* text) {
    ensure_gtk_init();
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    GdkClipboard* clipboard = gdk_display_get_clipboard(display);
    gdk_clipboard_set_text(clipboard, text ? text : "");
}

// Timer — schedule a repeating callback at interval_ms.
static gboolean on_timer_tick(gpointer data) {
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
    return G_SOURCE_CONTINUE;
}

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (!boxed_closure || interval_ms <= 0) return 0;
    guint id = g_timeout_add((guint)interval_ms, on_timer_tick, boxed_closure);
    return (int)id;
}

void aether_ui_timer_cancel_impl(int timer_id) {
    if (timer_id > 0) g_source_remove((guint)timer_id);
}

// Open URL in default browser
void aether_ui_open_url_impl(const char* url) {
    if (!url) return;
    ensure_gtk_init();
    gtk_show_uri(NULL, url, GDK_CURRENT_TIME);
}

// Dark mode detection
int aether_ui_dark_mode_check(void) {
    ensure_gtk_init();
    GtkSettings* settings = gtk_settings_get_default();
    if (!settings) return 0;
    gboolean dark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &dark, NULL);
    if (dark) return 1;
    // Also check theme name
    char* theme = NULL;
    g_object_get(settings, "gtk-theme-name", &theme, NULL);
    if (theme) {
        int is_dark = (strstr(theme, "dark") != NULL || strstr(theme, "Dark") != NULL);
        g_free(theme);
        if (is_dark) return 1;
    }
    return 0;
}

// Multi-window support
typedef struct {
    GtkWindow* window;
    int root_handle;
} WindowEntry;

static WindowEntry* extra_windows = NULL;
static int extra_window_count = 0;
static int extra_window_capacity = 0;

int aether_ui_window_create_impl(const char* title, int width, int height) {
    ensure_gtk_init();
    if (extra_window_count >= extra_window_capacity) {
        extra_window_capacity = extra_window_capacity == 0 ? 8 : extra_window_capacity * 2;
        extra_windows = realloc(extra_windows, sizeof(WindowEntry) * extra_window_capacity);
    }
    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title ? title : "");
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
    WindowEntry* e = &extra_windows[extra_window_count++];
    e->window = GTK_WINDOW(win);
    e->root_handle = 0;
    return extra_window_count; // 1-based
}

void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    if (win_handle < 1 || win_handle > extra_window_count) return;
    WindowEntry* e = &extra_windows[win_handle - 1];
    e->root_handle = root_handle;
    GtkWidget* root = aether_ui_get_widget(root_handle);
    if (root) gtk_window_set_child(e->window, root);
}

void aether_ui_window_show_impl(int win_handle) {
    if (win_handle < 1 || win_handle > extra_window_count) return;
    gtk_window_present(extra_windows[win_handle - 1].window);
}

void aether_ui_window_close_impl(int win_handle) {
    if (win_handle < 1 || win_handle > extra_window_count) return;
    gtk_window_close(extra_windows[win_handle - 1].window);
}

// Sheet (modal dialog window)
int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    ensure_gtk_init();
    GtkWidget* win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title ? title : "");
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
    return aether_ui_register_widget(win);
}

void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    GtkWidget* win = aether_ui_get_widget(handle);
    GtkWidget* root = aether_ui_get_widget(root_handle);
    if (win && root && GTK_IS_WINDOW(win)) {
        gtk_window_set_child(GTK_WINDOW(win), root);
    }
}

void aether_ui_sheet_present_impl(int handle) {
    GtkWidget* win = aether_ui_get_widget(handle);
    if (win && GTK_IS_WINDOW(win)) {
        gtk_window_present(GTK_WINDOW(win));
    }
}

void aether_ui_sheet_dismiss_impl(int handle) {
    GtkWidget* win = aether_ui_get_widget(handle);
    if (win && GTK_IS_WINDOW(win)) {
        gtk_window_close(GTK_WINDOW(win));
    }
}

// Image widget
int aether_ui_image_create(const char* filepath) {
    ensure_gtk_init();
    GtkWidget* img;
    if (filepath && *filepath) {
        img = gtk_image_new_from_file(filepath);
    } else {
        img = gtk_image_new();
    }
    return aether_ui_register_widget(img);
}

void aether_ui_image_set_size(int handle, int width, int height) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w && GTK_IS_IMAGE(w)) {
        gtk_image_set_pixel_size(GTK_IMAGE(w), width > height ? width : height);
        gtk_widget_set_size_request(w, width, height);
    }
}

// ---------------------------------------------------------------------------
// Canvas drawing + Events + Animation (Group 6)
// ---------------------------------------------------------------------------

// Canvas — command-buffer drawing via Cairo on a GtkDrawingArea.

typedef enum {
    CANVAS_BEGIN_PATH,
    CANVAS_MOVE_TO,
    CANVAS_LINE_TO,
    CANVAS_STROKE,
    CANVAS_FILL_RECT,
    CANVAS_CLEAR,
    CANVAS_ARC,         // add a circle/arc to the current path
    CANVAS_CLOSE_PATH,  // close the current sub-path
    CANVAS_FILL,        // fill the current path with a color
    CANVAS_FILL_TEXT,   // draw text at (x, y), size in w
    CANVAS_DRAW_IMAGE,  // blit an RGBA buffer at (x, y), size w×h
    CANVAS_FILL_LINEAR, // fill current path with a linear gradient
    CANVAS_FILL_RADIAL, // fill current path with a radial gradient
    CANVAS_CLIP_RECT    // intersect the clip region with rect (x,y,w,h)
} CanvasCmdType;

typedef struct {
    CanvasCmdType type;
    double x, y;              // MOVE_TO, LINE_TO, ARC center, FILL_TEXT origin,
                              //   DRAW_IMAGE top-left
    double r, g, b, a;        // STROKE / FILL color
    double w, h;              // FILL_RECT w/h; STROKE line_width (x); ARC radius (w);
                              //   ARC start angle (h); FILL_TEXT font size (w);
                              //   DRAW_IMAGE pixel dims
    double a0, a1;            // ARC start/end angle (radians)
    char* text;              // FILL_TEXT string (owned, freed on clear/destroy)
    unsigned char* pixels;   // DRAW_IMAGE RGBA8888 buffer (owned), iw*ih*4 bytes
    int iw, ih;              // DRAW_IMAGE pixel width/height
    // Gradient (FILL_LINEAR / FILL_RADIAL): geometry in x,y..(see impls),
    // plus owned stop arrays. FILL_LINEAR uses (gx1,gy1)→(gx2,gy2);
    // FILL_RADIAL uses center (gx1,gy1) radius gr + focal (gfx,gfy).
    // grad_line_width: 0 → fill the path; >0 → stroke it at that width.
    double gx1, gy1, gx2, gy2, gr, gfx, gfy, grad_line_width;
    int n_stops;
    int grad_extend;         // 0=pad, 1=reflect, 2=repeat (SVG spreadMethod)
    double* stop_off;        // owned: n_stops offsets (0..1)
    double* stop_rgba;       // owned: n_stops*4 colour comps (0..1)
} CanvasCmd;

typedef struct {
    CanvasCmd* cmds;
    int count;
    int capacity;
    int widget_handle;
    // Resize hook — the AeVG vg{} scope registers a closure here so it can
    // re-map its viewBox and re-flush its shapes at the new canvas size. Fired
    // from the draw func when the allocation differs from last_w/last_h.
    AeClosure* on_resize;
    int last_w;
    int last_h;
    // Click hook — AeVG's vg{} scope registers a closure here to receive
    // canvas-local (x, y) on a single click, so it can hit-test shapes and
    // dispatch per-element handlers (the widget-level on_click gives no coords).
    AeClosure* on_click;
    // Pointer-move hook — receives canvas-local (x, y) on every motion event,
    // for hover hit-testing (e.g. a treemap status line tracking the tile under
    // the cursor). Widget enter/leave (on_hover_enter/leave) gives no coords.
    AeClosure* on_move;
    // Key-press hook — receives a key NAME string (e.g. "Left", "a", "space",
    // "Escape") on every key-down while the canvas has focus. For games /
    // keyboard-driven canvases. The scene's event model already has the
    // dispatch side (grammar_events.dispatch_key_down); this bridges real keys.
    AeClosure* on_key;
    // Pointer-release hook — canvas-local (x, y) where the button came up.
    // Pairs with on_click + on_move to form a press→drag→release swipe.
    AeClosure* on_release;
} CanvasState;

static CanvasState* canvas_states = NULL;
static int canvas_state_count = 0;
static int canvas_state_capacity = 0;

static CanvasState* get_canvas_state(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_state_count) return NULL;
    return &canvas_states[canvas_id - 1];
}

static void canvas_add_cmd(int canvas_id, CanvasCmd cmd) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    if (cs->count >= cs->capacity) {
        cs->capacity = cs->capacity == 0 ? 64 : cs->capacity * 2;
        cs->cmds = realloc(cs->cmds, sizeof(CanvasCmd) * cs->capacity);
    }
    cs->cmds[cs->count++] = cmd;
}

// Replay a canvas command buffer onto any cairo context. Shared by the live
// GtkDrawingArea draw func and the off-screen PNG renderer (canvas_write_png),
// so on-screen and headless output are byte-identical.
static void canvas_replay(cairo_t* cr, CanvasState* cs) {
    if (!cs) return;
    for (int i = 0; i < cs->count; i++) {
        CanvasCmd* c = &cs->cmds[i];
        switch (c->type) {
            case CANVAS_BEGIN_PATH:
                cairo_new_path(cr);
                break;
            case CANVAS_MOVE_TO:
                cairo_move_to(cr, c->x, c->y);
                break;
            case CANVAS_LINE_TO:
                cairo_line_to(cr, c->x, c->y);
                break;
            case CANVAS_STROKE:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_set_line_width(cr, c->x); // line_width stored in x
                // cap/join from iw/ih (SVG defaults butt/miter).
                if (c->iw == 1)      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                else if (c->iw == 2) cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
                else                 cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
                if (c->ih == 1)      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                else if (c->ih == 2) cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
                else                 cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
                cairo_stroke(cr);
                break;
            case CANVAS_FILL_RECT:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_rectangle(cr, c->x, c->y, c->w, c->h);
                cairo_fill(cr);
                break;
            case CANVAS_CLIP_RECT:
                // Intersect the clip region with (x,y,w,h). Persists for the
                // rest of this replay (a fresh cairo_t per frame resets it), so
                // the AeVG scene emits one at the start to enforce the SVG
                // viewport's overflow:hidden — shapes outside the viewBox are
                // cut at its edge.
                cairo_rectangle(cr, c->x, c->y, c->w, c->h);
                cairo_clip(cr);
                cairo_new_path(cr);  // clear the path the rectangle added
                break;
            case CANVAS_ARC:
                // Append an arc to the current path. w = radius,
                // a0/a1 = start/end angle (radians). For a full
                // circle the caller passes 0..2π.
                cairo_arc(cr, c->x, c->y, c->w, c->a0, c->a1);
                break;
            case CANVAS_CLOSE_PATH:
                cairo_close_path(cr);
                break;
            case CANVAS_FILL:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_fill(cr);
                break;
            case CANVAS_FILL_TEXT:
                cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
                cairo_set_font_size(cr, c->w);
                cairo_move_to(cr, c->x, c->y);
                if (c->text) cairo_show_text(cr, c->text);
                cairo_new_path(cr); // show_text leaves a path; clear it
                break;
            case CANVAS_DRAW_IMAGE:
                if (c->pixels && c->iw > 0 && c->ih > 0) {
                    // Incoming buffer is RGBA8888 (R,G,B,A bytes).
                    // Cairo ARGB32 is native-endian premultiplied — on
                    // little-endian that's B,G,R,A byte order with
                    // premultiplied colour. Build a converted buffer.
                    int n = c->iw * c->ih;
                    unsigned char* conv = (unsigned char*)malloc(n * 4);
                    if (conv) {
                        for (int px = 0; px < n; px++) {
                            unsigned char sr = c->pixels[px*4+0];
                            unsigned char sg = c->pixels[px*4+1];
                            unsigned char sb = c->pixels[px*4+2];
                            unsigned char sa = c->pixels[px*4+3];
                            // premultiply
                            conv[px*4+0] = (unsigned char)(sb * sa / 255); // B
                            conv[px*4+1] = (unsigned char)(sg * sa / 255); // G
                            conv[px*4+2] = (unsigned char)(sr * sa / 255); // R
                            conv[px*4+3] = sa;                              // A
                        }
                        int stride = cairo_format_stride_for_width(
                            CAIRO_FORMAT_ARGB32, c->iw);
                        cairo_surface_t* surf = cairo_image_surface_create_for_data(
                            conv, CAIRO_FORMAT_ARGB32, c->iw, c->ih, stride);
                        if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                            // c->w / c->h, when > 0, are the DESTINATION extent
                            // (canvas px). If they differ from the source pixel
                            // dims (iw/ih), scale the source surface to fit —
                            // e.g. a 640x480 video frame into a smaller region.
                            // w/h <= 0 means blit 1:1 at native size.
                            int have_dest = (c->w > 0.0 && c->h > 0.0);
                            cairo_save(cr);
                            if (have_dest) {
                                cairo_translate(cr, c->x, c->y);
                                cairo_scale(cr, c->w / (double)c->iw,
                                                c->h / (double)c->ih);
                                cairo_set_source_surface(cr, surf, 0, 0);
                            } else {
                                cairo_set_source_surface(cr, surf, c->x, c->y);
                            }
                            // Nearest/good filtering for scaled video frames.
                            cairo_pattern_set_filter(cairo_get_source(cr),
                                                     CAIRO_FILTER_GOOD);
                            cairo_paint(cr);
                            cairo_restore(cr);
                            cairo_set_source_rgba(cr, 0, 0, 0, 1); // reset source
                        }
                        cairo_surface_destroy(surf);
                        free(conv);
                    }
                }
                break;
            case CANVAS_FILL_LINEAR:
            case CANVAS_FILL_RADIAL: {
                cairo_pattern_t* pat;
                if (c->type == CANVAS_FILL_LINEAR) {
                    pat = cairo_pattern_create_linear(c->gx1, c->gy1, c->gx2, c->gy2);
                } else {
                    // Radial: inner circle at the focal point (radius 0),
                    // outer circle at the center with radius gr.
                    pat = cairo_pattern_create_radial(c->gfx, c->gfy, 0.0,
                                                      c->gx1, c->gy1, c->gr);
                }
                for (int si = 0; si < c->n_stops; si++) {
                    cairo_pattern_add_color_stop_rgba(pat, c->stop_off[si],
                        c->stop_rgba[si*4+0], c->stop_rgba[si*4+1],
                        c->stop_rgba[si*4+2], c->stop_rgba[si*4+3]);
                }
                // SVG spreadMethod → cairo extend (pad is the cairo default).
                if (c->grad_extend == 1) {
                    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REFLECT);
                } else if (c->grad_extend == 2) {
                    cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
                } else {
                    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
                }
                cairo_set_source(cr, pat);
                if (c->grad_line_width > 0) {
                    cairo_set_line_width(cr, c->grad_line_width);
                    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                    cairo_stroke(cr);
                } else {
                    cairo_fill(cr);
                }
                cairo_pattern_destroy(pat);
                cairo_set_source_rgba(cr, 0, 0, 0, 1); // reset source
                break;
            }
            case CANVAS_CLEAR:
                break;
        }
    }
}

static void canvas_draw_func(GtkDrawingArea* area, cairo_t* cr,
                              int width, int height, gpointer data) {
    (void)area;
    int canvas_id = (int)(intptr_t)data;
    CanvasState* cs = get_canvas_state(canvas_id);
    // Resize: GTK re-invokes the draw func with the new allocation. If a
    // scene registered a resize hook and the size changed, fire it so it can
    // re-map + re-flush its shapes at the new scale BEFORE we replay. The hook
    // receives (w, h) and must rebuild the command buffer (clear + re-dispatch);
    // it must NOT queue another draw (we're already drawing).
    if (cs && cs->on_resize && cs->on_resize->fn &&
        (width != cs->last_w || height != cs->last_h)) {
        cs->last_w = width;
        cs->last_h = height;
        ((void(*)(void*, intptr_t, intptr_t))cs->on_resize->fn)(
            cs->on_resize->env, (intptr_t)width, (intptr_t)height);
    }
    canvas_replay(cr, cs);
}

// Render the canvas command buffer to a PNG file off-screen — works HEADLESS
// (no window, pure cairo image surface). The driver's screenshot endpoint and
// the AeVG parity harness both use this; it's byte-identical to the on-screen
// render because both go through canvas_replay. Returns 1 on success, 0 on
// failure. Background is transparent (ARGB32) unless the buffer fills it.
int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !path || width <= 0 || height <= 0) return 0;
    cairo_surface_t* surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }
    cairo_t* cr = cairo_create(surf);
    canvas_replay(cr, cs);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    return st == CAIRO_STATUS_SUCCESS ? 1 : 0;
}

int aether_ui_canvas_create_impl(int width, int height) {
    ensure_gtk_init();
    GtkWidget* da = gtk_drawing_area_new();
    // Focusable so it can receive key events (canvas_on_key); a GtkDrawingArea
    // isn't focusable by default. Harmless for canvases that never take keys.
    gtk_widget_set_focusable(da, TRUE);
    // content_width/height is the canvas's NATURAL (minimum) size — used when
    // no parent forces a size (headless PNG, canvas in a scroll view). But a
    // GtkDrawingArea won't grow past its content size on its own, so a canvas
    // in a window would stay 400x300 and just center. Make it expand + fill so
    // it takes the whole allocation; the draw func then gets the real size and
    // the resize hook fires, letting the vg scene rescale.
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), width);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), height);
    gtk_widget_set_hexpand(da, TRUE);
    gtk_widget_set_vexpand(da, TRUE);
    gtk_widget_set_halign(da, GTK_ALIGN_FILL);
    gtk_widget_set_valign(da, GTK_ALIGN_FILL);

    if (canvas_state_count >= canvas_state_capacity) {
        canvas_state_capacity = canvas_state_capacity == 0 ? 16 : canvas_state_capacity * 2;
        canvas_states = realloc(canvas_states, sizeof(CanvasState) * canvas_state_capacity);
    }
    CanvasState* cs = &canvas_states[canvas_state_count];
    cs->cmds = NULL;
    cs->count = 0;
    cs->capacity = 0;
    cs->on_resize = NULL;
    cs->on_move = NULL;
    cs->on_key = NULL;
    cs->on_release = NULL;
    cs->on_click = NULL;
    cs->last_w = width;
    cs->last_h = height;
    canvas_state_count++;
    int canvas_id = canvas_state_count; // 1-based

    cs->widget_handle = aether_ui_register_widget(da);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), canvas_draw_func,
                                    (gpointer)(intptr_t)canvas_id, NULL);
    return canvas_id;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    return cs ? cs->widget_handle : 0;
}

// Register a resize hook on a canvas. The boxed Aether closure takes (w, h)
// and is fired from the draw func when the allocation changes (see
// canvas_draw_func). AeVG's vg{} scope uses this to rescale its scene.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs) return;
    cs->on_resize = (AeClosure*)boxed_closure;
    // Seed last_w/h to the widget's content size so the first real allocation
    // that differs triggers a remap. (0 would fire spuriously on first draw.)
    cs->last_w = -1;
    cs->last_h = -1;
}

// Canvas click gesture — forwards the canvas-LOCAL (x, y) of a single click to
// the registered closure. Unlike on_double_click/on_button_clicked (which drop
// the coords), AeVG needs them to hit-test which shape was clicked. The
// "pressed" signal of a GtkGestureClick reports coordinates relative to the
// widget the controller is attached to (the drawing area), which is the same
// space AeVG stores shape bounds in (post-flush canvas px).
// Canvas button events use a LEGACY event controller, NOT GtkGestureClick.
// sommelier (ChromeOS Crostini) delivers touchpad taps as button press/
// release pairs that GtkGestureClick silently drops — first seen with
// button-3 in the context-menu saga, and equally true for BUTTON-1 taps on
// the treemap canvas (raw events reached the widget, "pressed" never fired,
// so clicks did nothing on the Chromebook). The legacy controller sees every
// event on every backend; filter by type + button ourselves.
//
// Translate an event's surface position into WIDGET-local coordinates
// (gdk_event_get_position is surface-relative; hit-testing needs canvas px).
static void event_widget_coords(GtkWidget* w, GdkEvent* ev,
                                 double* wx, double* wy) {
    double sx = 0, sy = 0;
    gdk_event_get_position(ev, &sx, &sy);
    GtkNative* native = gtk_widget_get_native(w);
    if (native) {
        double nx = 0, ny = 0;
        gtk_native_get_surface_transform(native, &nx, &ny);
        graphene_point_t p = GRAPHENE_POINT_INIT((float)(sx - nx), (float)(sy - ny));
        graphene_point_t out;
        if (gtk_widget_compute_point(GTK_WIDGET(native), w, &p, &out)) {
            *wx = out.x;
            *wy = out.y;
            return;
        }
    }
    *wx = sx;
    *wy = sy;
}

// One hook instance per registration: which widget (for coords + focus),
// which closure, and which event type it fires on.
typedef struct {
    GtkWidget* widget;
    AeClosure* closure;
    GdkEventType fires_on;   // GDK_BUTTON_PRESS (click) or _RELEASE (release)
} CanvasButtonHook;

static gboolean on_canvas_button_legacy(GtkEventControllerLegacy* c,
                                         GdkEvent* ev, gpointer data) {
    (void)c;
    CanvasButtonHook* hook = (CanvasButtonHook*)data;
    GdkEventType t = gdk_event_get_event_type(ev);
    if (t != hook->fires_on) return FALSE;
    if (gdk_button_event_get_button(ev) != 1) return FALSE;
    if (aeui_ctx_debug()) {
        GdkDevice* dev = gdk_event_get_device(ev);
        fprintf(stderr, "aeui: canvas btn1 %s dev=%s src=%d time=%u\n",
                t == GDK_BUTTON_PRESS ? "press" : "release",
                dev ? gdk_device_get_name(dev) : "(null)",
                dev ? (int)gdk_device_get_source(dev) : -1,
                gdk_event_get_time(ev));
    }
    double x = 0, y = 0;
    event_widget_coords(hook->widget, ev, &x, &y);
    if (aeui_ctx_debug())
        fprintf(stderr, "aeui: canvas %s px=(%.1f, %.1f)\n",
                t == GDK_BUTTON_PRESS ? "press" : "release", x, y);
    // Keep keyboard input flowing back to the canvas after a toolbar button
    // stole focus (the gesture-based focus-click never fires on sommelier).
    if (t == GDK_BUTTON_PRESS) gtk_widget_grab_focus(hook->widget);
    AeClosure* cl = hook->closure;
    if (cl && cl->fn) {
        ((void(*)(void*, double, double))cl->fn)(cl->env, x, y);
    }
    return FALSE;   // propagate — the ctx-menu legacy controller needs it too
}

static void canvas_add_button_hook(GtkWidget* w, AeClosure* closure,
                                    GdkEventType fires_on) {
    CanvasButtonHook* hook = g_new0(CanvasButtonHook, 1);
    hook->widget = w;
    hook->closure = closure;
    hook->fires_on = fires_on;
    GtkEventController* legacy = gtk_event_controller_legacy_new();
    g_signal_connect_data(legacy, "event",
                          G_CALLBACK(on_canvas_button_legacy), hook,
                          (GClosureNotify)g_free, 0);
    gtk_widget_add_controller(w, legacy);
}

// Register a single-click hook on a canvas. The boxed Aether closure takes
// (x: float, y: float) in canvas-local pixels. AeVG's vg{} scope uses this to
// dispatch_click into its scene, hit-testing tracked shape elements.
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_click = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    canvas_add_button_hook(w, cs->on_click, GDK_BUTTON_PRESS);
}

// Register a pointer-release hook on a canvas. Closure takes (x, y) in
// canvas-local px (the point where the button was released). With on_click +
// on_move this completes a press→drag→release gesture so callers can compute
// a swipe (drag delta) without per-element drag tracking. Built for the cube.
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_release = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    canvas_add_button_hook(w, cs->on_release, GDK_BUTTON_RELEASE);
}

// Pointer-move on the canvas: forward canvas-local (x, y) to the boxed closure.
// The GtkEventControllerMotion "motion" signal already reports widget-relative
// coords (unlike the enter/leave handlers, which discard them).
//
// Suppressed while the AetherUIDriver is armed: GTK re-synthesizes a motion
// at the parked pointer after every relayout, and under Xvfb that phantom
// hover clobbers state (status text) between a test's action and its
// assertion. Automation injects pointer-moves via POST /canvas/{id}/move,
// which calls the closure directly and stays deterministic.
static int aeui_remote_controlled(void);
static void on_canvas_move(GtkEventControllerMotion* ctrl, double x, double y,
                            gpointer data) {
    (void)ctrl;
    if (aeui_remote_controlled()) return;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*, double, double))c->fn)(c->env, x, y);
    }
}

// Register a pointer-move hook on a canvas. The boxed Aether closure takes
// (x: float, y: float) in canvas-local pixels — fired on every motion event.
// Used for hover hit-testing (e.g. a status line tracking the item under the
// cursor) where the widget-level enter/leave hooks are too coarse.
void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_move = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_canvas_move), boxed_closure);
    gtk_widget_add_controller(w, motion);
}

// Key-press on the canvas: forward the key NAME (gdk_keyval_name, e.g. "Left",
// "a", "space", "Escape") to the boxed closure. The widget must have focus —
// the canvas is set focusable at creation, and on_key registration grabs it.
static gboolean on_canvas_key(GtkEventControllerKey* ctrl, guint keyval,
                               guint keycode, GdkModifierType state, gpointer data) {
    (void)ctrl; (void)keycode; (void)state;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        const char* name = gdk_keyval_name(keyval);
        ((void(*)(void*, const char*))c->fn)(c->env, name ? name : "");
    }
    return TRUE;   // handled
}

// Grab keyboard focus once the widget is actually on screen. Calling
// grab_focus at registration is too early — the window isn't mapped yet, so
// the grab doesn't stick and keys go nowhere. Defer it to the "map" signal.
static void on_canvas_map_focus(GtkWidget* w, gpointer data) {
    (void)data;
    gtk_widget_grab_focus(w);
}

// A click anywhere on the canvas returns keyboard focus to it (clicking a
// button steals focus; this gets it back so keys keep working). Runs on the
// CAPTURE phase so it fires even if another gesture also handles the press.
static void on_canvas_focus_click(GtkGestureClick* g, int n, double x, double y,
                                   gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    gtk_widget_grab_focus(GTK_WIDGET(data));
}

// Register a key-down hook on a canvas. The boxed Aether closure takes
// (key: string). Bridges real GTK key events into the scene's keyboard model
// (which already has grammar_events.dispatch_key_down on the Aether side).
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (!cs || !boxed_closure) return;
    cs->on_key = (AeClosure*)boxed_closure;
    GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
    if (!w) return;
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_canvas_key), boxed_closure);
    gtk_widget_add_controller(w, keys);

    // Take focus when mapped (grab here is too early — window not shown yet).
    g_signal_connect(w, "map", G_CALLBACK(on_canvas_map_focus), NULL);
    if (gtk_widget_get_mapped(w)) gtk_widget_grab_focus(w);

    // Re-focus on click so the canvas keeps keyboard input after a button steals
    // focus. Capture phase so it co-exists with the coords click gesture.
    GtkGesture* fclick = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(fclick),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(fclick, "pressed", G_CALLBACK(on_canvas_focus_click), w);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(fclick));
}

void aether_ui_canvas_begin_path_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_BEGIN_PATH });
}

void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_MOVE_TO, .x = x, .y = y });
}

void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_LINE_TO, .x = x, .y = y });
}

// cap: 0=butt (SVG default), 1=round, 2=square. join: 0=miter (default),
// 1=round, 2=bevel. Stored in the unused iw/ih int fields.
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b,
                             double a, double line_width, int cap, int join) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_STROKE, .r = r, .g = g, .b = b, .a = a, .x = line_width,
        .iw = cap, .ih = join
    });
}

void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y,
                                double w, double h,
                                double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL_RECT, .x = x, .y = y, .w = w, .h = h,
        .r = r, .g = g, .b = b, .a = a
    });
}

// Intersect the canvas clip region with rect (x,y,w,h). AeVG emits one at the
// start of a scene flush to clip to the viewport (SVG overflow:hidden).
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y,
                                double w, double h) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_CLIP_RECT, .x = x, .y = y, .w = w, .h = h
    });
}

void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                                double start_angle, double end_angle) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_ARC, .x = cx, .y = cy, .w = radius,
        .a0 = start_angle, .a1 = end_angle
    });
}

void aether_ui_canvas_close_path_impl(int canvas_id) {
    canvas_add_cmd(canvas_id, (CanvasCmd){ .type = CANVAS_CLOSE_PATH });
}

void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL, .r = r, .g = g, .b = b, .a = a
    });
}

void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text,
                                      double x, double y, double font_size,
                                      double r, double g, double b, double a) {
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_FILL_TEXT, .x = x, .y = y, .w = font_size,
        .r = r, .g = g, .b = b, .a = a,
        .text = text ? strdup(text) : NULL
    });
}

void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;  // truncated buffer — skip
    // Own a copy: the Aether-side buffer doesn't outlive this call.
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_DRAW_IMAGE, .x = x, .y = y,
        .pixels = owned, .iw = iw, .ih = ih
    });
}

// As draw_image, but scale the iw×ih source to a dw×dh destination rect at
// (x,y) — for a video/raster frame whose pixel resolution differs from the
// region's canvas-px extent. dw/dh <= 0 falls back to a 1:1 native blit.
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y,
                                       double dw, double dh, int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    canvas_add_cmd(canvas_id, (CanvasCmd){
        .type = CANVAS_DRAW_IMAGE, .x = x, .y = y, .w = dw, .h = dh,
        .pixels = owned, .iw = iw, .ih = ih
    });
}

// std.collections FloatArray accessor (libaether) — read gradient
// stop arrays handed over as opaque FloatArray* from Aether.
extern double floatarr_get_unchecked(void* arr, int i);
extern int floatarr_size(void* arr);

// Copy n_stops offsets + n_stops*4 rgba comps out of the Aether
// FloatArray handles into freshly-owned C arrays on the command.
static void canvas_copy_stops(CanvasCmd* c, int n_stops,
                               void* offsets, void* rgba) {
    c->n_stops = n_stops;
    c->stop_off = (double*)malloc(sizeof(double) * (n_stops > 0 ? n_stops : 1));
    c->stop_rgba = (double*)malloc(sizeof(double) * (n_stops > 0 ? n_stops*4 : 1));
    for (int i = 0; i < n_stops; i++) {
        c->stop_off[i] = floatarr_get_unchecked(offsets, i);
        c->stop_rgba[i*4+0] = floatarr_get_unchecked(rgba, i*4+0);
        c->stop_rgba[i*4+1] = floatarr_get_unchecked(rgba, i*4+1);
        c->stop_rgba[i*4+2] = floatarr_get_unchecked(rgba, i*4+2);
        c->stop_rgba[i*4+3] = floatarr_get_unchecked(rgba, i*4+3);
    }
}

void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id,
        double x1, double y1, double x2, double y2,
        int n_stops, void* offsets, void* rgba, double line_width, int extend) {
    CanvasCmd cmd = { .type = CANVAS_FILL_LINEAR,
                      .gx1 = x1, .gy1 = y1, .gx2 = x2, .gy2 = y2,
                      .grad_line_width = line_width, .grad_extend = extend };
    canvas_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width, int extend) {
    CanvasCmd cmd = { .type = CANVAS_FILL_RADIAL,
                      .gx1 = cx, .gy1 = cy, .gr = radius, .gfx = fx, .gfy = fy,
                      .grad_line_width = line_width, .grad_extend = extend };
    canvas_copy_stops(&cmd, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, cmd);
}

void aether_ui_canvas_clear_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs) {
        // Free any owned text strings / image buffers / gradient stops.
        for (int i = 0; i < cs->count; i++) {
            CanvasCmd* c = &cs->cmds[i];
            if (c->type == CANVAS_FILL_TEXT && c->text) {
                free(c->text); c->text = NULL;
            }
            if (c->type == CANVAS_DRAW_IMAGE && c->pixels) {
                free(c->pixels); c->pixels = NULL;
            }
            if (c->type == CANVAS_FILL_LINEAR || c->type == CANVAS_FILL_RADIAL) {
                free(c->stop_off);  c->stop_off = NULL;
                free(c->stop_rgba); c->stop_rgba = NULL;
            }
        }
        cs->count = 0;
        GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
        if (w) gtk_widget_queue_draw(w);
    }
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    CanvasState* cs = get_canvas_state(canvas_id);
    if (cs) {
        GtkWidget* w = aether_ui_get_widget(cs->widget_handle);
        if (w) gtk_widget_queue_draw(w);
    }
}

// Event handlers — hover and double-click

static void on_hover_enter(GtkEventControllerMotion* ctrl, double x, double y,
                            gpointer data) {
    (void)ctrl; (void)x; (void)y;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*, intptr_t))c->fn)(c->env, 1);
    }
}

static void on_hover_leave(GtkEventControllerMotion* ctrl, gpointer data) {
    (void)ctrl;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*, intptr_t))c->fn)(c->env, 0);
    }
}

void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_hover_enter), boxed_closure);
    g_signal_connect(motion, "leave", G_CALLBACK(on_hover_leave), boxed_closure);
    gtk_widget_add_controller(w, motion);
}

static void on_double_click(GtkGestureClick* gesture, int n_press,
                             double x, double y, gpointer data) {
    (void)gesture; (void)x; (void)y;
    if (n_press != 2) return;
    AeClosure* c = (AeClosure*)data;
    if (c && c->fn) {
        ((void(*)(void*))c->fn)(c->env);
    }
}

void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    GtkGesture* gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_double_click), boxed_closure);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(gesture));
}

// Click handler (single click on any widget, not just buttons)
void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w || !boxed_closure) return;
    GtkGesture* gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 1);
    g_signal_connect(gesture, "released",
        G_CALLBACK(on_button_clicked), boxed_closure);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(gesture));
}

// Animation — opacity and position

void aether_ui_animate_opacity_impl(int handle, double target, int duration_ms) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    // For simplicity, set immediately (proper animation needs GLib timer)
    gtk_widget_set_opacity(w, target);
}

// Widget removal
void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    GtkWidget* parent = aether_ui_get_widget(parent_handle);
    GtkWidget* child = aether_ui_get_widget(child_handle);
    if (!parent || !child) return;
    if (GTK_IS_BOX(parent)) {
        gtk_box_remove(GTK_BOX(parent), child);
    }
}

void aether_ui_clear_children_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return;
    if (GTK_IS_BOX(w)) {
        GtkWidget* child;
        while ((child = gtk_widget_get_first_child(w)) != NULL) {
            gtk_box_remove(GTK_BOX(w), child);
        }
    }
}

// ---------------------------------------------------------------------------
// AetherUIDriver — Selenium-like AetherUIDriver
//
// An embedded HTTP server for Selenium-like remote control. When enabled:
//   1. A "Under Remote Control" banner is injected into the root — it cannot
//      be dismissed or hidden via the test API.
//   2. Widgets marked with seal_widget() are non-automatable — the test server
//      returns 403 Forbidden for any action on sealed widgets.
//   3. A simple HTTP API exposes widget state and actions.
//
// Endpoints:
//   GET  /widgets                    — list all widget handles + types (JSON)
//   GET  /widget/{id}               — widget state (type, text, value, visible)
//   POST /widget/{id}/click         — simulate button click
//   POST /widget/{id}/set_text?v=X  — set text/textfield value
//   POST /widget/{id}/toggle        — toggle a checkbox
//   POST /widget/{id}/set_value?v=X — set slider value
//   GET  /state/{id}                — get reactive state value
//   POST /state/{id}/set?v=X        — set reactive state value
//   POST /canvas/{id}/click?x=&y=   — click a canvas at (x,y); hit-tests AeVG shapes
//   POST /canvas/{id}/move?x=&y=    — pointer-move (hover) at (x,y)
//   POST /canvas/{id}/key?name=X    — key press by GDK name ("Down", "Return", …)
//   POST /window/resize?w=&h=       — resize the app's top-level window
//   POST /shutdown                  — close the app window; the process exits
//                                     cleanly (same path as the user closing
//                                     it). Harnesses use this instead of
//                                     signal-killing xvfb-run wrappers, which
//                                     leaves the app alive holding the port.
// ---------------------------------------------------------------------------

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

// Sealed widgets — test server refuses to interact with these.
static int* sealed_widgets = NULL;
static int sealed_count = 0;
static int sealed_capacity = 0;

void aether_ui_seal_widget_impl(int handle) {
    if (sealed_count >= sealed_capacity) {
        sealed_capacity = sealed_capacity == 0 ? 32 : sealed_capacity * 2;
        sealed_widgets = realloc(sealed_widgets, sizeof(int) * sealed_capacity);
    }
    sealed_widgets[sealed_count++] = handle;
}

// Walk GTK widget tree and seal every descendant.
static void seal_subtree_recursive(GtkWidget* w) {
    if (!w) return;
    // Find this widget's handle in the registry
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == w) {
            aether_ui_seal_widget_impl(i + 1);
            break;
        }
    }
    // Recurse into children
    for (GtkWidget* child = gtk_widget_get_first_child(w);
         child; child = gtk_widget_get_next_sibling(child)) {
        seal_subtree_recursive(child);
    }
}

void aether_ui_seal_subtree_impl(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) seal_subtree_recursive(w);
}

static int is_widget_sealed(int handle) {
    for (int i = 0; i < sealed_count; i++) {
        if (sealed_widgets[i] == handle) return 1;
    }
    return 0;
}

// Find the registry handle for a GtkWidget, or 0 if not registered.
static int handle_for_widget(GtkWidget* w) {
    if (!w) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == w) return i + 1;
    }
    return 0;
}

// Find parent handle for a widget by walking the GTK widget tree.
static int parent_handle_for(int handle) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) return 0;
    GtkWidget* parent = gtk_widget_get_parent(w);
    return handle_for_widget(parent);
}

// Banner widget handle — protected from the test API.
static int banner_handle = 0;

static const char* widget_type_name(GtkWidget* w) {
    if (!w) return "null";
    if (GTK_IS_LABEL(w)) return "text";
    if (GTK_IS_BUTTON(w)) return "button";
    if (GTK_IS_ENTRY(w)) return "textfield";
    if (GTK_IS_PASSWORD_ENTRY(w)) return "securefield";
    if (GTK_IS_CHECK_BUTTON(w)) return "toggle";
    if (GTK_IS_SCALE(w)) return "slider";
    if (GTK_IS_DROP_DOWN(w)) return "picker";
    if (GTK_IS_TEXT_VIEW(w)) return "textarea";
    if (GTK_IS_PROGRESS_BAR(w)) return "progressbar";
    if (GTK_IS_SEPARATOR(w)) return "divider";
    if (GTK_IS_SCROLLED_WINDOW(w)) return "scrollview";
    if (GTK_IS_OVERLAY(w)) return "zstack";
    if (GTK_IS_DRAWING_AREA(w)) return "canvas";
    if (GTK_IS_IMAGE(w)) return "image";
    if (GTK_IS_BOX(w)) {
        GtkOrientation o = gtk_orientable_get_orientation(GTK_ORIENTABLE(w));
        return o == GTK_ORIENTATION_VERTICAL ? "vstack" : "hstack";
    }
    return "widget";
}

// Get widget text content (for text labels and text fields).
static const char* widget_text_content(GtkWidget* w) {
    if (!w) return "";
    if (GTK_IS_LABEL(w)) return gtk_label_get_text(GTK_LABEL(w));
    if (GTK_IS_ENTRY(w)) {
        GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
        return gtk_entry_buffer_get_text(buf);
    }
    if (GTK_IS_CHECK_BUTTON(w)) return gtk_check_button_get_label(GTK_CHECK_BUTTON(w));
    if (GTK_IS_BUTTON(w)) return gtk_button_get_label(GTK_BUTTON(w));
    return "";
}

// Build JSON response for a single widget.
static int widget_to_json(int handle, char* buf, int bufsize) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (!w) {
        return snprintf(buf, bufsize, "{\"id\":%d,\"type\":\"null\"}", handle);
    }
    const char* type = widget_type_name(w);
    const char* text = widget_text_content(w);
    int visible = gtk_widget_get_visible(w) ? 1 : 0;
    int sealed = is_widget_sealed(handle) ? 1 : 0;
    int is_banner = (handle == banner_handle) ? 1 : 0;

    int enabled = gtk_widget_get_sensitive(w) ? 1 : 0;
    int n = snprintf(buf, bufsize,
        "{\"id\":%d,\"type\":\"%s\",\"text\":\"%s\",\"visible\":%s,\"sealed\":%s,\"banner\":%s,\"enabled\":%s",
        handle, type, text ? text : "",
        visible ? "true" : "false",
        sealed ? "true" : "false",
        is_banner ? "true" : "false",
        enabled ? "true" : "false");

    // Parent handle
    int parent_id = parent_handle_for(handle);
    n += snprintf(buf + n, bufsize - n, ",\"parent\":%d", parent_id);

    // Current allocation (0x0 until mapped). Tests use a canvas's real size
    // to compute the viewBox→pixel mapping after a /window/resize.
    n += snprintf(buf + n, bufsize - n, ",\"w\":%d,\"h\":%d",
                  gtk_widget_get_width(w), gtk_widget_get_height(w));

    // Add type-specific values
    if (GTK_IS_CHECK_BUTTON(w)) {
        int active = gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? 1 : 0;
        n += snprintf(buf + n, bufsize - n, ",\"active\":%s", active ? "true" : "false");
    } else if (GTK_IS_SCALE(w)) {
        double val = gtk_range_get_value(GTK_RANGE(w));
        n += snprintf(buf + n, bufsize - n, ",\"value\":%.2f", val);
    } else if (GTK_IS_PROGRESS_BAR(w)) {
        double val = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(w));
        n += snprintf(buf + n, bufsize - n, ",\"value\":%.2f", val);
    }

    n += snprintf(buf + n, bufsize - n, "}");
    return n;
}

// Parse a simple HTTP request: "GET /path HTTP/1.1\r\n..."
// Returns the method (0=GET, 1=POST) and path.
static int parse_http_request(const char* req, char* path, int pathsize) {
    int method = 0;
    if (strncmp(req, "POST", 4) == 0) method = 1;
    const char* p = strchr(req, ' ');
    if (!p) return -1;
    p++;
    const char* end = strchr(p, ' ');
    if (!end) end = strchr(p, '\r');
    if (!end) end = p + strlen(p);
    int len = (int)(end - p);
    if (len >= pathsize) len = pathsize - 1;
    memcpy(path, p, len);
    path[len] = '\0';
    return method;
}

// Extract integer from path like "/widget/5/click" → 5
static int extract_id_from_path(const char* path, const char* prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return -1;
    return atoi(path + plen);
}

// Extract query parameter: "/widget/5/set_text?v=hello" → "hello"
static const char* extract_query_param(const char* path, const char* key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char* p = strstr(path, needle);
    if (!p) return NULL;
    return p + strlen(needle);
}

// Data passed to the GTK idle callback for thread-safe widget interaction.
typedef struct {
    int action;  // 0=click 1=set_text 2=toggle 3=set_value 4=set_state
                 // 5=ctx_open 6=ctx_activate
    int handle;
    double dval;
    int ival;    // ctx item index (action 6)
    char sval[512];
    int done;
    int result;  // 0=ok, 1=sealed, 2=banner, 3=not_found
    int retval;  // action-specific return (ctx: mapped/count)
} TestAction;

static gboolean test_action_idle(gpointer data) {
    TestAction* ta = (TestAction*)data;
    GtkWidget* w = aether_ui_get_widget(ta->handle);

    if (ta->action == 4) {
        // State set — not a widget action
        aether_ui_state_set(ta->handle, ta->dval);
        ta->result = 0;
        ta->done = 1;
        return G_SOURCE_REMOVE;
    }

    if (!w) { ta->result = 3; ta->done = 1; return G_SOURCE_REMOVE; }
    if (ta->handle == banner_handle) { ta->result = 2; ta->done = 1; return G_SOURCE_REMOVE; }
    if (is_widget_sealed(ta->handle)) { ta->result = 1; ta->done = 1; return G_SOURCE_REMOVE; }

    switch (ta->action) {
        case 0: // click
            if (GTK_IS_BUTTON(w)) {
                g_signal_emit_by_name(w, "clicked");
            }
            break;
        case 1: // set_text
            if (GTK_IS_LABEL(w)) {
                gtk_label_set_text(GTK_LABEL(w), ta->sval);
            } else if (GTK_IS_ENTRY(w)) {
                GtkEntryBuffer* buf = gtk_entry_get_buffer(GTK_ENTRY(w));
                gtk_entry_buffer_set_text(buf, ta->sval, -1);
            }
            break;
        case 2: // toggle
            if (GTK_IS_CHECK_BUTTON(w)) {
                gboolean cur = gtk_check_button_get_active(GTK_CHECK_BUTTON(w));
                gtk_check_button_set_active(GTK_CHECK_BUTTON(w), !cur);
            }
            break;
        case 3: // set_value
            if (GTK_IS_SCALE(w)) {
                gtk_range_set_value(GTK_RANGE(w), ta->dval);
            } else if (GTK_IS_PROGRESS_BAR(w)) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), ta->dval);
            }
            break;
        case 5: // ctx_open — open the right-click menu, report mapped state
            ta->retval = aeui_ctx_menu_open(ta->handle);
            break;
        case 6: // ctx_activate — fire item[ival]'s closure
            ta->retval = aeui_ctx_menu_activate(ta->handle, ta->ival);
            break;
    }
    ta->result = 0;
    ta->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas click — fires a canvas's registered on_click closure with (x, y) on
// the GTK thread, exactly as the GtkGestureClick "pressed" handler would. Lets
// the HTTP driver exercise AeVG vg-scene shape clicks (hit-tested + recoloured)
// that the widget-level /widget/{id}/click can't reach.
typedef struct {
    int canvas_id;
    double x, y;
    int done;
    int result;  // 0=ok, 3=no such canvas / no handler
} CanvasClickAction;

static gboolean canvas_click_idle(gpointer data) {
    CanvasClickAction* a = (CanvasClickAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_click && cs->on_click->fn) {
        ((void(*)(void*, double, double))cs->on_click->fn)(cs->on_click->env, a->x, a->y);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas pointer-move — fires on_move with (x, y), as the motion controller
// would. Lets the driver test hover paths (hit-test → status line).
static gboolean canvas_move_idle(gpointer data) {
    CanvasClickAction* a = (CanvasClickAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_move && cs->on_move->fn) {
        ((void(*)(void*, double, double))cs->on_move->fn)(cs->on_move->env, a->x, a->y);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Canvas key press — fires on_key with a GDK key name ("Down", "Return",
// "Escape", "a"), as the key controller would. No focus needed: the closure
// is invoked directly, so tests can drive keyboard nav deterministically.
typedef struct {
    int canvas_id;
    char name[64];
    int done;
    int result;
} CanvasKeyAction;

static gboolean canvas_key_idle(gpointer data) {
    CanvasKeyAction* a = (CanvasKeyAction*)data;
    CanvasState* cs = get_canvas_state(a->canvas_id);
    if (cs && cs->on_key && cs->on_key->fn) {
        ((void(*)(void*, const char*))cs->on_key->fn)(cs->on_key->env, a->name);
        a->result = 0;
    } else {
        a->result = 3;
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

// Shutdown — destroy the top-level window on the GTK thread. GtkApplication
// ends its run loop when the last window goes, so the process exits by the
// SAME teardown path as a user closing the window. This is how test
// harnesses should end a driver session: signal-killing an xvfb-run wrapper
// leaves the app child alive and holding the driver port, and the next
// test then interrogates the wrong app.
static gboolean shutdown_idle(gpointer data) {
    (void)data;
    GtkWidget* root = aether_ui_get_widget(1);
    if (root) {
        GtkWidget* toplevel = root;
        while (gtk_widget_get_parent(toplevel))
            toplevel = gtk_widget_get_parent(toplevel);
        if (GTK_IS_WINDOW(toplevel)) gtk_window_destroy(GTK_WINDOW(toplevel));
    }
    return G_SOURCE_REMOVE;
}

// Window resize — set_default_size on the top-level acts as a resize request
// on a mapped GTK4 window. Lets tests exercise the resize → viewBox-remap →
// coordinate-unmapping path (a maximize/unmaximize regression hid here).
typedef struct {
    int w, h;
    int done;
    int result;
} WindowResizeAction;

static gboolean window_resize_idle(gpointer data) {
    WindowResizeAction* a = (WindowResizeAction*)data;
    GtkWidget* root = aether_ui_get_widget(1);
    a->result = 3;
    if (root) {
        GtkWidget* toplevel = root;
        while (gtk_widget_get_parent(toplevel))
            toplevel = gtk_widget_get_parent(toplevel);
        if (GTK_IS_WINDOW(toplevel)) {
            gtk_window_set_default_size(GTK_WINDOW(toplevel), a->w, a->h);
            a->result = 0;
        }
    }
    a->done = 1;
    return G_SOURCE_REMOVE;
}

static void send_response(int fd, int status, const char* status_text,
                           const char* content_type, const char* body) {
    char header[512];
    int bodylen = body ? (int)strlen(body) : 0;
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, bodylen);
    write(fd, header, hlen);
    if (body && bodylen > 0) write(fd, body, bodylen);
}

// Screenshot result — shared between server thread and GTK idle callback.
static unsigned char* ss_result_data = NULL;
static gsize ss_result_len = 0;
static volatile int ss_result_done = 0;

static gboolean screenshot_idle_cb(gpointer data) {
    (void)data;
    ss_result_data = NULL;
    ss_result_len = 0;

    GtkWidget* root = aether_ui_get_widget(1);
    if (!root) { ss_result_done = 1; return G_SOURCE_REMOVE; }

    GtkWidget* toplevel = root;
    while (gtk_widget_get_parent(toplevel))
        toplevel = gtk_widget_get_parent(toplevel);
    if (!GTK_IS_WINDOW(toplevel)) { ss_result_done = 1; return G_SOURCE_REMOVE; }

    GdkPaintable* paintable = gtk_widget_paintable_new(toplevel);
    int w = gdk_paintable_get_intrinsic_width(paintable);
    int h = gdk_paintable_get_intrinsic_height(paintable);
    if (w <= 0 || h <= 0) { w = 400; h = 300; }

    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, (double)w, (double)h);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    if (!node) {
        g_object_unref(paintable);
        ss_result_done = 1;
        return G_SOURCE_REMOVE;
    }

    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(toplevel));
    GskRenderer* renderer = gsk_renderer_new_for_surface(surface);
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node,
        &GRAPHENE_RECT_INIT(0, 0, w, h));
    GBytes* bytes = gdk_texture_save_to_png_bytes(texture);

    gsize len = 0;
    const unsigned char* raw = g_bytes_get_data(bytes, &len);
    ss_result_data = malloc(len);
    memcpy(ss_result_data, raw, len);
    ss_result_len = len;

    g_bytes_unref(bytes);
    g_object_unref(texture);
    gsk_render_node_unref(node);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    g_object_unref(paintable);
    ss_result_done = 1;
    return G_SOURCE_REMOVE;
}

static void handle_test_request(int client_fd) {
    char req[4096];
    int n = (int)read(client_fd, req, sizeof(req) - 1);
    if (n <= 0) { close(client_fd); return; }
    req[n] = '\0';

    char path[1024];
    int method = parse_http_request(req, path, sizeof(path));
    if (method < 0) {
        send_response(client_fd, 400, "Bad Request", "text/plain", "Bad request");
        close(client_fd);
        return;
    }

    // GET /widgets — list all widgets, with optional query filters
    //   /widgets           — all widgets
    //   /widgets?type=button  — only buttons
    //   /widgets?text=Submit  — only widgets whose text matches
    //   /widgets?type=button&text=+1  — combined filter
    if (method == 0 && strncmp(path, "/widgets", 8) == 0 &&
        (path[8] == '\0' || path[8] == '?')) {
        const char* filter_type = extract_query_param(path, "type");
        const char* filter_text = extract_query_param(path, "text");
        // Truncate filter values at '&' (multi-param)
        char ft_buf[128] = "", fx_buf[128] = "";
        if (filter_type) {
            strncpy(ft_buf, filter_type, sizeof(ft_buf) - 1);
            char* amp = strchr(ft_buf, '&'); if (amp) *amp = '\0';
        }
        if (filter_text) {
            strncpy(fx_buf, filter_text, sizeof(fx_buf) - 1);
            char* amp = strchr(fx_buf, '&'); if (amp) *amp = '\0';
        }

        char* body = malloc(widget_count * 512 + 64);
        int pos = 0;
        int first = 1;
        pos += sprintf(body + pos, "[");
        for (int i = 1; i <= widget_count; i++) {
            GtkWidget* w = aether_ui_get_widget(i);
            if (!w) continue;
            if (ft_buf[0] && strcmp(widget_type_name(w), ft_buf) != 0) continue;
            if (fx_buf[0]) {
                const char* t = widget_text_content(w);
                if (!t || strcmp(t, fx_buf) != 0) continue;
            }
            if (!first) pos += sprintf(body + pos, ",");
            first = 0;
            pos += widget_to_json(i, body + pos, 512);
        }
        pos += sprintf(body + pos, "]");
        send_response(client_fd, 200, "OK", "application/json", body);
        free(body);
        close(client_fd);
        return;
    }

    // GET /widget/{id}/children — list child widget handles
    if (method == 0 && strncmp(path, "/widget/", 8) == 0) {
        char* suffix = strchr(path + 8, '/');
        if (suffix && strcmp(suffix, "/children") == 0) {
            int id = atoi(path + 8);
            GtkWidget* w = aether_ui_get_widget(id);
            if (!w) {
                send_response(client_fd, 404, "Not Found", "application/json",
                              "{\"error\":\"widget not found\"}");
                close(client_fd);
                return;
            }
            char* body = malloc(widget_count * 64 + 64);
            int pos = 0;
            int first = 1;
            pos += sprintf(body + pos, "[");
            for (GtkWidget* child = gtk_widget_get_first_child(w);
                 child; child = gtk_widget_get_next_sibling(child)) {
                int ch = handle_for_widget(child);
                if (ch > 0) {
                    if (!first) pos += sprintf(body + pos, ",");
                    first = 0;
                    pos += widget_to_json(ch, body + pos, 512);
                }
            }
            pos += sprintf(body + pos, "]");
            send_response(client_fd, 200, "OK", "application/json", body);
            free(body);
            close(client_fd);
            return;
        }
    }

    // GET /screenshot — capture the root widget as PNG
    if (method == 0 && strcmp(path, "/screenshot") == 0) {
        ss_result_done = 0;
        g_idle_add(screenshot_idle_cb, NULL);
        while (!ss_result_done) usleep(1000);

        if (ss_result_data && ss_result_len > 0) {
            char header[256];
            int hlen = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/png\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n", ss_result_len);
            write(client_fd, header, hlen);
            write(client_fd, ss_result_data, ss_result_len);
            free(ss_result_data);
            ss_result_data = NULL;
        } else {
            send_response(client_fd, 500, "Error", "application/json",
                          "{\"error\":\"screenshot failed\"}");
        }
        close(client_fd);
        return;
    }

    // GET /widget/{id} — single widget info (no trailing slash/action)
    if (method == 0 && strncmp(path, "/widget/", 8) == 0 && strchr(path + 8, '/') == NULL) {
        int id = atoi(path + 8);
        if (id < 1 || id > widget_count) {
            send_response(client_fd, 404, "Not Found", "application/json",
                          "{\"error\":\"widget not found\"}");
        } else {
            char buf[512];
            widget_to_json(id, buf, sizeof(buf));
            send_response(client_fd, 200, "OK", "application/json", buf);
        }
        close(client_fd);
        return;
    }

    // GET /state/{id}
    if (method == 0 && strncmp(path, "/state/", 7) == 0 && strchr(path + 7, '/') == NULL) {
        int id = atoi(path + 7);
        double val = aether_ui_state_get(id);
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"id\":%d,\"value\":%.6f}", id, val);
        send_response(client_fd, 200, "OK", "application/json", buf);
        close(client_fd);
        return;
    }

    // POST actions — must run on GTK main thread via idle callback
    TestAction ta = {0};
    ta.done = 0;
    ta.result = 3;

    // POST /canvas/{id}/click?x=..&y=.. — fire a canvas's on_click at (x, y).
    // Hit-tests AeVG vg-scene shapes (per-element handlers), unlike the
    // widget-level click which has no coordinates.
    if (method == 1 && strncmp(path, "/canvas/", 8) == 0) {
        char* action_part = strchr(path + 8, '/');
        if (action_part && (strncmp(action_part, "/click", 6) == 0 ||
                            strncmp(action_part, "/move", 5) == 0)) {
            CanvasClickAction ca = {0};
            ca.canvas_id = atoi(path + 8);
            const char* xs = extract_query_param(path, "x");
            const char* ys = extract_query_param(path, "y");
            ca.x = xs ? atof(xs) : 0.0;
            ca.y = ys ? atof(ys) : 0.0;
            int is_move = action_part[1] == 'm';
            g_idle_add(is_move ? canvas_move_idle : canvas_click_idle, &ca);
            while (!ca.done) usleep(1000);
            if (ca.result == 0) {
                send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            } else {
                send_response(client_fd, 404, "Not Found", "application/json",
                              is_move ? "{\"error\":\"no canvas move handler\"}"
                                      : "{\"error\":\"no canvas click handler\"}");
            }
            close(client_fd);
            return;
        }
        if (action_part && strncmp(action_part, "/key", 4) == 0) {
            CanvasKeyAction ka = {0};
            ka.canvas_id = atoi(path + 8);
            const char* ns = extract_query_param(path, "name");
            if (ns) {
                strncpy(ka.name, ns, sizeof(ka.name) - 1);
                char* amp = strchr(ka.name, '&'); if (amp) *amp = '\0';
            }
            g_idle_add(canvas_key_idle, &ka);
            while (!ka.done) usleep(1000);
            if (ka.result == 0) {
                send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            } else {
                send_response(client_fd, 404, "Not Found", "application/json",
                              "{\"error\":\"no canvas key handler\"}");
            }
            close(client_fd);
            return;
        }
    }

    // POST /shutdown — close the window; the app exits cleanly. Respond
    // first (the main loop is about to end), then schedule the destroy.
    if (method == 1 && strncmp(path, "/shutdown", 9) == 0) {
        send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
        close(client_fd);
        g_idle_add(shutdown_idle, NULL);
        return;
    }

    // POST /window/resize?w=..&h=.. — resize the app's top-level window.
    if (method == 1 && strncmp(path, "/window/resize", 14) == 0) {
        WindowResizeAction ra = {0};
        const char* ws = extract_query_param(path, "w");
        const char* hs = extract_query_param(path, "h");
        ra.w = ws ? atoi(ws) : 0;
        ra.h = hs ? atoi(hs) : 0;
        if (ra.w > 0 && ra.h > 0) {
            g_idle_add(window_resize_idle, &ra);
            while (!ra.done) usleep(1000);
        }
        if (ra.w > 0 && ra.h > 0 && ra.result == 0) {
            send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
        } else {
            send_response(client_fd, 400, "Bad Request", "application/json",
                          "{\"error\":\"need w>0, h>0 and a top-level window\"}");
        }
        close(client_fd);
        return;
    }

    // POST /widget/{id}/context_menu        — open the right-click menu
    // POST /widget/{id}/context_menu/{idx}  — open, then activate item idx
    // Drives the REAL backend menu path (the same ctx_menu_open the gesture
    // uses), so this exercises the popover on the live compositor and reports
    // whether it mapped — the assertion X11-injection tests can't give. Placed
    // before the generic /widget/ dispatch so it isn't caught by its
    // unknown-action 400.
    if (method == 1 && strncmp(path, "/widget/", 8) == 0) {
        char* cm_part = strchr(path + 8, '/');
        if (cm_part && strncmp(cm_part, "/context_menu", 13) == 0) {
            ta.handle = atoi(path + 8);
            ta.action = 5; // open
            g_idle_add(test_action_idle, &ta);
            while (!ta.done) usleep(1000);
            int mapped = ta.retval;
            const char* idxp = cm_part + 13;   // optional trailing /{idx}
            if (*idxp == '/' && idxp[1] != '\0') {
                TestAction act = {0};
                act.handle = ta.handle;
                act.action = 6;
                act.ival = atoi(idxp + 1);
                g_idle_add(test_action_idle, &act);
                while (!act.done) usleep(1000);
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "{\"mapped\":%d,\"activated\":%d}", mapped, act.retval);
                send_response(client_fd, 200, "OK", "application/json", buf);
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"mapped\":%d}", mapped);
                send_response(client_fd, 200, "OK", "application/json", buf);
            }
            close(client_fd);
            return;
        }
    }

    // POST /widget/{id}/click
    if (method == 1 && strncmp(path, "/widget/", 8) == 0) {
        char* action_part = strchr(path + 8, '/');
        if (action_part) {
            ta.handle = atoi(path + 8);
            if (strncmp(action_part, "/click", 6) == 0) {
                ta.action = 0;
            } else if (strncmp(action_part, "/set_text", 9) == 0) {
                ta.action = 1;
                const char* v = extract_query_param(path, "v");
                if (v) strncpy(ta.sval, v, sizeof(ta.sval) - 1);
            } else if (strncmp(action_part, "/toggle", 7) == 0) {
                ta.action = 2;
            } else if (strncmp(action_part, "/set_value", 10) == 0) {
                ta.action = 3;
                const char* v = extract_query_param(path, "v");
                if (v) ta.dval = atof(v);
            } else {
                send_response(client_fd, 400, "Bad Request", "application/json",
                              "{\"error\":\"unknown action\"}");
                close(client_fd);
                return;
            }

            g_idle_add(test_action_idle, &ta);
            // Busy-wait for GTK thread to complete the action
            while (!ta.done) usleep(1000);

            if (ta.result == 1) {
                send_response(client_fd, 403, "Forbidden", "application/json",
                              "{\"error\":\"widget is sealed\"}");
            } else if (ta.result == 2) {
                send_response(client_fd, 403, "Forbidden", "application/json",
                              "{\"error\":\"banner cannot be manipulated\"}");
            } else if (ta.result == 3) {
                send_response(client_fd, 404, "Not Found", "application/json",
                              "{\"error\":\"widget not found\"}");
            } else {
                send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            }
            close(client_fd);
            return;
        }
    }

    // POST /state/{id}/set?v=X
    if (method == 1 && strncmp(path, "/state/", 7) == 0) {
        char* action_part = strchr(path + 7, '/');
        if (action_part && strncmp(action_part, "/set", 4) == 0) {
            ta.handle = atoi(path + 7);
            ta.action = 4;
            const char* v = extract_query_param(path, "v");
            if (v) ta.dval = atof(v);
            g_idle_add(test_action_idle, &ta);
            while (!ta.done) usleep(1000);
            send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            close(client_fd);
            return;
        }
    }

    // --- AetherUIDriver: tray + notifications ---
    //
    // Introspection GETs read the registry directly from the HTTP
    // thread — records are append-only and read-only after creation,
    // so a concurrent read sees a coherent snapshot.
    //
    // POST dispatch fires the registered Aether closure synchronously
    // on the HTTP thread. This is intentional: tray-only / headless
    // apps have no GTK main loop draining g_idle_add, so a thread-
    // marshalling dispatch would silently never fire under
    // app_run_headless(). The trade-off is that tray callbacks must
    // not touch GtkWidgets directly — they should mutate reactive
    // state cells or schedule work (consistent with the AvnSync v2
    // shape from the asks). When a real native tray backend lands
    // and runs inside a GTK main loop, the native callback itself
    // already runs on the GTK thread, so the dispatch model still
    // works without code change.

    // GET /tray
    if (method == 0 && strcmp(path, "/tray") == 0) {
        int n = aether_ui_tray_count();
        char* body = (char*)malloc((size_t)n * 512 + 64);
        int pos = sprintf(body, "[");
        for (int i = 1; i <= n; i++) {
            if (i > 1) pos += sprintf(body + pos, ",");
            pos += snprintf(body + pos, 512,
                "{\"id\":%d,\"name\":\"%s\",\"tooltip\":\"%s\","
                "\"menu_handle\":%d,\"icon\":\"%s\",\"template\":%s,"
                "\"sealed\":%s}",
                i, aether_ui_tray_name(i), aether_ui_tray_tooltip(i),
                aether_ui_tray_menu_handle(i),
                aether_ui_tray_current_icon(i),
                aether_ui_tray_is_template(i) ? "true" : "false",
                aether_ui_tray_is_sealed(i)   ? "true" : "false");
        }
        sprintf(body + pos, "]");
        send_response(client_fd, 200, "OK", "application/json", body);
        free(body);
        close(client_fd);
        return;
    }

    // GET /tray/{id}/icon — current icon (resolves reactive state cell)
    if (method == 0 && strncmp(path, "/tray/", 6) == 0 && strstr(path, "/icon")) {
        int id = atoi(path + 6);
        const char* icon = aether_ui_tray_current_icon(id);
        send_response(client_fd, 200, "OK", "text/plain", icon);
        close(client_fd);
        return;
    }

    // GET /tray/{id}
    if (method == 0 && strncmp(path, "/tray/", 6) == 0 && strchr(path + 6, '/') == NULL) {
        int id = atoi(path + 6);
        if (id < 1 || id > aether_ui_tray_count()) {
            send_response(client_fd, 404, "Not Found", "application/json",
                          "{\"error\":\"tray not found\"}");
        } else {
            char body[1024];
            snprintf(body, sizeof(body),
                "{\"id\":%d,\"name\":\"%s\",\"tooltip\":\"%s\","
                "\"menu_handle\":%d,\"icon\":\"%s\",\"template\":%s,"
                "\"sealed\":%s}",
                id, aether_ui_tray_name(id), aether_ui_tray_tooltip(id),
                aether_ui_tray_menu_handle(id),
                aether_ui_tray_current_icon(id),
                aether_ui_tray_is_template(id) ? "true" : "false",
                aether_ui_tray_is_sealed(id)   ? "true" : "false");
            send_response(client_fd, 200, "OK", "application/json", body);
        }
        close(client_fd);
        return;
    }

    // POST /tray/{id}/click | /menu/activate?label=... | /set_tooltip?v=...
    if (method == 1 && strncmp(path, "/tray/", 6) == 0) {
        int id = atoi(path + 6);
        if (strstr(path, "/click")) {
            int r = aether_ui_tray_emit_click(id);
            if (r == 0)      send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            else if (r == 1) send_response(client_fd, 403, "Forbidden", "application/json", "{\"error\":\"sealed\"}");
            else if (r == 4) send_response(client_fd, 204, "No Content", "application/json", "");
            else             send_response(client_fd, 404, "Not Found", "application/json", "{\"error\":\"tray not found\"}");
            close(client_fd);
            return;
        }
        if (strstr(path, "/menu/activate")) {
            const char* v = extract_query_param(path, "label");
            char label[256] = "";
            if (v) {
                strncpy(label, v, sizeof(label) - 1);
                char* amp = strchr(label, '&'); if (amp) *amp = '\0';
                // Minimal URL-decode for spaces (most tests need this).
                for (char* p = label; *p; p++) if (*p == '+') *p = ' ';
            }
            int r = aether_ui_tray_menu_activate(id, label);
            if (r == 0)      send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            else if (r == 1) send_response(client_fd, 403, "Forbidden", "application/json", "{\"error\":\"sealed\"}");
            else if (r == 4) send_response(client_fd, 204, "No Content", "application/json", "");
            else             send_response(client_fd, 404, "Not Found", "application/json", "{\"error\":\"item not found\"}");
            close(client_fd);
            return;
        }
        if (strstr(path, "/set_tooltip")) {
            const char* v = extract_query_param(path, "v");
            char text[256] = "";
            if (v) {
                strncpy(text, v, sizeof(text) - 1);
                char* amp = strchr(text, '&'); if (amp) *amp = '\0';
                for (char* p = text; *p; p++) if (*p == '+') *p = ' ';
            }
            aether_ui_tray_set_tooltip_reg(id, text);
            send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            close(client_fd);
            return;
        }
    }

    // GET /notifications
    if (method == 0 && strcmp(path, "/notifications") == 0) {
        int n = aether_ui_notif_count();
        char* body = (char*)malloc((size_t)n * 1536 + 64);
        int pos = sprintf(body, "[");
        for (int i = 1; i <= n; i++) {
            if (i > 1) pos += sprintf(body + pos, ",");
            pos += snprintf(body + pos, 1536,
                "{\"id\":%d,\"title\":\"%s\",\"body\":\"%s\","
                "\"icon\":\"%s\",\"tag\":\"%s\",\"dismissed\":%s}",
                i, aether_ui_notif_title(i), aether_ui_notif_body(i),
                aether_ui_notif_icon(i), aether_ui_notif_tag(i),
                aether_ui_notif_dismissed(i) ? "true" : "false");
        }
        sprintf(body + pos, "]");
        send_response(client_fd, 200, "OK", "application/json", body);
        free(body);
        close(client_fd);
        return;
    }

    // POST /notifications/{id}/click | /dismiss
    if (method == 1 && strncmp(path, "/notifications/", 15) == 0) {
        int id = atoi(path + 15);
        if (strstr(path, "/click")) {
            int r = aether_ui_notif_emit_click(id);
            if (r == 0)      send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            else if (r == 4) send_response(client_fd, 204, "No Content", "application/json", "");
            else             send_response(client_fd, 404, "Not Found", "application/json", "{\"error\":\"notification not found\"}");
            close(client_fd);
            return;
        }
        if (strstr(path, "/dismiss")) {
            int r = aether_ui_notif_mark_dismissed(id);
            if (r == 0) send_response(client_fd, 200, "OK", "application/json", "{\"ok\":true}");
            else        send_response(client_fd, 404, "Not Found", "application/json", "{\"error\":\"notification not found\"}");
            close(client_fd);
            return;
        }
    }

    send_response(client_fd, 404, "Not Found", "text/plain", "Not found");
    close(client_fd);
}

static void* test_server_thread(void* arg) {
    int port = (int)(intptr_t)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return NULL;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "AetherUIDriver: failed to bind to port %d: %s\n",
                port, strerror(errno));
        close(server_fd);
        return NULL;
    }
    listen(server_fd, 8);
    fprintf(stderr, "AetherUIDriver: listening on http://127.0.0.1:%d\n", port);

    for (;;) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;
        handle_test_request(client);
    }
    return NULL;
}

// Inject the "Under Remote Control" banner into the app root.
// Called from the GTK activate callback when test mode is on.
static void inject_remote_control_banner(int root_handle) {
    GtkWidget* root = aether_ui_get_widget(root_handle);
    if (!root || !GTK_IS_BOX(root)) return;

    GtkWidget* banner = gtk_label_new("Under Remote Control");
    gtk_widget_add_css_class(banner, "aether-rc-banner");

    GtkCssProvider* prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov,
        ".aether-rc-banner {"
        "  background-color: #cc3333;"
        "  color: white;"
        "  font-weight: bold;"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}", -1);
    GdkDisplay* display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(prov),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
    }
    g_object_unref(prov);

    banner_handle = aether_ui_register_widget(banner);
    gtk_box_prepend(GTK_BOX(root), banner);
}

static int test_server_port = 0;

// Nonzero once the driver's test server is armed (see on_canvas_move).
static int aeui_remote_controlled(void) { return test_server_port != 0; }

void aether_ui_enable_test_server_impl(int port, int root_handle) {
    test_server_port = port;

    inject_remote_control_banner(root_handle);

    pthread_t tid;
    pthread_create(&tid, NULL, test_server_thread, (void*)(intptr_t)port);
    pthread_detach(tid);
}

// ---------------------------------------------------------------------------
// Widget tree
// ---------------------------------------------------------------------------

// Called from Aether DSL wrappers where parent comes through _ctx (void*).
void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    GtkWidget* parent = aether_ui_get_widget(parent_handle);
    GtkWidget* child = aether_ui_get_widget(child_handle);
    if (!parent || !child) return;

    if (GTK_IS_BOX(parent)) {
        if (gtk_orientable_get_orientation(GTK_ORIENTABLE(parent)) == GTK_ORIENTATION_HORIZONTAL) {
            if (gtk_widget_get_hexpand(child) && gtk_widget_get_vexpand(child)) {
                gtk_widget_set_vexpand(child, FALSE);
            }
        }
        gtk_box_append(GTK_BOX(parent), child);
    } else if (GTK_IS_SCROLLED_WINDOW(parent)) {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(parent), child);
    } else if (GTK_IS_OVERLAY(parent)) {
        if (gtk_overlay_get_child(GTK_OVERLAY(parent)) == NULL) {
            gtk_overlay_set_child(GTK_OVERLAY(parent), child);
        } else {
            gtk_overlay_add_overlay(GTK_OVERLAY(parent), child);
        }
    }
}

void aether_ui_widget_set_hidden(int handle, int hidden) {
    GtkWidget* w = aether_ui_get_widget(handle);
    if (w) gtk_widget_set_visible(w, !hidden);
}

// ---------------------------------------------------------------------------
// Menus.
// GTK4 removed traditional GtkMenu in favor of GMenuModel + GtkPopoverMenu.
// Full native wiring (GMenu / GActionGroup) is follow-up work; for now
// these stubs maintain the cross-platform ABI so the test suite links and
// the DSL can be written once against a stable API. Menu items won't
// actually fire on GTK4 yet — track via the stub_actions list when it's
// worth wiring up.
// ---------------------------------------------------------------------------

typedef struct {
    int   is_bar;
    char* label;
} GtkMenuEntry;

static GtkMenuEntry* gtk_menus = NULL;
static int           gtk_menu_count = 0;
static int           gtk_menu_capacity = 0;

static int gtk_register_menu(int is_bar, const char* label) {
    if (gtk_menu_count >= gtk_menu_capacity) {
        gtk_menu_capacity = gtk_menu_capacity == 0 ? 8 : gtk_menu_capacity * 2;
        gtk_menus = (GtkMenuEntry*)realloc(gtk_menus,
                                           sizeof(GtkMenuEntry) * gtk_menu_capacity);
    }
    gtk_menus[gtk_menu_count].is_bar = is_bar;
    gtk_menus[gtk_menu_count].label = label ? strdup(label) : NULL;
    gtk_menu_count++;
    return gtk_menu_count;
}

int aether_ui_menu_bar_create(void) {
    return gtk_register_menu(1, NULL);
}

int aether_ui_menu_create(const char* label) {
    return gtk_register_menu(0, label);
}

void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure) {
    // Record into the cross-backend side-store so the AetherUIDriver
    // /tray/{id}/menu/activate route (and any future menu_popup test
    // route) can invoke the closure by label. The real GTK4 menu wiring
    // (GMenuModel + GActionGroup) is still TODO — see the stub header
    // comment above — but the closure is reachable from the driver today.
    aether_ui_menu_item_record(menu_handle, label, boxed_closure);
}

void aether_ui_menu_add_separator(int menu_handle) { (void)menu_handle; }

void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    (void)bar_handle; (void)menu_handle;
}

void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    (void)app_handle; (void)bar_handle;
}

void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    (void)menu_handle; (void)anchor_widget;
}

// ---------------------------------------------------------------------------
// Grid layout (GtkGrid).
// ---------------------------------------------------------------------------
int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    (void)cols; // GtkGrid sizes to content rather than using a fixed col count.
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), row_spacing);
    gtk_grid_set_column_spacing(GTK_GRID(grid), col_spacing);
    return aether_ui_register_widget(grid);
}

void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span) {
    GtkWidget* grid = aether_ui_get_widget(grid_handle);
    GtkWidget* child = aether_ui_get_widget(child_handle);
    if (!grid || !child || !GTK_IS_GRID(grid)) return;
    // If the child already has a parent, unparent first.
    GtkWidget* cur_parent = gtk_widget_get_parent(child);
    if (cur_parent) gtk_widget_unparent(child);
    gtk_grid_attach(GTK_GRID(grid), child, col, row, col_span, row_span);
}

// ---------------------------------------------------------------------------
// Reverse lookup — aether_ui_handle_for_widget.
// Linear scan over the widget registry. The hash-backed O(1) version
// ships in the Win32 backend; porting the hash to GTK4 is straightforward
// follow-up (GtkWidget* maps cleanly to the same hash).
// ---------------------------------------------------------------------------
int aether_ui_handle_for_widget(void* widget) {
    if (!widget) return 0;
    for (int i = 0; i < widget_count; i++) {
        if (widgets[i] == widget) return i + 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// System tray (Group 7).
//
// Registry-only on GTK4 today. The platform options for a real native
// tray in 2026 are:
//
//   - libayatana-appindicator (-1 / -3 packages): hard-wired to GTK3.
//     The header pulls in <gtk/gtk.h> at the GTK3 search path, which
//     conflicts with the GTK4 typedefs already in this file. Mixing
//     GTK3 and GTK4 in the same process is the original menu-stubbing
//     reason — same trap here.
//
//   - StatusNotifierItem (KDE freedesktop spec) over GDBus. The
//     GTK-version-agnostic path forward; ~400 LOC of GIO D-Bus code to
//     register an SNI on the bus + a DBusMenu for the popup. Adopted
//     by GNOME (via gnome-shell-extension-appindicator), KDE,
//     Cinnamon, Budgie. The right answer for an actual Linux tray.
//
// Phase 1 (this ask) ships the DSL + registry + AetherUIDriver routes
// so AvnSync v2 can wire its callbacks against a stable surface and
// validate them in CI via /tray/{id}/click and
// /tray/{id}/menu/activate. When the SNI implementation lands, it
// only needs to call aether_ui_tray_emit_click() / _menu_activate()
// on dbus events; the DSL contract stays unchanged.
//
// AETHER_UI_HEADLESS does not gate anything here because there's no
// native call to suppress yet. The flag still matters for app_run_
// headless's logging.
// ---------------------------------------------------------------------------
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) {
    int id = aether_ui_tray_register(name, boxed_left_click);
    if (!aeui_is_headless()) aether_ui_sni_register(id);
    return id;
}

void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) {
    aether_ui_tray_set_tooltip_reg(tray_id, text);
    aether_ui_sni_invalidate_tooltip(tray_id);
}

void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) {
    aether_ui_tray_set_menu_reg(tray_id, menu_handle);
    aether_ui_sni_invalidate_menu(tray_id);
}

void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle,
                                             const char* icon_clean,
                                             const char* icon_busy,
                                             const char* icon_alert) {
    aether_ui_tray_set_icon_for_state_reg(tray_id, state_handle,
                                          icon_clean, icon_busy, icon_alert);
    aether_ui_sni_invalidate_icon(tray_id);
}

void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) {
    aether_ui_tray_set_icon_template_reg(tray_id, is_template);
}

void aether_ui_tray_seal_impl(int tray_id) {
    aether_ui_tray_seal_reg(tray_id);
}

// ---------------------------------------------------------------------------
// Desktop notifications (Group 7b).
//
// libnotify is GTK-version-agnostic — depends only on glib/gobject/
// gdk-pixbuf — so the real native path ships on GTK4 today. When
// libnotify is missing (AEUI_HAVE_LIBNOTIFY undefined), the build
// falls through to registry-only, matching the tray story.
//
// Click callback: NotifyNotification emits "action-invoked" on the
// glib main loop, so the callback already runs on the right thread.
// We close over the boxed closure via g_object_set_data + the action
// handler invokes the registry's dispatch helper.
//
// AETHER_UI_HEADLESS suppresses the OS-level notify_notification_show
// call; the record still lands in the registry so driver tests can
// exercise click/dismiss.
// ---------------------------------------------------------------------------

#ifdef AEUI_HAVE_LIBNOTIFY
static int gtk_libnotify_init = 0;

static void ensure_libnotify_init(void) {
    if (!gtk_libnotify_init) {
        notify_init("aether-ui");
        gtk_libnotify_init = 1;
    }
}

// glib action handler: when the user clicks the notification, dispatch
// through the shared registry so the registered Aether closure fires.
static void on_libnotify_action(NotifyNotification* n, char* action,
                                 gpointer user_data) {
    (void)n; (void)action;
    int notif_id = GPOINTER_TO_INT(user_data);
    aether_ui_notif_emit_click(notif_id);
}

static void on_libnotify_closed(NotifyNotification* n, gpointer user_data) {
    (void)n;
    int notif_id = GPOINTER_TO_INT(user_data);
    aether_ui_notif_mark_dismissed(notif_id);
}

static void show_libnotify(int notif_id, const char* title, const char* body,
                            const char* icon_path, int with_click) {
    if (aeui_is_headless()) return;
    ensure_libnotify_init();
    const char* icon = (icon_path && icon_path[0]) ? icon_path : "dialog-information";
    NotifyNotification* n = notify_notification_new(
        title ? title : "", body ? body : "", icon);
    if (with_click) {
        // "default" is the freedesktop spec's reserved action key for
        // "user clicked the bubble (not a specific button)".
        notify_notification_add_action(n, "default", "Default",
            on_libnotify_action, GINT_TO_POINTER(notif_id), NULL);
    }
    g_signal_connect(n, "closed", G_CALLBACK(on_libnotify_closed),
                     GINT_TO_POINTER(notif_id));
    GError* err = NULL;
    if (!notify_notification_show(n, &err)) {
        // D-Bus notifier daemon may not be running (headless test
        // env, minimal WMs). Silently drop — the registry record
        // still exists for the driver to act on.
        if (err) g_error_free(err);
    }
}
#endif

int aether_ui_notify_impl(const char* title, const char* body) {
    int id = aether_ui_notify_register(title, body);
#ifdef AEUI_HAVE_LIBNOTIFY
    show_libnotify(id, title, body, NULL, 0);
#endif
    return id;
}

int aether_ui_notify_full_impl(const char* title, const char* body,
                                const char* icon_path, const char* tag,
                                void* boxed_click) {
    int id = aether_ui_notify_register_full(title, body, icon_path, tag, boxed_click);
#ifdef AEUI_HAVE_LIBNOTIFY
    show_libnotify(id, title, body, icon_path, boxed_click ? 1 : 0);
#endif
    return id;
}

int aether_ui_notify_request_permission_impl(void) {
    // Linux: always granted (libnotify just talks to the running
    // notification daemon over D-Bus; nothing to prompt for).
    return aether_ui_notify_request_permission();
}

// app_run_headless on Linux: pump a GMainLoop so D-Bus signals
// (SNI Activate/ContextMenu, DBusMenu Event/AboutToShow) get
// delivered. Without a running loop the tray icon would never
// register and menu clicks would never fire — the bus connection
// would be inert.
//
// AETHER_UI_HEADLESS=1: skip the loop (CI doesn't need D-Bus
// signal delivery — the AetherUIDriver routes drive everything
// directly through the registry). Fall through to the shared park
// helper so the process stays alive for the HTTP server thread.
void aether_ui_app_run_headless_impl(void) {
    if (aeui_is_headless()) {
        aether_ui_park_until_killed();
        return;
    }
    ensure_gtk_init();
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}
