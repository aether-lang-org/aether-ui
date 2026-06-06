// Aether UI Backend ABI
//
// This header declares the cross-platform widget API that every Aether UI
// backend must implement. The three backends are:
//
//   aether_ui_gtk4.c   — Linux, backed by GTK4
//   aether_ui_macos.m  — macOS, backed by AppKit
//   aether_ui_win32.c  — Windows, backed by USER32+GDI+
//
// The Aether DSL layer (module.ae) declares matching
// `extern` functions and is platform-neutral. Build-time backend selection
// happens in build.sh based on `uname -s`.
//
// Cross-platform test server (AetherUIDriver) lives in aether_ui_test_server.c
// and is linked into every backend.

#ifndef AETHER_UI_BACKEND_H
#define AETHER_UI_BACKEND_H

#include <stdint.h>

// Widget registry
int aether_ui_register_widget(void* widget);
void* aether_ui_get_widget(int handle);
// Reverse lookup: native-pointer → 1-based handle, 0 if not registered.
// O(1) amortized on Win32 (hash-backed); may be linear on other backends.
int aether_ui_handle_for_widget(void* widget);

// App lifecycle
int aether_ui_app_create(const char* title, int width, int height);
void aether_ui_app_set_body(int app_handle, int root_handle);
void aether_ui_app_run_raw(int app_handle);

// Surfaces — "DSL with Scope" destinations (window / render_to / record).
// container_new(kind) makes a detached content container (pushed as _ctx so
// children attach to it) — used as the zero-arg `with` factory for every
// surface kind. surface_run is the `window` builder body: creates the app,
// mounts the populated container, runs the loop (window only). The
// interactive/diag helpers track interactive verbs used on bounded surfaces.
int aether_ui_surface_container_new_impl(int kind);
void aether_ui_surface_run_impl(int container_handle,
                                const char* title, int width, int height);
int aether_ui_surface_note_interactive_impl(int container_handle);
int aether_ui_surface_diag_count_impl(int container_handle);

// Widget creation
int aether_ui_text_create(const char* text);
int aether_ui_button_create(const char* label, void* boxed_closure);
int aether_ui_button_create_plain(const char* label);
void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure);
int aether_ui_vstack_create(int spacing);
int aether_ui_hstack_create(int spacing);
int aether_ui_spacer_create(void);
int aether_ui_divider_create(void);

// Input widgets (Group 2)
int aether_ui_textfield_create(const char* placeholder, void* boxed_closure);
void aether_ui_textfield_set_text(int handle, const char* text);
const char* aether_ui_textfield_get_text(int handle);

int aether_ui_securefield_create(const char* placeholder, void* boxed_closure);

int aether_ui_toggle_create(const char* label, void* boxed_closure);
void aether_ui_toggle_set_active(int handle, int active);
int aether_ui_toggle_get_active(int handle);

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure);
void aether_ui_slider_set_value(int handle, double value);
double aether_ui_slider_get_value(int handle);

int aether_ui_picker_create(void* boxed_closure);
void aether_ui_picker_add_item(int handle, const char* item);
void aether_ui_picker_set_selected(int handle, int index);
int aether_ui_picker_get_selected(int handle);

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure);
void aether_ui_textarea_set_text(int handle, const char* text);
char* aether_ui_textarea_get_text(int handle);

int aether_ui_scrollview_create(void);
int aether_ui_progressbar_create(double fraction);
void aether_ui_progressbar_set_fraction(int handle, double fraction);

// Layout containers (Group 3)
int aether_ui_zstack_create(void);
int aether_ui_form_create(void);
int aether_ui_form_section_create(const char* title);
int aether_ui_navstack_create(void);
void aether_ui_navstack_push(int handle, const char* title, int body_handle);
void aether_ui_navstack_pop(int handle);

// Styling (Group 4)
void aether_ui_set_bg_color(int handle, double r, double g, double b, double a);
void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical);
void aether_ui_set_text_color(int handle, double r, double g, double b);
void aether_ui_set_font_size(int handle, double size);
void aether_ui_set_font_bold(int handle, int bold);
void aether_ui_set_corner_radius(int handle, double radius);
void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left);
void aether_ui_set_width(int handle, int width);
void aether_ui_set_height(int handle, int height);
void aether_ui_set_opacity(int handle, double opacity);
void aether_ui_set_enabled(int handle, int enabled);
void aether_ui_set_tooltip(int handle, const char* text);
void aether_ui_set_distribution(int handle, int distribution);
void aether_ui_set_alignment(int handle, int alignment);
void aether_ui_match_parent_width(int handle);
void aether_ui_match_parent_height(int handle);
void aether_ui_set_margin(int handle, int top, int right, int bottom, int left);
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left);
void aether_ui_enable_test_server_ctx(int port, void* ctx);

// System integration (Group 5)
void aether_ui_alert_impl(const char* title, const char* message);
char* aether_ui_file_open(const char* title);
void aether_ui_clipboard_write_impl(const char* text);
int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure);
void aether_ui_timer_cancel_impl(int timer_id);
void aether_ui_open_url_impl(const char* url);
int aether_ui_dark_mode_check(void);

int aether_ui_window_create_impl(const char* title, int width, int height);
void aether_ui_window_set_body_impl(int win_handle, int root_handle);
void aether_ui_window_show_impl(int win_handle);
void aether_ui_window_close_impl(int win_handle);

int aether_ui_sheet_create_impl(const char* title, int width, int height);
void aether_ui_sheet_set_body_impl(int handle, int root_handle);
void aether_ui_sheet_present_impl(int handle);
void aether_ui_sheet_dismiss_impl(int handle);

int aether_ui_image_create(const char* filepath);
void aether_ui_image_set_size(int handle, int width, int height);

// Menus (Group 5b) — native menu bars and context menus.
// Backend-implemented on Win32 (HMENU), GTK (GMenu), AppKit (NSMenu).
int  aether_ui_menu_bar_create(void);
int  aether_ui_menu_create(const char* label);
void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure);
void aether_ui_menu_add_separator(int menu_handle);
void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle);
void aether_ui_menu_bar_attach(int app_handle, int bar_handle);
// Context menu: popup a menu at cursor / widget bounds.
void aether_ui_menu_popup(int menu_handle, int anchor_widget);

// Grid layout (Group 3b) — 2D layout container.
// Children are placed with aether_ui_grid_place() at (row, col) with
// optional row/col spans. Unlike stacks, columns align across rows so
// labels-on-left / fields-on-right forms actually line up.
int  aether_ui_grid_create(int cols, int row_spacing, int col_spacing);
void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span);

// Canvas drawing (Group 6)
int aether_ui_canvas_create_impl(int width, int height);
int aether_ui_canvas_get_widget(int canvas_id);
void aether_ui_canvas_begin_path_impl(int canvas_id);
void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y);
void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y);
void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b,
                             double a, double line_width);
void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y,
                                double w, double h,
                                double r, double g, double b, double a);
void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                                double start_angle, double end_angle);
void aether_ui_canvas_close_path_impl(int canvas_id);
void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a);
void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text,
                                      double x, double y, double font_size,
                                      double r, double g, double b, double a);
void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len);
void aether_ui_canvas_fill_linear_gradient_impl(int canvas_id,
        double x1, double y1, double x2, double y2,
        int n_stops, void* offsets, void* rgba, double line_width);
void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width);
void aether_ui_canvas_clear_impl(int canvas_id);
void aether_ui_canvas_redraw_impl(int canvas_id);
// Off-screen render of the canvas command buffer to a PNG. Headless-capable
// (pure cairo image surface; no window). Returns 1 on success, 0 on failure.
int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height);

// Events
void aether_ui_on_hover_impl(int handle, void* boxed_closure);
void aether_ui_on_double_click_impl(int handle, void* boxed_closure);
void aether_ui_on_click_impl(int handle, void* boxed_closure);

// Animation
void aether_ui_animate_opacity_impl(int handle, double target, int duration_ms);

// Widget manipulation
void aether_ui_remove_child_impl(int parent_handle, int child_handle);
void aether_ui_clear_children_impl(int handle);

// AetherUIDriver
void aether_ui_enable_test_server_impl(int port, int root_handle);
void aether_ui_seal_widget_impl(int handle);
void aether_ui_seal_subtree_impl(int handle);

// System tray (Group 7) — OS-level status-area icon with optional
// tooltip, popup menu, and reactive icon swap. Phase 1 unblocks
// tray-only apps (AvnSync v2 etc). Backend storage + driver
// dispatch live in aether_ui_system_extras.c; per-backend native
// wiring (libayatana-appindicator / NSStatusItem / Shell_NotifyIcon)
// calls into the shared registry on click/menu-activate events.
int  aether_ui_tray_create_impl(const char* name, void* boxed_left_click);
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text);
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle);
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle,
                                             const char* icon_clean,
                                             const char* icon_busy,
                                             const char* icon_alert);
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template);
void aether_ui_tray_seal_impl(int tray_id);

// Desktop notifications (Group 7b) — fire-and-forget OS notifications.
// Click callback (notify_full) marshals back to the main thread before
// firing the Aether closure.
int  aether_ui_notify_impl(const char* title, const char* body);
int  aether_ui_notify_full_impl(const char* title, const char* body,
                                 const char* icon_path, const char* tag,
                                 void* boxed_click);
int  aether_ui_notify_request_permission_impl(void);

// Widget tree
void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle);
void aether_ui_widget_set_hidden(int handle, int hidden);

// Text mutation
void aether_ui_text_set_string(int handle, const char* text);

// Reactive state
int aether_ui_state_create(double initial);
double aether_ui_state_get(int handle);
void aether_ui_state_set(int handle, double value);
void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix);

#endif
