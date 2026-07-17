// Aether UI — Win32 backend
//
// Native Windows implementation of the backend ABI declared in
// aether_ui_backend.h. Uses USER32 (windows + controls), GDI+ (custom paint),
// COMCTL32 (slider, progressbar, tooltip), COMDLG32 (file dialogs),
// DWMAPI (dark mode, accent color), SHELL32 (open URL), WS2_32 (test server).
//
// Toolchain: MinGW-w64 (gcc). No MSVC-specific extensions — only POSIX-ish C
// plus Win32 headers. Wide-char APIs are used throughout; UTF-8 conversion
// happens at the API boundary.
//
// This file is paired with aether_ui_backend.h. The Aether DSL layer
// (module.ae) declares matching externs.

#include "aether_ui_backend.h"
#include "aether_ui_system_extras.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Windows 10 1809+ for immersive dark mode, per-monitor DPI v2.
// WINVER + _WIN32_WINNT + NTDDI_VERSION must all be set for MinGW's headers
// to expose GetDpiForSystem / AdjustWindowRectExForDpi / DwmSetWindowAttribute.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000007  /* NTDDI_WIN10_RS3 — covers per-monitor-v2 APIs */
#endif

// Winsock must be included before windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shlobj.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

// Library linkage is declared in build.sh (MinGW does not honor #pragma comment).
// Required libs: user32, gdi32, comctl32, comdlg32, shell32, dwmapi, uxtheme,
// ole32, uuid, ws2_32.

// ---------------------------------------------------------------------------
// Closure struct — must match Aether codegen's _AeClosure layout.
// ---------------------------------------------------------------------------
typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

static inline void invoke_closure(AeClosure* c) {
    if (c && c->fn) ((void (*)(void*))c->fn)(c->env);
}

// ---------------------------------------------------------------------------
// AETHER_UI_HEADLESS contract — set by CI, widget smoke tests, or any
// caller that wants to exercise the backend without a user. Every API
// that would otherwise run a modal message loop (MessageBox,
// TrackPopupMenu, GetOpenFileName, GetSaveFileName) returns without
// showing UI when this flag is set. Without this, those APIs block the
// calling thread forever — there is no user input on CI and no outer
// message pump to dismiss.
// ---------------------------------------------------------------------------
static int aeui_is_headless(void) {
    const char* v = getenv("AETHER_UI_HEADLESS");
    return v && v[0] && v[0] != '0';
}

// ---------------------------------------------------------------------------
// UTF-8 ↔ UTF-16 helpers.
//
// Windows uses UTF-16 internally (`wchar_t`). Aether uses UTF-8 (`char*`).
// Converted strings live in static rotating buffers to avoid caller cleanup.
// Callers may hold the result across one call, not indefinitely.
// ---------------------------------------------------------------------------
#define UTF_BUFS 8
#define UTF_BUF_SIZE 4096

static wchar_t* utf8_to_wide(const char* s) {
    static wchar_t bufs[UTF_BUFS][UTF_BUF_SIZE];
    static int idx = 0;
    if (!s) s = "";
    wchar_t* buf = bufs[idx];
    idx = (idx + 1) % UTF_BUFS;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, UTF_BUF_SIZE);
    if (n <= 0) buf[0] = L'\0';
    return buf;
}

static char* wide_to_utf8(const wchar_t* s) {
    static char bufs[UTF_BUFS][UTF_BUF_SIZE];
    static int idx = 0;
    if (!s) s = L"";
    char* buf = bufs[idx];
    idx = (idx + 1) % UTF_BUFS;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, UTF_BUF_SIZE, NULL, NULL);
    if (n <= 0) buf[0] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// Widget kind enum + metadata.
//
// Every HWND we create is tagged with a kind + per-widget metadata (style
// overrides, attached closures, child-layout info for stacks). Stored in the
// global widgets[] array so the test-server and styling APIs can introspect.
// ---------------------------------------------------------------------------
typedef enum {
    WK_NULL = 0,
    WK_TEXT,
    WK_BUTTON,
    WK_TEXTFIELD,
    WK_SECUREFIELD,
    WK_TEXTAREA,
    WK_TOGGLE,
    WK_SLIDER,
    WK_PICKER,
    WK_PROGRESSBAR,
    WK_IMAGE,
    WK_VSTACK,
    WK_HSTACK,
    WK_ZSTACK,
    WK_FORM,
    WK_FORM_SECTION,
    WK_NAVSTACK,
    WK_SCROLLVIEW,
    WK_SPACER,
    WK_DIVIDER,
    WK_CANVAS,
    WK_WINDOW,
    WK_SHEET,
    WK_GRID,
    WK_SCRIM,     // overlay modal scrim (full-client click-eater)
} WidgetKind;

typedef struct {
    COLORREF color;
    int has_value;
} ColorOverride;

typedef struct {
    // 0 = horizontal (HStack), 1 = vertical (VStack), 2 = ZStack
    int orientation;
    int spacing;
    int padding_top, padding_right, padding_bottom, padding_left;
    int alignment;    // 0=start, 1=center, 2=end
    int distribution; // 0=fill, 1=equal, 2=trailing
} StackLayout;

typedef struct {
    WidgetKind kind;
    HWND hwnd;
    HWND parent_hwnd;

    // Appearance overrides (applied via subclass paint hooks)
    ColorOverride bg;
    ColorOverride fg;
    int gradient_enabled;
    COLORREF grad_a, grad_b;
    int grad_vertical;
    int corner_radius;
    double opacity; // 0.0–1.0; <0 = no override

    // Fonts
    HFONT custom_font;
    double font_size;
    int font_bold;

    // Fixed sizing (0 = auto)
    int pref_width;
    int pref_height;
    int min_width;
    int min_height;

    // Margins (apply during parent layout)
    int margin_top, margin_right, margin_bottom, margin_left;

    // Stack container layout
    StackLayout stack;

    // Attached event closures
    AeClosure* on_click;
    AeClosure* on_hover;
    AeClosure* on_double_click;
    AeClosure* on_change; // text/value change for input widgets

    // Per-widget data (union over kind)
    union {
        struct { int timer_id; } button;
        struct { int canvas_id; } canvas;
        struct { double min_v, max_v, cur_v; } slider;
        struct { double fraction; } progressbar;
    } u;

    // Tooltip text (owned)
    wchar_t* tooltip;

    // Sealed flag (test server)
    int sealed;

    // CSS-class mirror (item 4/8 parity): win32 has no CSS, but the class
    // LIST is the driver's selection-visibility contract (.aui-row-selected)
    // — tracked here, emitted in widget JSON. Space-separated, owned.
    char* classes;
} Widget;

static Widget** widgets = NULL;
static int widget_count = 0;
static int widget_capacity = 0;

// Open-addressed reverse map HWND → 1-based handle.
//
// Before this, handle_for_hwnd() was an O(n) linear scan called from every
// WM_COMMAND, every stack layout, and every /widgets JSON emit — so a
// 500-widget app paid ~500 pointer compares per button click. Replacing it
// with a flat probe table keeps lookup O(1) amortized with predictable
// memory (8 bytes per live widget + load-factor slack).
//
// Keys are HWND (64-bit on x64); we use a 32-bit multiplicative Fibonacci
// hash and linear probing. Never shrinks — widgets are never unregistered
// in the current lifecycle, so stale tombstones aren't needed.
typedef struct { HWND hwnd; int handle; } WidgetHashEntry;
static WidgetHashEntry* widget_hash = NULL;
static int widget_hash_mask = 0;  // capacity - 1; capacity is always power of 2
static int widget_hash_count = 0;

static uint32_t hash_hwnd(HWND h) {
    // Fibonacci hash: multiply by the golden ratio constant, keep high bits.
    uint64_t k = (uint64_t)(uintptr_t)h;
    return (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 32);
}

static void widget_hash_grow(int new_cap_pow2) {
    WidgetHashEntry* old = widget_hash;
    int old_cap = widget_hash_mask + 1;
    widget_hash = (WidgetHashEntry*)calloc((size_t)new_cap_pow2,
                                            sizeof(WidgetHashEntry));
    widget_hash_mask = new_cap_pow2 - 1;
    widget_hash_count = 0;
    if (old) {
        for (int i = 0; i < old_cap; i++) {
            if (old[i].hwnd) {
                uint32_t slot = hash_hwnd(old[i].hwnd) & widget_hash_mask;
                while (widget_hash[slot].hwnd) slot = (slot + 1) & widget_hash_mask;
                widget_hash[slot] = old[i];
                widget_hash_count++;
            }
        }
        free(old);
    }
}

static void widget_hash_insert(HWND h, int handle) {
    if (!h) return;
    // Keep load factor < 0.5 for good probe distance. The `!widget_hash`
    // guard handles the initial empty-state correctly — otherwise the
    // `count * 2 >= mask + 1` arithmetic evaluates 0 >= 1 (false) and
    // the slot write dereferences a NULL table.
    if (!widget_hash || widget_hash_count * 2 >= (widget_hash_mask + 1)) {
        int new_cap = widget_hash ? (widget_hash_mask + 1) * 2 : 64;
        widget_hash_grow(new_cap);
    }
    uint32_t slot = hash_hwnd(h) & widget_hash_mask;
    while (widget_hash[slot].hwnd && widget_hash[slot].hwnd != h)
        slot = (slot + 1) & widget_hash_mask;
    if (!widget_hash[slot].hwnd) widget_hash_count++;
    widget_hash[slot].hwnd = h;
    widget_hash[slot].handle = handle;
}

static Widget* widget_at(int handle) {
    if (handle < 1 || handle > widget_count) return NULL;
    return widgets[handle - 1];
}

int aether_ui_register_widget(void* hwnd) {
    if (widget_count >= widget_capacity) {
        widget_capacity = widget_capacity == 0 ? 64 : widget_capacity * 2;
        widgets = (Widget**)realloc(widgets, sizeof(Widget*) * widget_capacity);
    }
    Widget* w = (Widget*)calloc(1, sizeof(Widget));
    w->hwnd = (HWND)hwnd;
    w->opacity = -1.0;
    w->font_size = 0.0;
    widgets[widget_count++] = w;
    widget_hash_insert((HWND)hwnd, widget_count);
    return widget_count; // 1-based
}

static int register_widget_typed(HWND hwnd, WidgetKind kind) {
    int h = aether_ui_register_widget(hwnd);
    if (h > 0) widgets[h - 1]->kind = kind;
    return h;
}

void* aether_ui_get_widget(int handle) {
    Widget* w = widget_at(handle);
    return w ? (void*)w->hwnd : NULL;
}

// O(1) average reverse lookup via the HWND hash. Falls back to linear scan
// for HWNDs that were never registered (the hash would miss anyway).
static int handle_for_hwnd(HWND h) {
    if (!h || !widget_hash) return 0;
    uint32_t slot = hash_hwnd(h) & widget_hash_mask;
    while (widget_hash[slot].hwnd) {
        if (widget_hash[slot].hwnd == h) return widget_hash[slot].handle;
        slot = (slot + 1) & widget_hash_mask;
    }
    return 0;
}

// Public wrapper matching the backend ABI.
int aether_ui_handle_for_widget(void* widget) {
    return handle_for_hwnd((HWND)widget);
}

// ---------------------------------------------------------------------------
// Reactive state — ported verbatim from the GTK4 backend (platform-neutral).
// ---------------------------------------------------------------------------
enum { AEUI_STATE_FLOAT = 0, AEUI_STATE_INT = 1,
       AEUI_STATE_BOOL = 2, AEUI_STATE_STRING = 3 };
typedef struct {
    int type;
    double num;   // float/int/bool payload
    char* str;    // string payload (owned)
} StateCell;

enum { AEUI_BIND_TEXT = 0, AEUI_BIND_ENABLED = 1, AEUI_BIND_HIDDEN = 2 };
typedef struct {
    int kind;
    int state_handle;
    int widget_handle;
    char* prefix;   // TEXT only
    char* suffix;   // TEXT only
    int decimals;   // TEXT + float cell: -1 = smart (int-valued → "%d")
    int invert;     // ENABLED/HIDDEN: apply the negation
} PropBinding;

static StateCell* state_cells = NULL;
static int state_count = 0;
static int state_capacity = 0;

static PropBinding* prop_bindings = NULL;
static int prop_binding_count = 0;
static int prop_binding_capacity = 0;

static StateCell* state_cell(int handle) {
    if (handle < 1 || handle > state_count) return NULL;
    return &state_cells[handle - 1];
}

static int state_create_cell(int type, double num, const char* str) {
    if (state_count >= state_capacity) {
        state_capacity = state_capacity == 0 ? 32 : state_capacity * 2;
        state_cells = realloc(state_cells, sizeof(StateCell) * state_capacity);
    }
    StateCell* c = &state_cells[state_count];
    c->type = type;
    c->num = num;
    c->str = str ? strdup(str) : NULL;
    state_count++;
    return state_count; // 1-based
}

int aether_ui_state_create(double initial) {
    return state_create_cell(AEUI_STATE_FLOAT, initial, NULL);
}
int aether_ui_state_create_s(const char* initial) {
    return state_create_cell(AEUI_STATE_STRING, 0.0, initial ? initial : "");
}
int aether_ui_state_create_i(int initial) {
    return state_create_cell(AEUI_STATE_INT, (double)initial, NULL);
}
int aether_ui_state_create_b(int initial) {
    return state_create_cell(AEUI_STATE_BOOL, initial ? 1.0 : 0.0, NULL);
}

double aether_ui_state_get(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_FLOAT) ? c->num : 0.0;
}
// Returned string is malloc'd (Aether externs own their string returns).
const char* aether_ui_state_get_s(int handle) {
    StateCell* c = state_cell(handle);
    return strdup((c && c->type == AEUI_STATE_STRING && c->str) ? c->str : "");
}
int aether_ui_state_get_i(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_INT) ? (int)c->num : 0;
}
int aether_ui_state_get_b(int handle) {
    StateCell* c = state_cell(handle);
    return (c && c->type == AEUI_STATE_BOOL) ? (c->num != 0.0) : 0;
}

int aether_ui_state_type(int handle) {
    StateCell* c = state_cell(handle);
    return c ? c->type : -1;
}

// Render a cell's display string (no prefix/suffix) into buf.
static void state_render_value(StateCell* c, int decimals, char* buf, int n) {
    if (!c) { buf[0] = '\0'; return; }
    switch (c->type) {
        case AEUI_STATE_STRING:
            snprintf(buf, n, "%s", c->str ? c->str : "");
            break;
        case AEUI_STATE_INT:
            snprintf(buf, n, "%d", (int)c->num);
            break;
        case AEUI_STATE_BOOL:
            snprintf(buf, n, "%s", c->num != 0.0 ? "true" : "false");
            break;
        default: // float
            if (decimals >= 0) {
                snprintf(buf, n, "%.*f", decimals, c->num);
            } else if (c->num == (int)c->num) {
                snprintf(buf, n, "%d", (int)c->num);
            } else {
                snprintf(buf, n, "%.2f", c->num);
            }
    }
}

static int state_truthy(StateCell* c) {
    if (!c) return 0;
    if (c->type == AEUI_STATE_STRING) return c->str && c->str[0];
    return c->num != 0.0;
}

static void apply_prop_binding(PropBinding* b) {
    StateCell* c = state_cell(b->state_handle);
    if (!c) return;
    if (b->kind == AEUI_BIND_TEXT) {
        char val[256];
        state_render_value(c, b->decimals, val, sizeof(val));
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s%s", b->prefix, val, b->suffix);
        aether_ui_text_set_string(b->widget_handle, buf);
    } else {
        int on = state_truthy(c);
        if (b->invert) on = !on;
        if (b->kind == AEUI_BIND_ENABLED) {
            aether_ui_set_enabled(b->widget_handle, on);
        } else { // HIDDEN: state truthy (post-invert) = hidden
            aether_ui_widget_set_hidden(b->widget_handle, on);
        }
    }
}

static void update_prop_bindings(int state_handle) {
    for (int i = 0; i < prop_binding_count; i++) {
        if (prop_bindings[i].state_handle == state_handle) {
            apply_prop_binding(&prop_bindings[i]);
        }
    }
}

void aether_ui_state_set(int handle, double value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_FLOAT) return;
    c->num = value;
    update_prop_bindings(handle);
}
void aether_ui_state_set_s(int handle, const char* value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_STRING) return;
    free(c->str);
    c->str = strdup(value ? value : "");
    update_prop_bindings(handle);
}
void aether_ui_state_set_i(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_INT) return;
    c->num = (double)value;
    update_prop_bindings(handle);
}
void aether_ui_state_set_b(int handle, int value) {
    StateCell* c = state_cell(handle);
    if (!c || c->type != AEUI_STATE_BOOL) return;
    c->num = value ? 1.0 : 0.0;
    update_prop_bindings(handle);
}

static PropBinding* prop_binding_new(int kind, int state_handle, int widget_handle) {
    if (prop_binding_count >= prop_binding_capacity) {
        prop_binding_capacity = prop_binding_capacity == 0 ? 32 : prop_binding_capacity * 2;
        prop_bindings = realloc(prop_bindings,
                                sizeof(PropBinding) * prop_binding_capacity);
    }
    PropBinding* b = &prop_bindings[prop_binding_count++];
    b->kind = kind;
    b->state_handle = state_handle;
    b->widget_handle = widget_handle;
    b->prefix = strdup("");
    b->suffix = strdup("");
    b->decimals = -1;
    b->invert = 0;
    return b;
}

// The original float text binding — now literally a TEXT PropBinding.
void aether_ui_state_bind_text(int state_handle, int text_handle,
                               const char* prefix, const char* suffix) {
    PropBinding* b = prop_binding_new(AEUI_BIND_TEXT, state_handle, text_handle);
    free(b->prefix);
    free(b->suffix);
    b->prefix = prefix ? strdup(prefix) : strdup("");
    b->suffix = suffix ? strdup(suffix) : strdup("");
    apply_prop_binding(b);
}

void aether_ui_bind_text_impl(int state_handle, int widget_handle, int decimals) {
    PropBinding* b = prop_binding_new(AEUI_BIND_TEXT, state_handle, widget_handle);
    b->decimals = decimals;
    apply_prop_binding(b);
}
void aether_ui_bind_enabled_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = prop_binding_new(AEUI_BIND_ENABLED, state_handle, widget_handle);
    b->invert = invert;
    apply_prop_binding(b);
}
void aether_ui_bind_hidden_impl(int state_handle, int widget_handle, int invert) {
    PropBinding* b = prop_binding_new(AEUI_BIND_HIDDEN, state_handle, widget_handle);
    b->invert = invert;
    apply_prop_binding(b);
}

// ---------------------------------------------------------------------------
// Stack container — custom window class.
//
// A container that lays out its children left-to-right (HStack), top-to-bottom
// (VStack), or over-each-other (ZStack). Mirrors the semantics of GtkBox and
// NSStackView. Handles WM_SIZE by querying each child's preferred size and
// laying them out with spacing + padding.
//
// Children that are themselves stacks are recursed. Spacers consume leftover
// flex space along the primary axis.
// ---------------------------------------------------------------------------
static const wchar_t* STACK_CLASS = L"AetherUIStack";

typedef struct {
    int measured_w;
    int measured_h;
    int is_spacer;
    int margin_t, margin_r, margin_b, margin_l;
} MeasuredChild;

// Measure a single widget's intrinsic size. STATIC/BUTTON use a minimal
// heuristic (text extents + padding); custom widgets honor pref_width/height.
static void measure_widget(Widget* w, int* out_w, int* out_h) {
    if (w->pref_width > 0 && w->pref_height > 0) {
        *out_w = w->pref_width;
        *out_h = w->pref_height;
        return;
    }
    RECT r;
    if (GetWindowRect(w->hwnd, &r)) {
        int cur_w = r.right - r.left;
        int cur_h = r.bottom - r.top;
        // Ask the control for its natural size via GetTextExtentPoint where it
        // makes sense, else fall back to current size.
        if (w->kind == WK_TEXT || w->kind == WK_BUTTON) {
            HDC hdc = GetDC(w->hwnd);
            HFONT font = w->custom_font ? w->custom_font
                         : (HFONT)SendMessageW(w->hwnd, WM_GETFONT, 0, 0);
            HFONT old = font ? (HFONT)SelectObject(hdc, font) : NULL;
            wchar_t text[1024];
            int tlen = GetWindowTextW(w->hwnd, text, 1024);
            SIZE sz;
            GetTextExtentPoint32W(hdc, text, tlen, &sz);
            if (old) SelectObject(hdc, old);
            ReleaseDC(w->hwnd, hdc);
            int pad_x = w->kind == WK_BUTTON ? 24 : 4;
            int pad_y = w->kind == WK_BUTTON ? 10 : 4;
            *out_w = sz.cx + pad_x;
            *out_h = sz.cy + pad_y;
            if (w->pref_width > 0) *out_w = w->pref_width;
            if (w->pref_height > 0) *out_h = w->pref_height;
            return;
        }
        *out_w = w->pref_width > 0 ? w->pref_width : cur_w;
        *out_h = w->pref_height > 0 ? w->pref_height : cur_h;
        return;
    }
    *out_w = w->pref_width > 0 ? w->pref_width : 100;
    *out_h = w->pref_height > 0 ? w->pref_height : 24;
}

// Layout all direct child windows of the stack.
static void stack_do_layout(HWND stack_hwnd) {
    int h = handle_for_hwnd(stack_hwnd);
    if (h == 0) return;
    Widget* sw = widget_at(h);
    if (!sw) return;
    StackLayout* sl = &sw->stack;
    int orientation = sl->orientation;

    RECT client;
    GetClientRect(stack_hwnd, &client);
    int avail_w = (client.right - client.left) - sl->padding_left - sl->padding_right;
    int avail_h = (client.bottom - client.top) - sl->padding_top - sl->padding_bottom;

    // Collect children in z-order.
    HWND* children = NULL;
    int nchildren = 0, cap = 0;
    for (HWND c = GetWindow(stack_hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (nchildren >= cap) {
            cap = cap == 0 ? 16 : cap * 2;
            children = (HWND*)realloc(children, sizeof(HWND) * cap);
        }
        children[nchildren++] = c;
    }
    if (nchildren == 0) { free(children); return; }

    // ZStack: overlay every child filling the client area.
    if (orientation == 2) {
        for (int i = 0; i < nchildren; i++) {
            SetWindowPos(children[i], NULL,
                         sl->padding_left, sl->padding_top,
                         avail_w, avail_h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        free(children);
        return;
    }

    // Measure + identify spacers.
    MeasuredChild* mc = (MeasuredChild*)calloc(nchildren, sizeof(MeasuredChild));
    int total_primary = 0;
    int spacer_count = 0;
    for (int i = 0; i < nchildren; i++) {
        int ch = handle_for_hwnd(children[i]);
        Widget* cw = widget_at(ch);
        if (cw && cw->kind == WK_SPACER) {
            mc[i].is_spacer = 1;
            spacer_count++;
            continue;
        }
        int cw_w = 0, ch_h = 0;
        if (cw) {
            measure_widget(cw, &cw_w, &ch_h);
            mc[i].margin_t = cw->margin_top;
            mc[i].margin_r = cw->margin_right;
            mc[i].margin_b = cw->margin_bottom;
            mc[i].margin_l = cw->margin_left;
        } else {
            cw_w = 100; ch_h = 24;
        }
        mc[i].measured_w = cw_w;
        mc[i].measured_h = ch_h;
        total_primary += (orientation == 1) ? (ch_h + mc[i].margin_t + mc[i].margin_b)
                                            : (cw_w + mc[i].margin_l + mc[i].margin_r);
    }
    int spacing_total = sl->spacing * (nchildren - 1);
    int primary_avail = (orientation == 1) ? avail_h : avail_w;
    int flex = primary_avail - total_primary - spacing_total;
    if (flex < 0) flex = 0;
    int per_spacer = spacer_count > 0 ? (flex / spacer_count) : 0;

    // Lay out.
    int cur = (orientation == 1) ? sl->padding_top : sl->padding_left;
    for (int i = 0; i < nchildren; i++) {
        int x, y, w, h;
        if (orientation == 1) { // VStack
            int ch_size = mc[i].is_spacer ? per_spacer : mc[i].measured_h;
            h = ch_size;
            w = avail_w - mc[i].margin_l - mc[i].margin_r;
            if (mc[i].measured_w > 0 && mc[i].measured_w < w && !mc[i].is_spacer) {
                if (sl->alignment == 1)
                    x = sl->padding_left + mc[i].margin_l + (w - mc[i].measured_w) / 2;
                else if (sl->alignment == 2)
                    x = sl->padding_left + mc[i].margin_l + (w - mc[i].measured_w);
                else
                    x = sl->padding_left + mc[i].margin_l;
                w = mc[i].measured_w;
            } else {
                x = sl->padding_left + mc[i].margin_l;
            }
            y = cur + mc[i].margin_t;
            cur = y + h + mc[i].margin_b + sl->spacing;
        } else { // HStack
            int ch_size = mc[i].is_spacer ? per_spacer : mc[i].measured_w;
            w = ch_size;
            h = avail_h - mc[i].margin_t - mc[i].margin_b;
            if (mc[i].measured_h > 0 && mc[i].measured_h < h && !mc[i].is_spacer) {
                if (sl->alignment == 1)
                    y = sl->padding_top + mc[i].margin_t + (h - mc[i].measured_h) / 2;
                else if (sl->alignment == 2)
                    y = sl->padding_top + mc[i].margin_t + (h - mc[i].measured_h);
                else
                    y = sl->padding_top + mc[i].margin_t;
                h = mc[i].measured_h;
            } else {
                y = sl->padding_top + mc[i].margin_t;
            }
            x = cur + mc[i].margin_l;
            cur = x + w + mc[i].margin_r + sl->spacing;
        }
        SetWindowPos(children[i], NULL, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    free(mc);
    free(children);
}

static LRESULT CALLBACK stack_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            stack_do_layout(hwnd);
            return 0;

        case WM_ERASEBKGND: {
            int h = handle_for_hwnd(hwnd);
            Widget* w = widget_at(h);
            if (w && w->bg.has_value) {
                HDC hdc = (HDC)wp;
                RECT r;
                GetClientRect(hwnd, &r);
                HBRUSH br = CreateSolidBrush(w->bg.color);
                FillRect(hdc, &r, br);
                DeleteObject(br);
                return 1;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_COMMAND: {
            // Forward control notifications from children to their registered
            // on_click closures (buttons, checkboxes).
            HWND child = (HWND)lp;
            WORD code = HIWORD(wp);
            int ch = handle_for_hwnd(child);
            Widget* cw = widget_at(ch);
            if (cw) {
                if (cw->kind == WK_BUTTON && (code == BN_CLICKED || code == 0)) {
                    if (!cw->sealed) invoke_closure(cw->on_click);
                } else if (cw->kind == WK_TOGGLE && code == BN_CLICKED) {
                    if (!cw->sealed) invoke_closure(cw->on_change);
                } else if ((cw->kind == WK_TEXTFIELD || cw->kind == WK_SECUREFIELD
                            || cw->kind == WK_TEXTAREA) && code == EN_CHANGE) {
                    if (!cw->sealed) invoke_closure(cw->on_change);
                } else if (cw->kind == WK_PICKER && code == CBN_SELCHANGE) {
                    if (!cw->sealed) invoke_closure(cw->on_change);
                }
            }
            return 0;
        }

        case WM_HSCROLL:
        case WM_VSCROLL: {
            HWND child = (HWND)lp;
            int ch = handle_for_hwnd(child);
            Widget* cw = widget_at(ch);
            if (cw && cw->kind == WK_SLIDER) {
                int pos = (int)SendMessageW(child, TBM_GETPOS, 0, 0);
                // Map pos back to the slider's min/max range.
                double min_v = cw->u.slider.min_v;
                double max_v = cw->u.slider.max_v;
                double val = min_v + (max_v - min_v) * (pos / 1000.0);
                cw->u.slider.cur_v = val;
                if (!cw->sealed) invoke_closure(cw->on_change);
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            HWND child = (HWND)lp;
            HDC hdc = (HDC)wp;
            int ch = handle_for_hwnd(child);
            Widget* cw = widget_at(ch);
            if (cw) {
                if (cw->fg.has_value) SetTextColor(hdc, cw->fg.color);
                if (cw->bg.has_value) {
                    SetBkColor(hdc, cw->bg.color);
                    // Keep a cached brush per-widget (leaked for now; cleaned
                    // in WM_DESTROY via DeleteObject for widgets we know of).
                    static HBRUSH last_brush = NULL;
                    static COLORREF last_color = 0;
                    if (last_brush && last_color != cw->bg.color) {
                        DeleteObject(last_brush);
                        last_brush = NULL;
                    }
                    if (!last_brush) {
                        last_brush = CreateSolidBrush(cw->bg.color);
                        last_color = cw->bg.color;
                    }
                    return (LRESULT)last_brush;
                }
                SetBkMode(hdc, TRANSPARENT);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Main app window class + DPI awareness.
// ---------------------------------------------------------------------------
static const wchar_t* APP_CLASS = L"AetherUIAppWindow";
static const wchar_t* DIVIDER_CLASS = L"AetherUIDivider";
static const wchar_t* SPACER_CLASS = L"AetherUISpacer";
static const wchar_t* SCRIM_CLASS = L"AetherUIScrim";
static LRESULT CALLBACK scrim_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static const wchar_t* CANVAS_CLASS = L"AetherUICanvas";
static int win_classes_registered = 0;
static int gdiplus_started = 0;
static ULONG_PTR gdiplus_token = 0;

// GDI+ flat API — declared here to avoid pulling in <gdiplus.h> (C++ only).
typedef struct {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

__declspec(dllimport) int __stdcall GdiplusStartup(ULONG_PTR* token,
    const GdiplusStartupInput* input, void* output);
__declspec(dllimport) void __stdcall GdiplusShutdown(ULONG_PTR token);

static void ensure_gdiplus(void) {
    if (gdiplus_started) return;
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&gdiplus_token, &in, NULL) == 0) gdiplus_started = 1;
}

static LRESULT CALLBACK divider_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        int my = r.top + (r.bottom - r.top) / 2;
        MoveToEx(hdc, r.left, my, NULL);
        LineTo(hdc, r.right, my);
        SelectObject(hdc, old);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK spacer_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Canvas drawing backend lives farther down; the window proc forwards to it.
static LRESULT CALLBACK canvas_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK grid_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static int menu_dispatch_command(UINT id);

// Menu command IDs start here to avoid collision with button control IDs.
#define AE_MENU_ID_BASE 0x8000

// Window class name for grid containers; the class is registered in
// register_window_classes() alongside STACK / DIVIDER / SPACER / CANVAS.
static const wchar_t* GRID_CLASS = L"AetherUIGrid";

static LRESULT CALLBACK app_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            // Resize the single root child to fill the client area.
            HWND child = GetWindow(hwnd, GW_CHILD);
            if (child) {
                RECT r;
                GetClientRect(hwnd, &r);
                SetWindowPos(child, NULL, 0, 0,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            RECT* suggested = (RECT*)lp;
            SetWindowPos(hwnd, NULL,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_COMMAND: {
            // WM_COMMAND with a menu ID (no control HWND) → look up the
            // registered closure. Otherwise forward to the stack proc,
            // which handles WM_COMMAND from child controls.
            WORD id = LOWORD(wp);
            if (lp == 0 && HIWORD(wp) == 0 && id >= AE_MENU_ID_BASE) {
                if (menu_dispatch_command(id)) return 0;
            }
            return stack_wnd_proc(hwnd, msg, wp, lp);
        }
        case WM_HSCROLL:
        case WM_VSCROLL:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            // App window forwards control notifications to the stack proc
            // by calling it directly; the root widget is always a stack.
            return stack_wnd_proc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_window_classes(HINSTANCE inst) {
    if (win_classes_registered) return;
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = app_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = stack_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = STACK_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = divider_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = DIVIDER_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = spacer_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = SPACER_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = canvas_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = CANVAS_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = grid_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = GRID_CLASS;
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = scrim_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = SCRIM_CLASS;
    RegisterClassExW(&wc);

    win_classes_registered = 1;
}

// DPI awareness setup — try the newest API first, fall back for older Windows.
typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);

static void init_dpi_awareness(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        FARPROC raw = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        SetProcessDpiAwarenessContextFn fn = (SetProcessDpiAwarenessContextFn)(void(*)(void))raw;
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    // Fallback: per-monitor aware (Windows 8.1+)
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessFn)(int);
        FARPROC raw = GetProcAddress(shcore, "SetProcessDpiAwareness");
        SetProcessDpiAwarenessFn fn = (SetProcessDpiAwarenessFn)(void(*)(void))raw;
        if (fn) fn(2);
        FreeLibrary(shcore);
    }
}

// Hidden holder window — serves as the initial parent for newly-created
// widgets. Win32 refuses to create WS_CHILD windows with a NULL parent, so
// every widget starts life parented here; it is reparented to its real
// container by aether_ui_widget_add_child_ctx / aether_ui_app_set_body.
static HWND widget_holder = NULL;

// Canvas keyboard-focus helpers (defined with the canvas globals below;
// used by the app-run/show path above them).
static void aeui_focus_pending_key_canvas(void);
static int  aeui_hwnd_is_key_canvas(HWND hwnd);

static int init_done = 0;
static void ensure_win_init(void) {
    if (init_done) return;
    init_done = 1;
    init_dpi_awareness();
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS
            | ICC_DATE_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    ensure_gdiplus();
    register_window_classes(GetModuleHandleW(NULL));
    // Create a hidden, message-only-ish holder. Not WS_OVERLAPPEDWINDOW
    // (that would show) — a plain popup window with no WS_VISIBLE.
    widget_holder = CreateWindowExW(
        0, STACK_CLASS, L"AetherUIHolder",
        WS_POPUP, 0, 0, 0, 0,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
typedef struct {
    wchar_t* title;
    int width;
    int height;
    int root_handle;
    HWND hwnd;
    HMENU pending_menu;  // menu bar to SetMenu() on this app's window
} AppEntry;

static AppEntry* apps = NULL;
static int app_count = 0;
static int app_capacity = 0;

int aether_ui_app_create(const char* title, int width, int height) {
    ensure_win_init();
    if (app_count >= app_capacity) {
        app_capacity = app_capacity == 0 ? 4 : app_capacity * 2;
        apps = (AppEntry*)realloc(apps, sizeof(AppEntry) * app_capacity);
    }
    AppEntry* e = &apps[app_count];
    e->title = _wcsdup(utf8_to_wide(title));
    e->width = width;
    e->height = height;
    e->root_handle = 0;
    e->hwnd = NULL;
    e->pending_menu = NULL;
    app_count++;
    return app_count;
}

void aether_ui_app_set_body(int app_handle, int root_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    apps[app_handle - 1].root_handle = root_handle;
}

// Apply immersive dark mode to a window if the system is in dark mode.
static void apply_window_theme(HWND hwnd) {
    BOOL dark = FALSE;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD val = 0, sz = sizeof(val);
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL,
            (LPBYTE)&val, &sz) == ERROR_SUCCESS) {
            dark = (val == 0);
        }
        RegCloseKey(key);
    }
    if (dark) {
        BOOL v = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &v, sizeof(v));
    }
}

void aether_ui_app_run_raw(int app_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    AppEntry* e = &apps[app_handle - 1];
    ensure_win_init();

    // DPI-scaled size
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(e->width, dpi, 96);
    int h = MulDiv(e->height, dpi, 96);

    // Account for non-client area so client ≈ requested size.
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE,
                              WS_EX_APPWINDOW, dpi);
    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    e->hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, APP_CLASS, e->title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!e->hwnd) return;
    apply_window_theme(e->hwnd);

    // Reparent the root widget into the app window.
    if (e->root_handle > 0) {
        Widget* rw = widget_at(e->root_handle);
        if (rw) {
            SetParent(rw->hwnd, e->hwnd);
            LONG_PTR st = GetWindowLongPtrW(rw->hwnd, GWL_STYLE);
            SetWindowLongPtrW(rw->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
            SetWindowLongPtrW(rw->hwnd, GWL_EXSTYLE,
                GetWindowLongPtrW(rw->hwnd, GWL_EXSTYLE) & ~WS_EX_APPWINDOW);
            RECT cr;
            GetClientRect(e->hwnd, &cr);
            SetWindowPos(rw->hwnd, NULL, 0, 0,
                         cr.right - cr.left, cr.bottom - cr.top,
                         SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    }

    // Attach the menu bar (if one was registered via aether_ui_menu_bar_attach).
    // We defer this until here so the window exists.
    if (e->pending_menu) {
        SetMenu(e->hwnd, e->pending_menu);
    }

    // Honor AETHER_UI_HEADLESS for CI and unattended scenarios. The window
    // is still created, the message loop still pumps, and the test server
    // still responds — but nothing is ever rendered to the visible desktop.
    // This keeps GitHub Actions `windows-latest` runs clean (no taskbar
    // icons, no UAC/SmartScreen visibility, no chance of a stuck window).
    const char* headless = getenv("AETHER_UI_HEADLESS");
    int show_mode = (headless && headless[0] && headless[0] != '0') ? SW_HIDE : SW_SHOW;
    ShowWindow(e->hwnd, show_mode);
    if (show_mode == SW_SHOW) UpdateWindow(e->hwnd);

    // A canvas that registered on_key wants the initial keyboard focus so
    // arrow keys work before the user clicks anything (gp/falling_blocks).
    // Now that the window is shown and the canvas is reparented into it,
    // the focus sticks. (Defined below with the canvas globals.)
    aeui_focus_pending_key_canvas();

    // Check AETHER_UI_TEST_PORT and launch test server if set.
    const char* test_port_env = getenv("AETHER_UI_TEST_PORT");
    if (test_port_env) {
        int port = atoi(test_port_env);
        if (port > 0 && e->root_handle > 0) {
            aether_ui_enable_test_server_impl(port, e->root_handle);
        }
    }

    // Message loop with Tab/Enter/Esc dialog-navigation support.
    // IsDialogMessageW routes Tab between WS_TABSTOP controls, Shift+Tab
    // reverses, Enter activates the default button, Esc cancels. Without
    // this wrap, those keys would fall through to the plain
    // TranslateMessage/DispatchMessage path and be ignored by child
    // controls (no focus traversal at all).
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        // When a key-registered canvas has focus, route keystrokes straight
        // to it — IsDialogMessage would otherwise eat Return (default button)
        // and Escape (cancel) before the canvas's WM_KEYDOWN sees them, and
        // gp/falling_blocks navigate with exactly those. (The driver's
        // /canvas/key path bypasses this loop entirely, so specs are
        // unaffected either way — this is for real keyboard input.)
        int canvas_has_focus =
            (msg.message == WM_KEYDOWN || msg.message == WM_CHAR) &&
            aeui_hwnd_is_key_canvas(msg.hwnd);
        // Only the top-level app window participates in dialog nav; child
        // popups created via aether_ui_window_create are independent.
        if (canvas_has_focus || !IsDialogMessageW(e->hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Surface table — "DSL with Scope" surfaces (window / render_to / record).
// Platform-agnostic handle bookkeeping (mirrors aether_ui_gtk4.c).
// ---------------------------------------------------------------------------
#define AUI_SURFACE_WINDOW 0
#define AUI_SURFACE_RENDER 1
#define AUI_SURFACE_RECORD 2

typedef struct {
    int container_handle;
    int kind;
    int app_handle;
    int diag_count;
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

int aether_ui_surface_container_new_impl(int kind) {
    int container = aether_ui_vstack_create(0);
    surface_add(container, kind, 0);
    return container;
}

// Deferred-flush registry — see aether_ui_backend.h. Mirrors the GTK backend
// so the AeVG `vg { … }` deferred-colour fix works identically on Win32.
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

void aether_ui_surface_flush_deferred_impl(void) {
    for (int i = 0; i < deferred_flush_count; i++) {
        invoke_closure(deferred_flushes[i]);
    }
    deferred_flush_count = 0;
}

void aether_ui_surface_run_impl(int container_handle,
                                const char* title, int width, int height) {
    SurfaceEntry* s = surface_for_container(container_handle);
    if (!s || s->kind != AUI_SURFACE_WINDOW) return;
    aether_ui_surface_flush_deferred_impl();
    int app = aether_ui_app_create(title, width, height);
    s->app_handle = app;
    aether_ui_app_set_body(app, container_handle);
    aether_ui_app_run_raw(app);
}

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

// ---------------------------------------------------------------------------
// Core widgets: text, button, vstack, hstack, spacer, divider.
// ---------------------------------------------------------------------------
int aether_ui_text_create(const char* text) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", utf8_to_wide(text),
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return register_widget_typed(h, WK_TEXT);
}

void aether_ui_text_set_string(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (w && w->hwnd) SetWindowTextW(w->hwnd, utf8_to_wide(text));
}

void aether_ui_button_set_label(int handle, const char* label) {
    Widget* w = widget_at(handle);
    if (w && w->hwnd) SetWindowTextW(w->hwnd, utf8_to_wide(label));
}

// Right-click context menus: not yet implemented on Win32 (TrackPopupMenu is
// the native shape). No-op stub — same precedent as canvas_on_move.
void aether_ui_context_menu_item_impl(int handle, const char* label,
                                      void* boxed_closure) {
    (void)handle; (void)label; (void)boxed_closure;
}

// Shortcuts + focus stubs (item 9) — keep the cross-platform ABI green;
// real accelerators/focus wiring is follow-up work on this backend.
void aether_ui_shortcut_impl(const char* combo, void* boxed_closure) {
    (void)combo; (void)boxed_closure;
}
void aether_ui_focus_impl(int handle) {
    Widget* w = widget_at(handle);
    if (w) SetFocus(w->hwnd);
}
void aether_ui_context_menu_item_accel_impl(int handle, const char* label,
                                            const char* accel,
                                            void* boxed_closure) {
    char joined[256];
    if (accel && accel[0]) {
        snprintf(joined, sizeof(joined), "%s    %s", label ? label : "", accel);
    } else {
        snprintf(joined, sizeof(joined), "%s", label ? label : "");
    }
    aether_ui_context_menu_item_impl(handle, joined, boxed_closure);
}


int aether_ui_button_create_plain(const char* label) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"BUTTON", utf8_to_wide(label),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return register_widget_typed(h, WK_BUTTON);
}

int aether_ui_button_create(const char* label, void* boxed_closure) {
    int handle = aether_ui_button_create_plain(label);
    Widget* w = widget_at(handle);
    if (w) w->on_click = (AeClosure*)boxed_closure;
    return handle;
}

void aether_ui_set_onclick_ctx(void* ctx, void* boxed_closure) {
    int handle = (int)(intptr_t)ctx;
    Widget* w = widget_at(handle);
    if (w) w->on_click = (AeClosure*)boxed_closure;
}

static int create_stack(int orientation, int spacing) {
    ensure_win_init();
    // WS_EX_CONTROLPARENT: the dialog tab-walk (GetNextDlgTabItem /
    // IsDialogMessage) recurses INTO this container instead of treating
    // it as a leaf — without it, Tab from a nested control resolves to
    // the wrong widget.
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, STACK_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    WidgetKind kind = orientation == 0 ? WK_HSTACK
                   : orientation == 1 ? WK_VSTACK : WK_ZSTACK;
    int handle = register_widget_typed(h, kind);
    Widget* w = widget_at(handle);
    if (w) {
        w->stack.orientation = orientation;
        w->stack.spacing = spacing;
    }
    return handle;
}

int aether_ui_vstack_create(int spacing) { return create_stack(1, spacing); }
int aether_ui_hstack_create(int spacing) { return create_stack(0, spacing); }
int aether_ui_zstack_create(void) { return create_stack(2, 0); }

int aether_ui_spacer_create(void) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, SPACER_CLASS, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    return register_widget_typed(h, WK_SPACER);
}

int aether_ui_divider_create(void) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, DIVIDER_CLASS, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
        widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    int handle = register_widget_typed(h, WK_DIVIDER);
    Widget* w = widget_at(handle);
    if (w) { w->pref_width = 1; w->pref_height = 12; }
    return handle;
}

// ---------------------------------------------------------------------------
// Parent → child wiring. Unlike GTK's "add_child", Win32 parents children
// at creation time OR reparents via SetParent. Our DSL calls this after
// creation, so we reparent here.
// ---------------------------------------------------------------------------
void aether_ui_widget_add_child_ctx(void* parent_ctx, int child_handle) {
    int parent_handle = (int)(intptr_t)parent_ctx;
    Widget* p = widget_at(parent_handle);
    Widget* c = widget_at(child_handle);
    if (!p || !c) return;
    SetParent(c->hwnd, p->hwnd);
    LONG_PTR st = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
    SetWindowLongPtrW(c->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    ShowWindow(c->hwnd, SW_SHOW);
    if (p->kind == WK_VSTACK || p->kind == WK_HSTACK || p->kind == WK_ZSTACK) {
        stack_do_layout(p->hwnd);
    }
}

void aether_ui_widget_set_hidden(int handle, int hidden) {
    Widget* w = widget_at(handle);
    if (w) ShowWindow(w->hwnd, hidden ? SW_HIDE : SW_SHOW);
}

// ---------------------------------------------------------------------------
// Input widgets: textfield, securefield, textarea, toggle, slider, picker,
// progressbar. All register change closures dispatched via the stack proc's
// WM_COMMAND handler.
// ---------------------------------------------------------------------------
int aether_ui_textfield_create(const char* placeholder, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    // Windows has no native placeholder on EDIT — use EM_SETCUEBANNER (comctl32 6+)
    if (placeholder && *placeholder) {
        SendMessageW(h, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                     (LPARAM)utf8_to_wide(placeholder));
    }
    int handle = register_widget_typed(h, WK_TEXTFIELD);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 24; }
    return handle;
}

int aether_ui_securefield_create(const char* placeholder, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_PASSWORD | ES_AUTOHSCROLL,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (placeholder && *placeholder) {
        SendMessageW(h, 0x1501, TRUE, (LPARAM)utf8_to_wide(placeholder));
    }
    int handle = register_widget_typed(h, WK_SECUREFIELD);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 24; }
    return handle;
}

void aether_ui_textfield_set_text(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (w) SetWindowTextW(w->hwnd, utf8_to_wide(text));
}

const char* aether_ui_textfield_get_text(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return "";
    wchar_t buf[4096];
    GetWindowTextW(w->hwnd, buf, 4096);
    return wide_to_utf8(buf);
}

int aether_ui_toggle_create(const char* label, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"BUTTON", utf8_to_wide(label),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    int handle = register_widget_typed(h, WK_TOGGLE);
    Widget* w = widget_at(handle);
    if (w) w->on_change = (AeClosure*)boxed_closure;
    return handle;
}

void aether_ui_toggle_set_active(int handle, int active) {
    Widget* w = widget_at(handle);
    if (w) SendMessageW(w->hwnd, BM_SETCHECK,
        active ? BST_CHECKED : BST_UNCHECKED, 0);
}

int aether_ui_toggle_get_active(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return 0;
    return SendMessageW(w->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
}

// Radio-group a toggle with another. STUB: Win32 radio semantics need
// BS_AUTORADIOBUTTON + WS_GROUP at creation time; grouping an existing
// checkbox after the fact requires a style swap not yet wired. The GTK4
// backend is real; callers get plain checkboxes here (no exclusivity).
void aether_ui_toggle_set_group(int handle, int group_with) {
    (void)handle; (void)group_with;
}

int aether_ui_slider_create(double min_val, double max_val,
                            double initial, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    double frac = (max_val > min_val) ? (initial - min_val) / (max_val - min_val) : 0;
    SendMessageW(h, TBM_SETPOS, TRUE, (LPARAM)(int)(frac * 1000));
    int handle = register_widget_typed(h, WK_SLIDER);
    Widget* w = widget_at(handle);
    if (w) {
        w->u.slider.min_v = min_val;
        w->u.slider.max_v = max_val;
        w->u.slider.cur_v = initial;
        w->on_change = (AeClosure*)boxed_closure;
        w->pref_height = 28;
    }
    return handle;
}

void aether_ui_slider_set_value(int handle, double value) {
    Widget* w = widget_at(handle);
    if (!w) return;
    double frac = (w->u.slider.max_v > w->u.slider.min_v)
        ? (value - w->u.slider.min_v) / (w->u.slider.max_v - w->u.slider.min_v) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    SendMessageW(w->hwnd, TBM_SETPOS, TRUE, (LPARAM)(int)(frac * 1000));
    w->u.slider.cur_v = value;
}

double aether_ui_slider_get_value(int handle) {
    Widget* w = widget_at(handle);
    return w ? w->u.slider.cur_v : 0.0;
}

int aether_ui_picker_create(void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    int handle = register_widget_typed(h, WK_PICKER);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 200; }
    return handle;
}

void aether_ui_picker_add_item(int handle, const char* item) {
    Widget* w = widget_at(handle);
    if (!w) return;
    SendMessageW(w->hwnd, CB_ADDSTRING, 0, (LPARAM)utf8_to_wide(item));
    // GtkDropDown parity: the first item becomes the selection (CB_SETCURSEL
    // does not fire CBN_SELCHANGE, so on_change stays quiet — GTK's default
    // selection is silent too). Without this, CB_GETCURSEL answers -1 and
    // the label is empty until the user picks.
    if (SendMessageW(w->hwnd, CB_GETCURSEL, 0, 0) == CB_ERR) {
        SendMessageW(w->hwnd, CB_SETCURSEL, 0, 0);
    }
}

void aether_ui_picker_set_selected(int handle, int index) {
    Widget* w = widget_at(handle);
    if (w) SendMessageW(w->hwnd, CB_SETCURSEL, (WPARAM)index, 0);
}

int aether_ui_picker_get_selected(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    return (int)SendMessageW(w->hwnd, CB_GETCURSEL, 0, 0);
}

int aether_ui_textarea_create(const char* placeholder, void* boxed_closure) {
    ensure_win_init();
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
            | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (placeholder && *placeholder) {
        SendMessageW(h, 0x1501, TRUE, (LPARAM)utf8_to_wide(placeholder));
    }
    int handle = register_widget_typed(h, WK_TEXTAREA);
    Widget* w = widget_at(handle);
    if (w) { w->on_change = (AeClosure*)boxed_closure; w->pref_height = 120; }
    return handle;
}

void aether_ui_textarea_set_text(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (w) SetWindowTextW(w->hwnd, utf8_to_wide(text));
}

char* aether_ui_textarea_get_text(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return strdup("");
    int len = GetWindowTextLengthW(w->hwnd);
    wchar_t* buf = (wchar_t*)malloc(sizeof(wchar_t) * (len + 1));
    GetWindowTextW(w->hwnd, buf, len + 1);
    char* out = strdup(wide_to_utf8(buf));
    free(buf);
    return out;
}

int aether_ui_scrollview_create(void) {
    // Approximation: a stack with scroll styles. Full virtualization would
    // need a custom class; for now we lean on the stack child hosting + the
    // parent window's scrollbars if needed.
    int handle = create_stack(1, 0);
    Widget* w = widget_at(handle);
    if (w) {
        LONG_PTR st = GetWindowLongPtrW(w->hwnd, GWL_STYLE);
        SetWindowLongPtrW(w->hwnd, GWL_STYLE, st | WS_VSCROLL | WS_HSCROLL);
        w->kind = WK_SCROLLVIEW;
    }
    return handle;
}

// splitview stub: a plain stack in the split's orientation (children lay
// out side by side / stacked; no draggable splitter). A real Win32
// splitter is follow-up work — this keeps the cross-platform ABI green.
// NB: splitview("h") means panes side-by-side = a HORIZONTAL stack.
int aether_ui_splitview_create(int vertical) {
    return create_stack(vertical, 0);
}
int aether_ui_split_position_impl(int handle) {
    (void)handle;
    return -1;
}
void aether_ui_split_set_position_impl(int handle, int px) {
    (void)handle; (void)px;
}
void aether_ui_widget_weight_impl(int handle, int n) {
    (void)handle; (void)n;
}
void aether_ui_on_layout_impl(int handle, void* boxed_closure) {
    (void)handle; (void)boxed_closure;
}
int aether_ui_wrap_create(void) {
    // Stub: a horizontal stack (no wrapping).
    return create_stack(0, 6);
}

// tabs stub: a vertical stack; each tab() appends its page. No tab strip,
// no page switching yet (real Win32 = SysTabControl32 / a header of
// buttons over a WK_ZSTACK) — pages just stack. Keeps the ABI + build
// green; the composite is walkable so specs see the pages.
int aether_ui_tabs_create(void* boxed_closure) {
    (void)boxed_closure;
    return create_stack(1, 0);  // vertical
}
int aether_ui_tab_add(int tabs_handle, const char* title) {
    (void)title;
    // Return an inner vstack parented into the tabs stack, so the DSL
    // block's children attach inside the page (matches the GTK contract).
    int page = create_stack(1, 0);
    aether_ui_widget_add_child_ctx((void*)(intptr_t)tabs_handle, page);
    return page;
}
int aether_ui_tabs_selected(int tabs_handle) { (void)tabs_handle; return -1; }
int aether_ui_tabs_count(int tabs_handle)    { (void)tabs_handle; return 0; }
void aether_ui_tabs_select(int tabs_handle, int index) {
    (void)tabs_handle; (void)index;
}
void aether_ui_tabs_set_on_change(int tabs_handle, void* boxed_closure) {
    (void)tabs_handle; (void)boxed_closure;
}

int aether_ui_progressbar_create(double fraction) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    SendMessageW(h, PBM_SETRANGE32, 0, 1000);
    int v = (int)(fraction * 1000);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    SendMessageW(h, PBM_SETPOS, (WPARAM)v, 0);
    int handle = register_widget_typed(h, WK_PROGRESSBAR);
    Widget* w = widget_at(handle);
    if (w) { w->u.progressbar.fraction = fraction; w->pref_height = 20; }
    return handle;
}

void aether_ui_progressbar_set_fraction(int handle, double fraction) {
    Widget* w = widget_at(handle);
    if (!w) return;
    int v = (int)(fraction * 1000);
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    SendMessageW(w->hwnd, PBM_SETPOS, (WPARAM)v, 0);
    w->u.progressbar.fraction = fraction;
}

// ---------------------------------------------------------------------------
// Form / FormSection / NavStack — thin wrappers over VStack.
// ---------------------------------------------------------------------------
int aether_ui_form_create(void) {
    int handle = create_stack(1, 12);
    Widget* w = widget_at(handle);
    if (w) {
        w->kind = WK_FORM;
        w->stack.padding_top = w->stack.padding_bottom = 12;
        w->stack.padding_left = w->stack.padding_right = 16;
    }
    return handle;
}

int aether_ui_form_section_create(const char* title) {
    int handle = create_stack(1, 6);
    Widget* w = widget_at(handle);
    if (w) w->kind = WK_FORM_SECTION;
    if (title && *title) {
        int th = aether_ui_text_create(title);
        Widget* tw = widget_at(th);
        if (tw) {
            // Bold section header
            HFONT base = (HFONT)SendMessageW(tw->hwnd, WM_GETFONT, 0, 0);
            LOGFONTW lf;
            GetObjectW(base, sizeof(lf), &lf);
            lf.lfWeight = FW_BOLD;
            HFONT bold = CreateFontIndirectW(&lf);
            SendMessageW(tw->hwnd, WM_SETFONT, (WPARAM)bold, TRUE);
            tw->custom_font = bold;
        }
        aether_ui_widget_add_child_ctx((void*)(intptr_t)handle, th);
    }
    return handle;
}

int aether_ui_navstack_create(void) {
    int handle = create_stack(1, 0);
    Widget* w = widget_at(handle);
    if (w) w->kind = WK_NAVSTACK;
    return handle;
}

void aether_ui_navstack_push(int handle, const char* title, int body_handle) {
    (void)title;
    Widget* host = widget_at(handle);
    Widget* body = widget_at(body_handle);
    if (!host || !body) return;
    // Hide previous children, show this one.
    for (HWND c = GetWindow(host->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        ShowWindow(c, SW_HIDE);
    }
    SetParent(body->hwnd, host->hwnd);
    LONG_PTR st = GetWindowLongPtrW(body->hwnd, GWL_STYLE);
    SetWindowLongPtrW(body->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    ShowWindow(body->hwnd, SW_SHOW);
    stack_do_layout(host->hwnd);
}

void aether_ui_navstack_pop(int handle) {
    Widget* host = widget_at(handle);
    if (!host) return;
    HWND top = NULL;
    for (HWND c = GetWindow(host->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (IsWindowVisible(c)) top = c;
    }
    if (top) {
        DestroyWindow(top);
        // Re-show the new top child if any.
        for (HWND c = GetWindow(host->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            ShowWindow(c, SW_SHOW);
        }
    }
    stack_do_layout(host->hwnd);
}

// ---------------------------------------------------------------------------
// Styling + theming.
// ---------------------------------------------------------------------------
static inline COLORREF rgb_from_doubles(double r, double g, double b) {
    int ri = (int)(r * 255); if (ri < 0) ri = 0; if (ri > 255) ri = 255;
    int gi = (int)(g * 255); if (gi < 0) gi = 0; if (gi > 255) gi = 255;
    int bi = (int)(b * 255); if (bi < 0) bi = 0; if (bi > 255) bi = 255;
    return RGB(ri, gi, bi);
}

void aether_ui_set_bg_color(int handle, double r, double g, double b, double a) {
    (void)a; // Solid colors only; alpha requires layered-window composition.
    Widget* w = widget_at(handle);
    if (!w) return;
    w->bg.has_value = 1;
    w->bg.color = rgb_from_doubles(r, g, b);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

void aether_ui_set_bg_gradient(int handle,
                               double r1, double g1, double b1,
                               double r2, double g2, double b2, int vertical) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->gradient_enabled = 1;
    w->grad_a = rgb_from_doubles(r1, g1, b1);
    w->grad_b = rgb_from_doubles(r2, g2, b2);
    w->grad_vertical = vertical;
    InvalidateRect(w->hwnd, NULL, TRUE);
}

void aether_ui_set_text_color(int handle, double r, double g, double b) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->fg.has_value = 1;
    w->fg.color = rgb_from_doubles(r, g, b);
    InvalidateRect(w->hwnd, NULL, TRUE);
}

static void apply_font(Widget* w) {
    if (!w) return;
    LOGFONTW lf = {0};
    HFONT base = (HFONT)SendMessageW(w->hwnd, WM_GETFONT, 0, 0);
    if (base) GetObjectW(base, sizeof(lf), &lf);
    else GetObjectW((HFONT)GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    if (w->font_size > 0) {
        // Font size in points → logical units for current DPI
        HDC hdc = GetDC(w->hwnd);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(w->hwnd, hdc);
        lf.lfHeight = -MulDiv((int)w->font_size, dpi, 72);
    }
    lf.lfWeight = w->font_bold ? FW_BOLD : FW_NORMAL;
    if (w->custom_font) DeleteObject(w->custom_font);
    w->custom_font = CreateFontIndirectW(&lf);
    SendMessageW(w->hwnd, WM_SETFONT, (WPARAM)w->custom_font, TRUE);
}

void aether_ui_set_font_size(int handle, double size) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->font_size = size;
    apply_font(w);
}

void aether_ui_set_font_bold(int handle, int bold) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->font_bold = bold;
    apply_font(w);
}

void aether_ui_set_corner_radius(int handle, double radius) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->corner_radius = (int)radius;
    RECT r;
    GetClientRect(w->hwnd, &r);
    HRGN rgn = CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1,
                                   (int)radius * 2, (int)radius * 2);
    SetWindowRgn(w->hwnd, rgn, TRUE);
}

void aether_ui_set_edge_insets(int handle, double top, double right,
                               double bottom, double left) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->stack.padding_top = (int)top;
    w->stack.padding_right = (int)right;
    w->stack.padding_bottom = (int)bottom;
    w->stack.padding_left = (int)left;
    if (w->kind == WK_VSTACK || w->kind == WK_HSTACK || w->kind == WK_ZSTACK) {
        stack_do_layout(w->hwnd);
    }
}

void aether_ui_set_width(int handle, int width) {
    Widget* w = widget_at(handle);
    if (w) w->pref_width = width;
}

void aether_ui_set_height(int handle, int height) {
    Widget* w = widget_at(handle);
    if (w) w->pref_height = height;
}

void aether_ui_set_opacity(int handle, double opacity) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->opacity = opacity;
    // WS_EX_LAYERED only works reliably on top-level windows; for child
    // widgets this is a no-op. Apps requesting child opacity should use
    // compositing backends (we fall back silently).
    LONG_PTR ex = GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE);
    if (!(ex & WS_CHILD)) {
        SetWindowLongPtrW(w->hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        double a = opacity;
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        SetLayeredWindowAttributes(w->hwnd, 0, (BYTE)(a * 255), LWA_ALPHA);
    }
}

void aether_ui_set_enabled(int handle, int enabled) {
    Widget* w = widget_at(handle);
    if (w) EnableWindow(w->hwnd, enabled);
}

void aether_ui_set_tooltip(int handle, const char* text) {
    Widget* w = widget_at(handle);
    if (!w) return;
    static HWND tooltip_hwnd = NULL;
    if (!tooltip_hwnd) {
        tooltip_hwnd = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            NULL, NULL, GetModuleHandleW(NULL), NULL);
    }
    if (w->tooltip) free(w->tooltip);
    w->tooltip = _wcsdup(utf8_to_wide(text));
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = GetParent(w->hwnd);
    ti.uId = (UINT_PTR)w->hwnd;
    ti.lpszText = w->tooltip;
    SendMessageW(tooltip_hwnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void aether_ui_set_distribution(int handle, int distribution) {
    Widget* w = widget_at(handle);
    if (w) w->stack.distribution = distribution;
}

void aether_ui_set_alignment(int handle, int alignment) {
    Widget* w = widget_at(handle);
    if (w) w->stack.alignment = alignment;
}

void aether_ui_match_parent_width(int handle) {
    Widget* w = widget_at(handle);
    if (w) w->pref_width = -1; // marker: fill parent
}

void aether_ui_match_parent_height(int handle) {
    Widget* w = widget_at(handle);
    if (w) w->pref_height = -1;
}

void aether_ui_set_margin(int handle, int top, int right, int bottom, int left) {
    Widget* w = widget_at(handle);
    if (!w) return;
    w->margin_top = top;
    w->margin_right = right;
    w->margin_bottom = bottom;
    w->margin_left = left;
}

void aether_ui_set_onclick_ctx_style_apply(void* ctx, double r, double g, double b, double a) {
    (void)ctx; (void)r; (void)g; (void)b; (void)a;
}
void aether_ui_set_bg_color_ctx(void* ctx, double r, double g, double b, double a) {
    aether_ui_set_bg_color((int)(intptr_t)ctx, r, g, b, a);
}
void aether_ui_set_margin_ctx(void* ctx, int top, int right, int bottom, int left) {
    aether_ui_set_margin((int)(intptr_t)ctx, top, right, bottom, left);
}
void aether_ui_enable_test_server_ctx(int port, void* ctx) {
    aether_ui_enable_test_server_impl(port, (int)(intptr_t)ctx);
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

// ---------------------------------------------------------------------------
// Events (hover, double-click, click-on-arbitrary-widget).
// ---------------------------------------------------------------------------
void aether_ui_on_click_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (w) w->on_click = (AeClosure*)boxed_closure;
}

void aether_ui_on_hover_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (w) w->on_hover = (AeClosure*)boxed_closure;
}

void aether_ui_on_double_click_impl(int handle, void* boxed_closure) {
    Widget* w = widget_at(handle);
    if (w) w->on_double_click = (AeClosure*)boxed_closure;
}

// ---------------------------------------------------------------------------
// System services: alert, file open, clipboard, timer, open URL, dark mode.
// ---------------------------------------------------------------------------
void aether_ui_alert_impl(const char* title, const char* message) {
    if (aeui_is_headless()) return;  // MessageBox would block indefinitely
    ensure_win_init();
    MessageBoxW(NULL, utf8_to_wide(message), utf8_to_wide(title),
                MB_OK | MB_ICONINFORMATION);
}

char* aether_ui_file_open(const char* title) {
    if (aeui_is_headless()) return NULL;  // modal file dialog would block
    ensure_win_init();
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"All Files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = utf8_to_wide(title ? title : "Open File");
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        return strdup(wide_to_utf8(file));
    }
    return NULL;
}

void aether_ui_clipboard_write_impl(const char* text) {
    if (!text) return;
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    wchar_t* wide = utf8_to_wide(text);
    size_t len = wcslen(wide) + 1;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(wchar_t));
    if (mem) {
        wchar_t* ptr = (wchar_t*)GlobalLock(mem);
        memcpy(ptr, wide, len * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

// Timer: each user timer is keyed by (hwnd, id). We use the first live app
// window as the timer host — but ui.timer is usually called INSIDE the
// window block, BEFORE app_run creates the hwnd. SetTimer(NULL, id, …)
// then runs as a THREAD timer and Windows IGNORES the passed id,
// assigning its own — so a callback matching on our id never fires (gp's
// whole 30Hz heartbeat was dead on win32). Track the SYSTEM id + host.
typedef struct {
    int id;
    UINT_PTR sys_id;   // what WM_TIMER actually reports
    HWND host;         // NULL = thread timer
    AeClosure* closure;
    int alive;
} TimerEntry;
static TimerEntry* timers = NULL;
static int timer_count = 0;
static int timer_capacity = 0;
static int next_timer_id = 100;

static void CALLBACK timer_cb(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < timer_count; i++) {
        if (timers[i].alive && timers[i].sys_id == id) {
            invoke_closure(timers[i].closure);
            return;
        }
    }
}

int aether_ui_timer_create_impl(int interval_ms, void* boxed_closure) {
    if (timer_count >= timer_capacity) {
        timer_capacity = timer_capacity == 0 ? 16 : timer_capacity * 2;
        timers = (TimerEntry*)realloc(timers, sizeof(TimerEntry) * timer_capacity);
    }
    int id = next_timer_id++;
    HWND host = (app_count > 0) ? apps[0].hwnd : NULL;
    UINT_PTR sys_id = SetTimer(host, id, interval_ms, (TIMERPROC)timer_cb);
    timers[timer_count].id = id;
    // With a window host SetTimer returns the id we passed; with a NULL
    // host it returns the system-assigned thread-timer id.
    timers[timer_count].sys_id = host ? (UINT_PTR)id : sys_id;
    timers[timer_count].host = host;
    timers[timer_count].closure = (AeClosure*)boxed_closure;
    timers[timer_count].alive = 1;
    timer_count++;
    return id;
}

void aether_ui_timer_cancel_impl(int timer_id) {
    for (int i = 0; i < timer_count; i++) {
        if (timers[i].id == timer_id) {
            timers[i].alive = 0;
            KillTimer(timers[i].host, timers[i].sys_id);
            return;
        }
    }
}

void aether_ui_open_url_impl(const char* url) {
    ShellExecuteW(NULL, L"open", utf8_to_wide(url), NULL, NULL, SW_SHOWNORMAL);
}

int aether_ui_dark_mode_check(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    DWORD val = 1, sz = sizeof(val);
    int dark = 0;
    if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL,
        (LPBYTE)&val, &sz) == ERROR_SUCCESS) dark = (val == 0);
    RegCloseKey(key);
    return dark;
}

// ---------------------------------------------------------------------------
// Secondary windows + sheets.
// ---------------------------------------------------------------------------
int aether_ui_window_create_impl(const char* title, int width, int height) {
    ensure_win_init();
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(width, dpi, 96);
    int h = MulDiv(height, dpi, 96);
    RECT rc = { 0, 0, w, h };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, APP_CLASS, utf8_to_wide(title),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 0;
    apply_window_theme(hwnd);
    return register_widget_typed(hwnd, WK_WINDOW);
}

void aether_ui_window_set_body_impl(int win_handle, int root_handle) {
    Widget* win = widget_at(win_handle);
    Widget* root = widget_at(root_handle);
    if (!win || !root) return;
    SetParent(root->hwnd, win->hwnd);
    LONG_PTR st = GetWindowLongPtrW(root->hwnd, GWL_STYLE);
    SetWindowLongPtrW(root->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    RECT cr;
    GetClientRect(win->hwnd, &cr);
    SetWindowPos(root->hwnd, NULL, 0, 0,
                 cr.right - cr.left, cr.bottom - cr.top,
                 SWP_NOZORDER | SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------------------
// In-window overlay layer (roadmap item 1, win32 parity) — toast / modal /
// tooltip, drawn INSIDE the app window as raised child HWNDs, never as a
// popup. Mirrors the AppKit translation (aether_ui_macos.m): the overlay
// table is append-only, handles 1-based and monotonic, closing flips `live`
// to 0 without removing the entry — GET /overlays must show a toast as
// observably DEAD, not merely absent.
//
// The "host" is simply the app window's client area: child windows z-order
// over siblings, so a raised scrim + content IS the overlay stack. The
// scrim is a WS_EX_LAYERED child (uniform alpha works for child windows
// since Win8) that swallows clicks; a scrim click is the ONLY path that
// fires on_dismiss (GTK/macOS behave the same).
// ---------------------------------------------------------------------------
typedef struct {
    HWND content;        // reparented into the app window while live
    HWND scrim;          // NULL when non-modal
    AeClosure* on_dismiss;
    int modal;
    int live;
} Win32OverlayEntry;

static Win32OverlayEntry* w32_overlays = NULL;
static int w32_overlay_count = 0;
static int w32_overlay_capacity = 0;

static Win32OverlayEntry* w32_overlay_at(int handle) {
    if (handle < 1 || handle > w32_overlay_count) return NULL;
    return &w32_overlays[handle - 1];
}

// Natural size of a (possibly detached) widget subtree. measure_widget
// answers leaves (text extent, pref sizes); stacks sum/max their children —
// a detached modal card has never been laid out, so its own rect is 0x0
// and only recursion gives an honest size.
static void measure_subtree(HWND hwnd, int* out_w, int* out_h) {
    int handle = handle_for_hwnd(hwnd);
    Widget* w = handle ? widget_at(handle) : NULL;
    if (!w) { *out_w = 0; *out_h = 0; return; }
    if (w->kind == WK_VSTACK || w->kind == WK_HSTACK || w->kind == WK_ZSTACK) {
        int total_w = 0, total_h = 0, n = 0;
        for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            int cw = 0, ch = 0;
            measure_subtree(c, &cw, &ch);
            if (w->kind == WK_VSTACK) {
                if (cw > total_w) total_w = cw;
                total_h += ch;
            } else if (w->kind == WK_HSTACK) {
                total_w += cw;
                if (ch > total_h) total_h = ch;
            } else {
                if (cw > total_w) total_w = cw;
                if (ch > total_h) total_h = ch;
            }
            n++;
        }
        int sp = (n > 1) ? (n - 1) * w->stack.spacing : 0;
        if (w->kind == WK_VSTACK) total_h += sp;
        if (w->kind == WK_HSTACK) total_w += sp;
        // Stack margins apply during parent layout; include our own padding.
        *out_w = total_w + w->margin_left + w->margin_right + 16;
        *out_h = total_h + w->margin_top + w->margin_bottom + 16;
        return;
    }
    measure_widget(w, out_w, out_h);
    // Leaves get breathing room the win32 text extent doesn't include.
    *out_w += 12;
    *out_h += 8;
}

static LRESULT CALLBACK scrim_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) {
        // Find the overlay owning this scrim; fire on_dismiss then close.
        for (int i = 0; i < w32_overlay_count; i++) {
            Win32OverlayEntry* e = &w32_overlays[i];
            if (e->live && e->scrim == hwnd) {
                if (e->on_dismiss && e->on_dismiss->fn) {
                    ((void(*)(void*))e->on_dismiss->fn)(e->on_dismiss->env);
                }
                aether_ui_overlay_close_impl(i + 1);
                return 0;
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int aether_ui_overlay_open_impl(int win_handle, int content_handle,
                                int anchor, int dx, int dy, int modal) {
    (void)win_handle;   // single-window today; overlays land on the primary
    ensure_win_init();
    Widget* content = widget_at(content_handle);
    if (!content || app_count < 1 || !apps[0].hwnd) return 0;
    HWND host = apps[0].hwnd;

    if (w32_overlay_count >= w32_overlay_capacity) {
        w32_overlay_capacity = w32_overlay_capacity == 0 ? 8 : w32_overlay_capacity * 2;
        w32_overlays = (Win32OverlayEntry*)realloc(
            w32_overlays, sizeof(Win32OverlayEntry) * w32_overlay_capacity);
        if (!w32_overlays) { w32_overlay_count = 0; w32_overlay_capacity = 0; return 0; }
    }
    int handle = w32_overlay_count + 1;
    Win32OverlayEntry* e = &w32_overlays[w32_overlay_count];
    memset(e, 0, sizeof(*e));
    e->modal = modal ? 1 : 0;
    e->live = 1;

    RECT cr;
    GetClientRect(host, &cr);
    int W = cr.right - cr.left, H = cr.bottom - cr.top;

    // Scrim first so it sits BELOW the content in z-order.
    if (modal) {
        HWND scrim = CreateWindowExW(WS_EX_LAYERED, SCRIM_CLASS, L"",
            WS_CHILD | WS_VISIBLE, 0, 0, W, H,
            host, NULL, GetModuleHandleW(NULL), NULL);
        SetLayeredWindowAttributes(scrim, 0, 115, LWA_ALPHA);  // ~45% black
        register_widget_typed(scrim, WK_SCRIM);
        SetWindowPos(scrim, HWND_TOP, 0, 0, W, H, SWP_SHOWWINDOW);
        e->scrim = scrim;
    }

    // Reparent the content into the window, size it naturally, place by
    // anchor. anchor packs halign in bits 0-1 (0=start 1=center 2=end) and
    // valign in bits 2-3: code = h + v*4; dx/dy are SIGNED insets (positive
    // from start/top, negative from end/bottom).
    int cw = 0, ch = 0;
    measure_subtree(content->hwnd, &cw, &ch);
    if (cw < 40) cw = 40;
    if (ch < 24) ch = 24;
    if (cw > W) cw = W;
    if (ch > H) ch = H;
    int h = anchor & 3, v = (anchor >> 2) & 3;
    int x, y;
    if (h == 0)      x = dx > 0 ? dx : 0;
    else if (h == 2) x = W - cw + (dx < 0 ? dx : 0);
    else             x = (W - cw) / 2 + dx;
    if (v == 0)      y = dy > 0 ? dy : 0;
    else if (v == 2) y = H - ch + (dy < 0 ? dy : 0);
    else             y = (H - ch) / 2 + dy;

    SetParent(content->hwnd, host);
    SetWindowPos(content->hwnd, HWND_TOP, x, y, cw, ch, SWP_SHOWWINDOW);
    e->content = content->hwnd;
    w32_overlay_count++;
    return handle;
}

void aether_ui_overlay_close_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    if (!e || !e->live) return;   // idempotent
    e->live = 0;
    if (e->scrim) { DestroyWindow(e->scrim); e->scrim = NULL; }
    if (e->content) {
        // Back to the hidden holder — the widget stays registered (and
        // reusable), it just leaves the window like GTK's overlay child.
        ShowWindow(e->content, SW_HIDE);
        SetParent(e->content, widget_holder);
        e->content = NULL;
    }
}

void aether_ui_overlay_set_on_dismiss_impl(int overlay_handle, void* boxed_closure) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    if (e) e->on_dismiss = (AeClosure*)boxed_closure;
}

int aether_ui_overlay_is_live_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return e ? e->live : 0;
}

int aether_ui_overlay_count_impl(void) { return w32_overlay_count; }

int aether_ui_overlay_is_modal_impl(int overlay_handle) {
    Win32OverlayEntry* e = w32_overlay_at(overlay_handle);
    return e ? e->modal : 0;
}

// Escape closes the TOPMOST live overlay; returns 1 if one was closed so
// the caller can let Escape propagate when nothing is open.
static int w32_escape_overlays(void) {
    for (int i = w32_overlay_count - 1; i >= 0; i--) {
        if (w32_overlays[i].live) {
            aether_ui_overlay_close_impl(i + 1);
            return 1;
        }
    }
    return 0;
}

// Toast auto-dismiss: sys-timer-id → overlay handle (thread timers get a
// SYSTEM-assigned id — the ui.timer lesson).
typedef struct { UINT_PTR sys_id; int overlay; } ToastTimer;
static ToastTimer toast_timers[16];

static void CALLBACK toast_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < 16; i++) {
        if (toast_timers[i].sys_id == id && toast_timers[i].overlay) {
            KillTimer(NULL, id);
            aether_ui_overlay_close_impl(toast_timers[i].overlay);
            toast_timers[i].sys_id = 0;
            toast_timers[i].overlay = 0;
            return;
        }
    }
    KillTimer(NULL, id);
}

int aether_ui_toast_impl(int win_handle, const char* text, int ms) {
    // A real registered text widget, opened bottom-center, non-modal,
    // lifted 24px off the edge (anchor 9 = h:1 center, v:2 bottom).
    int content = aether_ui_text_create(text ? text : "");
    int handle = aether_ui_overlay_open_impl(win_handle, content, 9, 0, -24, 0);
    if (handle > 0 && ms > 0) {
        UINT_PTR sid = SetTimer(NULL, 0, (UINT)ms, toast_timer_proc);
        for (int i = 0; i < 16; i++) {
            if (toast_timers[i].sys_id == 0) {
                toast_timers[i].sys_id = sid;
                toast_timers[i].overlay = handle;
                break;
            }
        }
    }
    return handle;
}
void aether_ui_widget_apply_css_impl(int handle, const char* property_css) {
    (void)handle; (void)property_css;
}
void aether_ui_widget_add_css_class_impl(int handle, const char* cls) {
    Widget* w = widget_at(handle);
    if (!w || !cls || !cls[0]) return;
    if (w->classes && strstr(w->classes, cls)) return;  // dedupe (name-safe: our classes don't prefix each other)
    size_t need = (w->classes ? strlen(w->classes) + 1 : 0) + strlen(cls) + 1;
    char* nc = (char*)malloc(need);
    if (w->classes && w->classes[0]) {
        sprintf(nc, "%s %s", w->classes, cls);
    } else {
        strcpy(nc, cls);
    }
    free(w->classes);
    w->classes = nc;
}
void aether_ui_widget_remove_css_class_impl(int handle, const char* cls) {
    Widget* w = widget_at(handle);
    if (!w || !w->classes || !cls || !cls[0]) return;
    char* pos = strstr(w->classes, cls);
    if (!pos) return;
    size_t cl = strlen(cls);
    // Close the gap incl. one separating space on either side.
    char* from = pos + cl;
    if (*from == ' ') from++;
    else if (pos > w->classes && pos[-1] == ' ') pos--;
    memmove(pos, from, strlen(from) + 1);
}
void aether_ui_canvas_group_begin_impl(int canvas_id) { (void)canvas_id; }
void aether_ui_canvas_group_end_impl(int canvas_id, double alpha) {
    (void)canvas_id; (void)alpha;
}
int aether_ui_canvas_read_pixel_impl(int canvas_id, int px, int py,
                                     int width, int height) {
    (void)canvas_id; (void)px; (void)py; (void)width; (void)height;
    return -1;
}
// Drawn vg tooltip — one overlay per SHOW (a hide flips it dead; the next
// hover opens a fresh one, matching the macOS/GTK observable contract:
// specs assert live:1 on hover, live:0 after moving off, live:1 again).
static int w32_tooltip_overlay = 0;
static int w32_tooltip_label = 0;

int aether_ui_vg_tooltip_show_impl(int canvas_id, const char* text,
                                   double cx, double cy) {
    if (!text || !text[0]) { aether_ui_vg_tooltip_hide_impl(); return 0; }
    int cwidget = aether_ui_canvas_get_widget(canvas_id);
    Widget* cw = widget_at(cwidget);
    if (!cw || app_count < 1 || !apps[0].hwnd) return 0;

    // Canvas-local → app-window client coords; sit below-right of the
    // pointer like a native tip.
    POINT p = { (int)cx + 12, (int)cy + 18 };
    MapWindowPoints(cw->hwnd, apps[0].hwnd, &p, 1);

    if (w32_tooltip_overlay
        && aether_ui_overlay_is_live_impl(w32_tooltip_overlay)
        && w32_tooltip_label) {
        // Live: retext + reposition the existing overlay content.
        Widget* lbl = widget_at(w32_tooltip_label);
        if (lbl) {
            aether_ui_text_set_string(w32_tooltip_label, text);
            int tw = 0, th = 0;
            measure_widget(lbl, &tw, &th);
            SetWindowPos(lbl->hwnd, HWND_TOP, p.x, p.y, tw + 12, th + 8,
                         SWP_SHOWWINDOW);
        }
        return w32_tooltip_overlay;
    }
    w32_tooltip_label = aether_ui_text_create(text);
    // anchor 0 = top-start; dx/dy are the absolute client position.
    w32_tooltip_overlay = aether_ui_overlay_open_impl(0, w32_tooltip_label,
                                                      0, p.x, p.y, 0);
    return w32_tooltip_overlay;
}

void aether_ui_vg_tooltip_hide_impl(void) {
    if (w32_tooltip_overlay
        && aether_ui_overlay_is_live_impl(w32_tooltip_overlay)) {
        aether_ui_overlay_close_impl(w32_tooltip_overlay);
    }
}

// The drawn tooltip path is opt-in via $AETHER_UI_TOOLTIP=drawn (the same
// switch GTK honors; win32 has no native vg-shape tooltip to prefer).
int aether_ui_vg_tooltip_drawn_impl(void) {
    const char* v = getenv("AETHER_UI_TOOLTIP");
    return v && strcmp(v, "drawn") == 0;
}

void aether_ui_window_show_impl(int win_handle) {
    Widget* w = widget_at(win_handle);
    if (w) { ShowWindow(w->hwnd, SW_SHOW); UpdateWindow(w->hwnd); }
}

void aether_ui_window_close_impl(int win_handle) {
    Widget* w = widget_at(win_handle);
    if (w) DestroyWindow(w->hwnd);
}

int aether_ui_sheet_create_impl(const char* title, int width, int height) {
    return aether_ui_window_create_impl(title, width, height);
}

void aether_ui_sheet_set_body_impl(int handle, int root_handle) {
    aether_ui_window_set_body_impl(handle, root_handle);
}

void aether_ui_sheet_present_impl(int handle) {
    aether_ui_window_show_impl(handle);
}

void aether_ui_sheet_dismiss_impl(int handle) {
    aether_ui_window_close_impl(handle);
}

// GDI+ flat-API image loaders — used to pick up PNG/JPEG/GIF/BMP/TIFF at
// runtime without pulling in the C++ GDI+ headers. The returned HBITMAP
// owns its pixels and can be passed straight to STM_SETIMAGE.
__declspec(dllimport) int __stdcall GdipCreateBitmapFromFile(
    const wchar_t* filename, void** bitmap);
__declspec(dllimport) int __stdcall GdipCreateHBITMAPFromBitmap(
    void* bitmap, HBITMAP* hbm, unsigned int background_argb);
__declspec(dllimport) int __stdcall GdipDisposeImage(void* image);

// Load any format GDI+ supports (PNG, JPEG, GIF, BMP, TIFF, ICO) into an
// HBITMAP suitable for SS_BITMAP. Falls back to LoadImageW for plain BMPs
// when GDI+ initialization fails or isn't available.
static HBITMAP load_image_any_format(const wchar_t* filename) {
    ensure_gdiplus();
    if (gdiplus_started) {
        void* gdi_bitmap = NULL;
        if (GdipCreateBitmapFromFile(filename, &gdi_bitmap) == 0 && gdi_bitmap) {
            HBITMAP hbm = NULL;
            // Transparent background (0x00000000 = ARGB all zero).
            GdipCreateHBITMAPFromBitmap(gdi_bitmap, &hbm, 0);
            GdipDisposeImage(gdi_bitmap);
            if (hbm) return hbm;
        }
    }
    // BMP-only fallback.
    return (HBITMAP)LoadImageW(NULL, filename, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
}

int aether_ui_image_create(const char* filepath) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_BITMAP,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    HBITMAP bmp = load_image_any_format(utf8_to_wide(filepath));
    if (bmp) SendMessageW(h, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bmp);
    return register_widget_typed(h, WK_IMAGE);
}

void aether_ui_image_set_size(int handle, int width, int height) {
    Widget* w = widget_at(handle);
    if (w) { w->pref_width = width; w->pref_height = height; }
}

// ---------------------------------------------------------------------------
// Menu bar + context menus.
//
// Win32 menus (HMENU) are not HWNDs, so they live in a parallel registry.
// Each menu item is a small command ID (starting at AE_MENU_ID_BASE) that
// we dispatch from app_wnd_proc's WM_COMMAND: the ID looks up the
// registered closure and invokes it. Menu bars attach to the app window
// via SetMenu(); context menus use TrackPopupMenu().
// ---------------------------------------------------------------------------

typedef struct {
    HMENU    hmenu;
    int      is_menu_bar;   // 1 = menu bar (top-level); 0 = popup/submenu
    int      attached;      // 1 once this menu has been added to a parent bar
    wchar_t* label;         // display text for submenus (NULL for menu bars)
} MenuEntry;

static MenuEntry* menus = NULL;
static int        menu_count = 0;
static int        menu_capacity = 0;

typedef struct {
    UINT       id;
    AeClosure* closure;
} MenuCommand;

static MenuCommand* menu_commands = NULL;
static int          menu_command_count = 0;
static int          menu_command_capacity = 0;
static UINT         next_menu_id = AE_MENU_ID_BASE;

static int register_menu(HMENU hmenu, int is_bar, const wchar_t* label) {
    if (menu_count >= menu_capacity) {
        menu_capacity = menu_capacity == 0 ? 8 : menu_capacity * 2;
        menus = (MenuEntry*)realloc(menus, sizeof(MenuEntry) * menu_capacity);
    }
    menus[menu_count].hmenu = hmenu;
    menus[menu_count].is_menu_bar = is_bar;
    menus[menu_count].attached = 0;
    menus[menu_count].label = label ? _wcsdup(label) : NULL;
    menu_count++;
    return menu_count; // 1-based
}

static MenuEntry* menu_at(int handle) {
    if (handle < 1 || handle > menu_count) return NULL;
    return &menus[handle - 1];
}

// Called from the app window's WM_COMMAND when LOWORD(wParam) is a menu ID.
// Returns 1 if handled.
static int menu_dispatch_command(UINT id) {
    for (int i = 0; i < menu_command_count; i++) {
        if (menu_commands[i].id == id) {
            invoke_closure(menu_commands[i].closure);
            return 1;
        }
    }
    return 0;
}

int aether_ui_menu_bar_create(void) {
    ensure_win_init();
    HMENU hmenu = CreateMenu();
    return register_menu(hmenu, 1, NULL);
}

int aether_ui_menu_create(const char* label) {
    ensure_win_init();
    HMENU hmenu = CreatePopupMenu();
    return register_menu(hmenu, 0, utf8_to_wide(label ? label : "Menu"));
}

void aether_ui_menu_add_item(int menu_handle, const char* label,
                             void* boxed_closure) {
    MenuEntry* m = menu_at(menu_handle);
    if (!m) return;
    UINT id = next_menu_id++;
    AppendMenuW(m->hmenu, MF_STRING, id, utf8_to_wide(label));
    if (menu_command_count >= menu_command_capacity) {
        menu_command_capacity = menu_command_capacity == 0
                                ? 32 : menu_command_capacity * 2;
        menu_commands = (MenuCommand*)realloc(menu_commands,
            sizeof(MenuCommand) * menu_command_capacity);
    }
    menu_commands[menu_command_count].id = id;
    menu_commands[menu_command_count].closure = (AeClosure*)boxed_closure;
    menu_command_count++;
    // Side-store for the AetherUIDriver /tray/{id}/menu/activate route.
    aether_ui_menu_item_record(menu_handle, label, boxed_closure);
}

void aether_ui_menu_add_separator(int menu_handle) {
    MenuEntry* m = menu_at(menu_handle);
    if (m) AppendMenuW(m->hmenu, MF_SEPARATOR, 0, NULL);
}

void aether_ui_menu_bar_add_menu(int bar_handle, int menu_handle) {
    MenuEntry* bar = menu_at(bar_handle);
    MenuEntry* sub = menu_at(menu_handle);
    if (!bar || !sub) return;
    const wchar_t* label = sub->label ? sub->label : L"Menu";
    AppendMenuW(bar->hmenu, MF_POPUP, (UINT_PTR)sub->hmenu, label);
    sub->attached = 1;
}

void aether_ui_menu_bar_attach(int app_handle, int bar_handle) {
    if (app_handle < 1 || app_handle > app_count) return;
    MenuEntry* bar = menu_at(bar_handle);
    if (!bar) return;
    AppEntry* e = &apps[app_handle - 1];
    // aether_ui_app_run_raw hasn't created the window yet in the common flow,
    // so stash the menu bar and attach it at show-time.
    e->pending_menu = bar->hmenu;
}

void aether_ui_menu_popup(int menu_handle, int anchor_widget) {
    // TrackPopupMenu runs its own modal message loop and only returns
    // when the menu is dismissed by a click or Escape. In headless
    // contexts (widget smoke tests, CI without a window server) the
    // call would block indefinitely — no user input, no outer message
    // pump to dismiss. Respect AETHER_UI_HEADLESS and also require a
    // visible ancestor window as a second guard for programming errors
    // that pop a menu from an unmounted widget.
    if (aeui_is_headless()) return;
    MenuEntry* m = menu_at(menu_handle);
    Widget* w = widget_at(anchor_widget);
    if (!m || !w || !w->hwnd) return;
    HWND owner = GetParent(w->hwnd);
    if (!owner || !IsWindowVisible(owner)) return;
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(m->hmenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                   pt.x, pt.y, 0, owner, NULL);
}

// ---------------------------------------------------------------------------
// Grid layout.
//
// A 2D container where children claim (row, col) cells with optional row
// and column spans. Columns are equal-width by default; rows size to their
// tallest child. Matches GtkGrid / NSGridView semantics.
// ---------------------------------------------------------------------------

typedef struct {
    HWND hwnd;
    int  cols;
    int  row_spacing;
    int  col_spacing;
    struct {
        HWND hwnd;
        int  row, col, row_span, col_span;
    } items[64];
    int item_count;
} GridEntry;

static GridEntry** grids = NULL;
static int         grid_count = 0;
static int         grid_capacity = 0;

static GridEntry* grid_for_hwnd(HWND hwnd) {
    for (int i = 0; i < grid_count; i++) {
        if (grids[i] && grids[i]->hwnd == hwnd) return grids[i];
    }
    return NULL;
}

static void grid_do_layout(HWND hwnd) {
    GridEntry* g = grid_for_hwnd(hwnd);
    if (!g || g->item_count == 0) return;

    RECT client;
    GetClientRect(hwnd, &client);
    int total_w = client.right - client.left;
    int total_h = client.bottom - client.top;

    // Determine row count from max row index.
    int rows = 0;
    for (int i = 0; i < g->item_count; i++) {
        int r = g->items[i].row + g->items[i].row_span;
        if (r > rows) rows = r;
    }
    if (rows == 0) return;

    int col_w = (total_w - g->col_spacing * (g->cols - 1)) / g->cols;
    int row_h = (total_h - g->row_spacing * (rows - 1)) / rows;
    if (col_w < 0) col_w = 0;
    if (row_h < 0) row_h = 0;

    for (int i = 0; i < g->item_count; i++) {
        int x = g->items[i].col * (col_w + g->col_spacing);
        int y = g->items[i].row * (row_h + g->row_spacing);
        int w = col_w * g->items[i].col_span
                + g->col_spacing * (g->items[i].col_span - 1);
        int h = row_h * g->items[i].row_span
                + g->row_spacing * (g->items[i].row_span - 1);
        SetWindowPos(g->items[i].hwnd, NULL, x, y, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static LRESULT CALLBACK grid_wnd_proc(HWND hwnd, UINT msg,
                                       WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            grid_do_layout(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return stack_wnd_proc(hwnd, msg, wp, lp);
        case WM_COMMAND:
        case WM_HSCROLL:
        case WM_VSCROLL:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return stack_wnd_proc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int aether_ui_grid_create(int cols, int row_spacing, int col_spacing) {
    ensure_win_init();
    if (cols < 1) cols = 1;
    HWND hwnd = CreateWindowExW(0, GRID_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 0;
    if (grid_count >= grid_capacity) {
        grid_capacity = grid_capacity == 0 ? 4 : grid_capacity * 2;
        grids = (GridEntry**)realloc(grids, sizeof(GridEntry*) * grid_capacity);
    }
    GridEntry* g = (GridEntry*)calloc(1, sizeof(GridEntry));
    g->hwnd = hwnd;
    g->cols = cols;
    g->row_spacing = row_spacing;
    g->col_spacing = col_spacing;
    grids[grid_count++] = g;
    return register_widget_typed(hwnd, WK_GRID);
}

void aether_ui_grid_place(int grid_handle, int child_handle,
                          int row, int col, int row_span, int col_span) {
    Widget* g = widget_at(grid_handle);
    Widget* c = widget_at(child_handle);
    if (!g || !c || g->kind != WK_GRID) return;
    GridEntry* ge = grid_for_hwnd(g->hwnd);
    if (!ge || ge->item_count >= 64) return;
    if (row_span < 1) row_span = 1;
    if (col_span < 1) col_span = 1;
    // Reparent the child to the grid.
    SetParent(c->hwnd, g->hwnd);
    LONG_PTR st = GetWindowLongPtrW(c->hwnd, GWL_STYLE);
    SetWindowLongPtrW(c->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
    ge->items[ge->item_count].hwnd = c->hwnd;
    ge->items[ge->item_count].row = row;
    ge->items[ge->item_count].col = col;
    ge->items[ge->item_count].row_span = row_span;
    ge->items[ge->item_count].col_span = col_span;
    ge->item_count++;
    grid_do_layout(g->hwnd);
}

// ---------------------------------------------------------------------------
// Canvas backend — GDI+ flat-API path, minimal for first pass.
//
// Paths are recorded as a command stream; WM_PAINT replays them onto an
// offscreen bitmap then BitBlt-s to screen. Supports begin_path, move_to,
// line_to, stroke, fill_rect, clear.
// ---------------------------------------------------------------------------
typedef enum {
    CV_BEGIN, CV_MOVE, CV_LINE, CV_STROKE, CV_FILL_RECT, CV_CLEAR,
    CV_ARC, CV_CLOSE, CV_FILL, CV_FILL_TEXT, CV_DRAW_IMAGE,
    CV_FILL_LINEAR, CV_FILL_RADIAL
} CanvasCmdKind;

typedef struct {
    CanvasCmdKind k;
    // p0..p3 carry geometry: x1,y1,x2,y2 for lines; x,y,w,h for rects;
    // line width in p0 for stroke commands; ARC: cx,cy,radius,(unused);
    // FILL_TEXT: x,y,font_size,(unused); DRAW_IMAGE: x,y,(unused),(unused).
    float p0, p1, p2, p3;
    float a0, a1;          // ARC start/end angle (radians)
    float cr, cg, cb, calpha;
    char* text;            // FILL_TEXT string (owned)
    unsigned char* pixels; // DRAW_IMAGE RGBA8888 buffer (owned)
    int iw, ih;            // DRAW_IMAGE pixel dims
    // Gradient: linear (gx1,gy1)→(gx2,gy2); radial center (gx1,gy1) r gr.
    float gx1, gy1, gx2, gy2, gr, gfx, gfy;
    float grad_line_width;  // 0 → fill; >0 → stroke (GDI approximates as fill)
    int n_stops;
    double* stop_off;      // owned
    double* stop_rgba;     // owned: n_stops*4
} CanvasCmd;

typedef struct {
    HWND hwnd;
    int width, height;
    CanvasCmd* cmds;
    int cmd_count;
    int cmd_cap;
    float cur_x, cur_y;
    AeClosure* on_move;    // pointer-move hook (canvas-local x,y); null = none
    AeClosure* on_click;   // pointer-press hook (canvas-local x,y)
    AeClosure* on_release; // pointer-release hook (canvas-local x,y)
    AeClosure* on_key;     // key-press hook (GDK key name string)
} Canvas;

static Canvas* canvases = NULL;
static int canvas_count = 0;
static int canvas_cap = 0;

// The most recently key-registered canvas, focused after the window shows
// (registration-time focus can't stick — the canvas is still on the hidden
// holder then; mirrors GTK's "focus on map").
static int pending_key_canvas = 0;

static void aeui_focus_pending_key_canvas(void) {
    if (pending_key_canvas >= 1 && pending_key_canvas <= canvas_count) {
        SetFocus(canvases[pending_key_canvas - 1].hwnd);
    }
}

static int aeui_hwnd_is_key_canvas(HWND hwnd) {
    return pending_key_canvas >= 1 && pending_key_canvas <= canvas_count &&
           hwnd == canvases[pending_key_canvas - 1].hwnd;
}

static void canvas_free_text(int canvas_id);  // fwd; frees FILL_TEXT strings

static int canvas_id_for_hwnd(HWND hwnd) {
    for (int i = 0; i < canvas_count; i++) {
        if (canvases[i].hwnd == hwnd) return i + 1;
    }
    return 0;
}

static void canvas_add_cmd(int canvas_id, CanvasCmd cmd) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* c = &canvases[canvas_id - 1];
    if (c->cmd_count >= c->cmd_cap) {
        c->cmd_cap = c->cmd_cap == 0 ? 128 : c->cmd_cap * 2;
        c->cmds = (CanvasCmd*)realloc(c->cmds, sizeof(CanvasCmd) * c->cmd_cap);
    }
    c->cmds[c->cmd_count++] = cmd;
}

int aether_ui_canvas_create_impl(int width, int height) {
    ensure_win_init();
    HWND h = CreateWindowExW(0, CANVAS_CLASS, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, widget_holder, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return 0;
    if (canvas_count >= canvas_cap) {
        canvas_cap = canvas_cap == 0 ? 8 : canvas_cap * 2;
        canvases = (Canvas*)realloc(canvases, sizeof(Canvas) * canvas_cap);
    }
    Canvas* cv = &canvases[canvas_count++];
    cv->hwnd = h;
    cv->width = width;
    cv->height = height;
    cv->cmds = NULL;
    cv->cmd_count = 0;
    cv->cmd_cap = 0;
    cv->cur_x = cv->cur_y = 0;
    cv->on_move = NULL;
    cv->on_click = NULL;
    cv->on_release = NULL;
    cv->on_key = NULL;
    int widget_handle = register_widget_typed(h, WK_CANVAS);
    Widget* ww = widget_at(widget_handle);
    if (ww) {
        ww->pref_width = width;
        ww->pref_height = height;
        ww->u.canvas.canvas_id = canvas_count;
    }
    return canvas_count;
}

int aether_ui_canvas_get_widget(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return 0;
    return handle_for_hwnd(canvases[canvas_id - 1].hwnd);
}

// Resize hook — honest stub. Win32 WM_SIZE → re-map + re-flush is not wired
// yet; the vg scene renders at its initial size and does not rescale on window
// resize on Win32. Tracked for parity with the GTK backend.
void aether_ui_canvas_on_resize_impl(int canvas_id, void* boxed_closure) {
    (void)canvas_id; (void)boxed_closure;
}

// Pointer-press on a canvas: WM_LBUTTONDOWN → (x,y) into the closure, and
// the driver's POST /canvas/{id}/click drives the same hook. Mirrors the
// GTK4 GtkGestureClick "pressed" path.
void aether_ui_canvas_on_click_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_click = (AeClosure*)boxed_closure;
}

void aether_ui_canvas_on_move_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_move = (AeClosure*)boxed_closure;
}

// Keyboard input on a canvas: WM_KEYDOWN → GDK-style key name string into
// the closure (mirrors the GTK4 GtkEventControllerKey path — the app-side
// key handlers compare against GDK names like "Left"/"Return"/"Escape").
// The canvas is made a tab stop so real keystrokes reach it once focused;
// the driver's POST /canvas/{id}/key drives the same hook without focus.
void aether_ui_canvas_on_key_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    Canvas* cv = &canvases[canvas_id - 1];
    cv->on_key = (AeClosure*)boxed_closure;
    LONG_PTR st = GetWindowLongPtrW(cv->hwnd, GWL_STYLE);
    SetWindowLongPtrW(cv->hwnd, GWL_STYLE, st | WS_TABSTOP);
    // Focus is deferred to app-show: at registration the canvas is still
    // parented to the hidden holder, so SetFocus wouldn't stick (mirrors the
    // GTK "focus on map" lesson). The window-show path grabs it once the
    // window is up; clicking the canvas also focuses it (WM_LBUTTONDOWN).
    pending_key_canvas = canvas_id;
}

// Pointer-release on a canvas: WM_LBUTTONUP → (x,y) into the closure.
void aether_ui_canvas_on_release_impl(int canvas_id, void* boxed_closure) {
    if (canvas_id < 1 || canvas_id > canvas_count || !boxed_closure) return;
    canvases[canvas_id - 1].on_release = (AeClosure*)boxed_closure;
}

// begin_path starts a fresh command stream — drop any previously-recorded
// commands so a redraw-per-frame loop doesn't accumulate unboundedly.
// Previously this was an append-only op, which meant an animated canvas
// leaked ~16 bytes/cmd * 60Hz forever (hundreds of MB/hr on busy scenes).
void aether_ui_canvas_begin_path_impl(int canvas_id) {
    if (canvas_id >= 1 && canvas_id <= canvas_count) {
        canvas_free_text(canvas_id);
        canvases[canvas_id - 1].cmd_count = 0;
    }
    CanvasCmd c = {0}; c.k = CV_BEGIN;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_move_to_impl(int canvas_id, double x, double y) {
    CanvasCmd c = {0}; c.k = CV_MOVE; c.p0 = x; c.p1 = y;
    canvas_add_cmd(canvas_id, c);
    if (canvas_id >= 1 && canvas_id <= canvas_count) {
        canvases[canvas_id - 1].cur_x = x;
        canvases[canvas_id - 1].cur_y = y;
    }
}

void aether_ui_canvas_line_to_impl(int canvas_id, double x, double y) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* cv = &canvases[canvas_id - 1];
    CanvasCmd c = {0}; c.k = CV_LINE;
    c.p0 = cv->cur_x; c.p1 = cv->cur_y; c.p2 = x; c.p3 = y;
    canvas_add_cmd(canvas_id, c);
    cv->cur_x = x; cv->cur_y = y;
}

void aether_ui_canvas_stroke_impl(int canvas_id, double r, double g, double b,
                                   double a, double line_width, int cap, int join) {
    (void)cap; (void)join; // stroke cap/join not yet honored on GDI+ (round default)
    CanvasCmd c = {0};
    c.k = CV_STROKE; c.cr = r; c.cg = g; c.cb = b; c.calpha = a; c.p0 = line_width;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_rect_impl(int canvas_id, double x, double y,
                                      double w, double h,
                                      double r, double g, double b, double a) {
    CanvasCmd c = {0};
    c.k = CV_FILL_RECT; c.p0 = x; c.p1 = y; c.p2 = w; c.p3 = h;
    c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    canvas_add_cmd(canvas_id, c);
}

// Viewport clip — no-op on Win32 for now (GTK-verified feature).
void aether_ui_canvas_clip_rect_impl(int canvas_id, double x, double y,
                                      double w, double h) {
    (void)canvas_id; (void)x; (void)y; (void)w; (void)h;
}

void aether_ui_canvas_arc_impl(int canvas_id, double cx, double cy, double radius,
                                double start_angle, double end_angle) {
    CanvasCmd c = {0};
    c.k = CV_ARC; c.p0 = cx; c.p1 = cy; c.p2 = radius;
    c.a0 = start_angle; c.a1 = end_angle;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_close_path_impl(int canvas_id) {
    CanvasCmd c = {0}; c.k = CV_CLOSE;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_impl(int canvas_id, double r, double g, double b, double a) {
    CanvasCmd c = {0};
    c.k = CV_FILL; c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_text_impl(int canvas_id, const char* text,
                                      double x, double y, double font_size,
                                      int font_flags,
                                      double r, double g, double b, double a) {
    (void)font_flags;   // font-family selection not yet wired on Win32
    CanvasCmd c = {0};
    c.k = CV_FILL_TEXT; c.p0 = x; c.p1 = y; c.p2 = font_size;
    c.cr = r; c.cg = g; c.cb = b; c.calpha = a;
    c.text = text ? _strdup(text) : NULL;
    canvas_add_cmd(canvas_id, c);
}

// Stroke (outline) text — STUB. GDI text-outline needs a Path via
// BeginPath/TextOut/EndPath then StrokePath; deferred to when we're next on
// winbaz. No-op keeps the ABI linkable (the GTK4 backend is real).
void aether_ui_canvas_stroke_text_impl(int canvas_id, const char* text,
                                        double x, double y, double font_size,
                                        double line_width, int font_flags,
                                        double r, double g, double b, double a) {
    (void)canvas_id; (void)text; (void)x; (void)y; (void)font_size;
    (void)line_width; (void)font_flags; (void)r; (void)g; (void)b; (void)a;
}

// Text metrics — STUB (returns zeros). Win32 has real metrics via GDI
// (GetTextExtentPoint32 / GetTextMetrics); wiring them is deferred to when
// we're next on winbaz. Zeros keep the ABI linkable so vg.text_extent
// compiles everywhere (GTK4 has the real impl).
double aether_ui_text_measure(double size, const char* text) { (void)size; (void)text; return 0.0; }
double aether_ui_font_ascent(double size)  { (void)size; return 0.0; }
double aether_ui_font_descent(double size) { (void)size; return 0.0; }
double aether_ui_font_height(double size)  { (void)size; return 0.0; }

void aether_ui_canvas_draw_image_impl(int canvas_id, double x, double y,
                                       int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    if (iw <= 0 || ih <= 0 || !rgba) return;
    if (byte_len < iw * ih * 4) return;
    unsigned char* owned = (unsigned char*)malloc(iw * ih * 4);
    if (!owned) return;
    memcpy(owned, rgba, iw * ih * 4);
    CanvasCmd c = {0};
    c.k = CV_DRAW_IMAGE; c.p0 = x; c.p1 = y;
    c.pixels = owned; c.iw = iw; c.ih = ih;
    canvas_add_cmd(canvas_id, c);
}

// Scaled draw — Win32 blits 1:1 for now (ignores dw/dh); the GTK backend
// scales. Keeps the ABI total; proper Win32 scaling is a later port.
void aether_ui_canvas_draw_image_scaled_impl(int canvas_id, double x, double y,
                                       double dw, double dh, int iw, int ih,
                                       const unsigned char* rgba, int byte_len) {
    (void)dw; (void)dh;
    aether_ui_canvas_draw_image_impl(canvas_id, x, y, iw, ih, rgba, byte_len);
}

extern double floatarr_get_unchecked(void* arr, int i);

static void win32_copy_stops(CanvasCmd* c, int n_stops,
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
    (void)extend; // spreadMethod not yet honored on the GDI+ backend
    CanvasCmd c = {0};
    c.k = CV_FILL_LINEAR; c.gx1 = x1; c.gy1 = y1; c.gx2 = x2; c.gy2 = y2;
    c.grad_line_width = line_width;
    win32_copy_stops(&c, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_fill_radial_gradient_impl(int canvas_id,
        double cx, double cy, double radius, double fx, double fy,
        int n_stops, void* offsets, void* rgba, double line_width, int extend) {
    (void)extend; // spreadMethod not yet honored on the GDI+ backend
    CanvasCmd c = {0};
    c.k = CV_FILL_RADIAL; c.gx1 = cx; c.gy1 = cy; c.gr = radius;
    c.gfx = fx; c.gfy = fy; c.grad_line_width = line_width;
    win32_copy_stops(&c, n_stops, offsets, rgba);
    canvas_add_cmd(canvas_id, c);
}

// Free owned per-command buffers (FILL_TEXT strings, DRAW_IMAGE
// pixels, gradient stop arrays) before a cmd-buffer reset.
static void canvas_free_text(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    Canvas* cv = &canvases[canvas_id - 1];
    for (int i = 0; i < cv->cmd_count; i++) {
        CanvasCmd* c = &cv->cmds[i];
        if (c->k == CV_FILL_TEXT && c->text) {
            free(c->text); c->text = NULL;
        }
        if (c->k == CV_DRAW_IMAGE && c->pixels) {
            free(c->pixels); c->pixels = NULL;
        }
        if (c->k == CV_FILL_LINEAR || c->k == CV_FILL_RADIAL) {
            free(c->stop_off);  c->stop_off = NULL;
            free(c->stop_rgba); c->stop_rgba = NULL;
        }
    }
}

void aether_ui_canvas_clear_impl(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    canvas_free_text(canvas_id);
    canvases[canvas_id - 1].cmd_count = 0;
    CanvasCmd c = {0}; c.k = CV_CLEAR;
    canvas_add_cmd(canvas_id, c);
}

void aether_ui_canvas_redraw_impl(int canvas_id) {
    if (canvas_id < 1 || canvas_id > canvas_count) return;
    InvalidateRect(canvases[canvas_id - 1].hwnd, NULL, TRUE);
}

// canvas_write_png — off-screen PNG render. NOT YET IMPLEMENTED on Win32:
// would replay the command buffer into a memory DC / GDI+ Bitmap and
// Bitmap::Save with the PNG encoder CLSID. Returns 0 (failure) so callers
// detect the gap. Tracked follow-up; the GTK4 backend has the working impl.
int aether_ui_canvas_write_png_impl(int canvas_id, const char* path,
                                     int width, int height) {
    (void)canvas_id; (void)path; (void)width; (void)height;
    return 0;
}

static void canvas_paint(HWND hwnd, HDC hdc, int width, int height) {
    int id = canvas_id_for_hwnd(hwnd);
    if (id == 0) return;
    Canvas* cv = &canvases[id - 1];
    // Plain GDI replay — GDI+ bindings from C are clunky; for a first
    // pass this delivers lines + filled rects with correct pixels.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem, bmp);
    // White background
    RECT full = { 0, 0, width, height };
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mem, &full, white);

    HPEN cur_pen = NULL;
    HPEN old_pen = (HPEN)SelectObject(mem, GetStockObject(BLACK_PEN));

    for (int i = 0; i < cv->cmd_count; i++) {
        CanvasCmd* cmd = &cv->cmds[i];
        switch (cmd->k) {
            case CV_CLEAR:
                FillRect(mem, &full, white);
                break;
            case CV_LINE: {
                MoveToEx(mem, (int)cmd->p0, (int)cmd->p1, NULL);
                LineTo(mem, (int)cmd->p2, (int)cmd->p3);
                break;
            }
            case CV_STROKE: {
                if (cur_pen) DeleteObject(cur_pen);
                int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                    bi = (int)(cmd->cb * 255);
                cur_pen = CreatePen(PS_SOLID, (int)cmd->p0, RGB(ri, gi, bi));
                SelectObject(mem, cur_pen);
                break;
            }
            case CV_FILL_RECT: {
                int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                    bi = (int)(cmd->cb * 255);
                HBRUSH br = CreateSolidBrush(RGB(ri, gi, bi));
                RECT r = { (int)cmd->p0, (int)cmd->p1,
                           (int)(cmd->p0 + cmd->p2), (int)(cmd->p1 + cmd->p3) };
                FillRect(mem, &r, br);
                DeleteObject(br);
                break;
            }
            case CV_ARC: {
                // Outline the circle/arc. GDI's Arc takes a bounding box
                // + two radial points; for a full circle (a0=0,a1=2π)
                // we use Ellipse. Stroke colour comes from the current pen.
                int cx = (int)cmd->p0, cy = (int)cmd->p1, rad = (int)cmd->p2;
                Ellipse(mem, cx - rad, cy - rad, cx + rad, cy + rad);
                break;
            }
            case CV_CLOSE:
                break;  // polygon close handled in CV_FILL accumulation
            case CV_FILL: {
                // Accumulate the points from MOVE/LINE/ARC commands since
                // the last BEGIN into a polygon, fill with the given color.
                POINT pts[256];
                int np = 0;
                for (int j = i - 1; j >= 0 && np < 256; j--) {
                    CanvasCmdKind k = cv->cmds[j].k;
                    if (k == CV_BEGIN) break;
                    if (k == CV_MOVE) {
                        pts[np].x = (int)cv->cmds[j].p0;
                        pts[np].y = (int)cv->cmds[j].p1;
                        np++;
                    } else if (k == CV_LINE) {
                        pts[np].x = (int)cv->cmds[j].p2;
                        pts[np].y = (int)cv->cmds[j].p3;
                        np++;
                    }
                }
                if (np >= 3) {
                    int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                        bi = (int)(cmd->cb * 255);
                    HBRUSH br = CreateSolidBrush(RGB(ri, gi, bi));
                    HBRUSH oldbr = (HBRUSH)SelectObject(mem, br);
                    Polygon(mem, pts, np);
                    SelectObject(mem, oldbr);
                    DeleteObject(br);
                }
                break;
            }
            case CV_FILL_LINEAR:
            case CV_FILL_RADIAL: {
                // Plain GDI has no multi-stop gradient. Approximate:
                // fill the path's region with a 2-stop GradientFill
                // (first → last stop) across the path bounding box,
                // clipped to the accumulated path. Radial degrades to
                // the same axis-aligned approximation. Good enough for
                // a first pass; GDI+ would be needed for true fidelity.
                if (cmd->n_stops >= 1) {
                    // Bounding box from the points accumulated since BEGIN.
                    int have = 0, mnx = 0, mny = 0, mxx = 0, mxy = 0;
                    for (int j = i - 1; j >= 0; j--) {
                        CanvasCmdKind k = cv->cmds[j].k;
                        if (k == CV_BEGIN) break;
                        int qx, qy, ok = 0;
                        if (k == CV_MOVE) { qx=(int)cv->cmds[j].p0; qy=(int)cv->cmds[j].p1; ok=1; }
                        else if (k == CV_LINE) { qx=(int)cv->cmds[j].p2; qy=(int)cv->cmds[j].p3; ok=1; }
                        if (ok) {
                            if (!have) { mnx=mxx=qx; mny=mxy=qy; have=1; }
                            else { if(qx<mnx)mnx=qx; if(qx>mxx)mxx=qx; if(qy<mny)mny=qy; if(qy>mxy)mxy=qy; }
                        }
                    }
                    if (have && mxx > mnx && mxy > mny) {
                        int s0 = 0, s1 = cmd->n_stops - 1;
                        TRIVERTEX v[2];
                        v[0].x = mnx; v[0].y = mny;
                        v[0].Red   = (COLOR16)(cmd->stop_rgba[s0*4+0]*65535);
                        v[0].Green = (COLOR16)(cmd->stop_rgba[s0*4+1]*65535);
                        v[0].Blue  = (COLOR16)(cmd->stop_rgba[s0*4+2]*65535);
                        v[0].Alpha = 0;
                        v[1].x = mxx; v[1].y = mxy;
                        v[1].Red   = (COLOR16)(cmd->stop_rgba[s1*4+0]*65535);
                        v[1].Green = (COLOR16)(cmd->stop_rgba[s1*4+1]*65535);
                        v[1].Blue  = (COLOR16)(cmd->stop_rgba[s1*4+2]*65535);
                        v[1].Alpha = 0;
                        GRADIENT_RECT gr = { 0, 1 };
                        GradientFill(mem, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
                    }
                }
                break;
            }
            case CV_FILL_TEXT: {
                if (cmd->text) {
                    int ri = (int)(cmd->cr * 255), gi = (int)(cmd->cg * 255),
                        bi = (int)(cmd->cb * 255);
                    HFONT font = CreateFontA((int)cmd->p2, 0, 0, 0, FW_NORMAL,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, NULL);
                    HFONT oldfont = (HFONT)SelectObject(mem, font);
                    SetTextColor(mem, RGB(ri, gi, bi));
                    SetBkMode(mem, TRANSPARENT);
                    // cairo origin is baseline; GDI TextOut is top-left.
                    // Offset up by ~font ascent (≈0.8 of size) for parity.
                    TextOutA(mem, (int)cmd->p0, (int)(cmd->p1 - cmd->p2 * 0.8f),
                             cmd->text, (int)strlen(cmd->text));
                    SelectObject(mem, oldfont);
                    DeleteObject(font);
                }
                break;
            }
            case CV_DRAW_IMAGE: {
                if (cmd->pixels && cmd->iw > 0 && cmd->ih > 0) {
                    // Source is RGBA8888 top-down. Build a top-down
                    // 32bpp BGRA DIB (negative height) and StretchDIBits.
                    // GDI ignores alpha for plain blits; swizzle R/B and
                    // (approximately) composite onto the white backing by
                    // premultiplying against white where alpha < 255.
                    int n = cmd->iw * cmd->ih;
                    unsigned char* conv = (unsigned char*)malloc(n * 4);
                    if (conv) {
                        for (int px = 0; px < n; px++) {
                            unsigned char sr = cmd->pixels[px*4+0];
                            unsigned char sg = cmd->pixels[px*4+1];
                            unsigned char sb = cmd->pixels[px*4+2];
                            unsigned char sa = cmd->pixels[px*4+3];
                            int inv = 255 - sa;
                            conv[px*4+0] = (unsigned char)((sb*sa + 255*inv)/255); // B
                            conv[px*4+1] = (unsigned char)((sg*sa + 255*inv)/255); // G
                            conv[px*4+2] = (unsigned char)((sr*sa + 255*inv)/255); // R
                            conv[px*4+3] = 255;
                        }
                        BITMAPINFO bi = {0};
                        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bi.bmiHeader.biWidth = cmd->iw;
                        bi.bmiHeader.biHeight = -cmd->ih; // top-down
                        bi.bmiHeader.biPlanes = 1;
                        bi.bmiHeader.biBitCount = 32;
                        bi.bmiHeader.biCompression = BI_RGB;
                        StretchDIBits(mem, (int)cmd->p0, (int)cmd->p1,
                            cmd->iw, cmd->ih, 0, 0, cmd->iw, cmd->ih,
                            conv, &bi, DIB_RGB_COLORS, SRCCOPY);
                        free(conv);
                    }
                }
                break;
            }
            default: break;
        }
    }

    BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old_pen);
    if (cur_pen) DeleteObject(cur_pen);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Map a Win32 virtual-key code to the GDK key name the app-side handlers
// expect (gp/falling_blocks compare against GDK's "Left"/"Return"/"space"…).
// Returns a static string; writes single letters/digits into `buf`.
static const char* vk_to_gdk_name(WPARAM vk, char* buf, int buflen) {
    switch (vk) {
        case VK_LEFT:   return "Left";
        case VK_RIGHT:  return "Right";
        case VK_UP:     return "Up";
        case VK_DOWN:   return "Down";
        case VK_RETURN: return "Return";
        case VK_ESCAPE: return "Escape";
        case VK_DELETE: return "Delete";
        case VK_BACK:   return "BackSpace";
        case VK_TAB:    return "Tab";
        case VK_SPACE:  return "space";
        case VK_HOME:   return "Home";
        case VK_END:    return "End";
        case VK_PRIOR:  return "Page_Up";
        case VK_NEXT:   return "Page_Down";
        default: break;
    }
    // Letters (A-Z) and digits (0-9): GDK reports the lowercase char.
    if ((vk >= 'A' && vk <= 'Z')) {
        if (buflen >= 2) { buf[0] = (char)(vk + 32); buf[1] = '\0'; return buf; }
    }
    if ((vk >= '0' && vk <= '9')) {
        if (buflen >= 2) { buf[0] = (char)vk; buf[1] = '\0'; return buf; }
    }
    return "";
}

static LRESULT CALLBACK canvas_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            canvas_paint(hwnd, hdc, r.right - r.left, r.bottom - r.top);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            // Forward canvas-local (x, y) to the on_move hook. WM_MOUSEMOVE
            // lParam carries client-area coords (top-left origin) — the same
            // space the canvas draws in.
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_move && cv->on_move->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_move->fn)(
                        cv->on_move->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            // Pointer press → on_click(x, y). Also grab focus so subsequent
            // keys reach the canvas (mirrors the GTK focus-on-click path).
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                SetFocus(hwnd);
                if (cv->on_click && cv->on_click->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_click->fn)(
                        cv->on_click->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_release && cv->on_release->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_release->fn)(
                        cv->on_release->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            // A double-click delivers DOWN, UP, DBLCLK, UP. Apps that treat a
            // fast second click as a "drill" (gp) rely on the second press
            // arriving — synthesize it as another on_click so the double-tap
            // path fires the handler twice, matching GTK's n_press semantics
            // where the app counts its own presses.
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_click && cv->on_click->fn) {
                    int x = (int)(short)LOWORD(lp);
                    int y = (int)(short)HIWORD(lp);
                    ((void(*)(void*, double, double))cv->on_click->fn)(
                        cv->on_click->env, (double)x, (double)y);
                }
            }
            return 0;
        }
        case WM_GETDLGCODE:
            // Claim arrow keys and character keys so they arrive as WM_KEYDOWN
            // here (canvas nav) instead of being eaten by dialog navigation.
            // NOT Tab — Tab must still move focus between the app's widgets.
            // (Return/Escape reach WM_KEYDOWN via the message loop, which only
            // special-cases Tab for IsDialogMessage.)
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            int cid = canvas_id_for_hwnd(hwnd);
            if (cid >= 1) {
                Canvas* cv = &canvases[cid - 1];
                if (cv->on_key && cv->on_key->fn) {
                    char buf[8];
                    const char* name = vk_to_gdk_name(wp, buf, sizeof(buf));
                    if (name[0]) {
                        ((void(*)(void*, const char*))cv->on_key->fn)(
                            cv->on_key->env, name);
                    }
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Animation: opacity interpolation via WM_TIMER.
// ---------------------------------------------------------------------------
typedef struct {
    int widget_handle;
    double start, target;
    int duration_ms;
    DWORD start_tick;
    int timer_id;
} Animation;
static Animation* anims = NULL;
static int anim_count = 0;
static int anim_cap = 0;

static void CALLBACK anim_tick(HWND hwnd, UINT msg, UINT_PTR id, DWORD now) {
    (void)hwnd; (void)msg; (void)now;
    for (int i = 0; i < anim_count; i++) {
        if ((UINT_PTR)anims[i].timer_id != id) continue;
        DWORD elapsed = GetTickCount() - anims[i].start_tick;
        double t = (double)elapsed / (double)anims[i].duration_ms;
        if (t >= 1.0) t = 1.0;
        double v = anims[i].start + (anims[i].target - anims[i].start) * t;
        aether_ui_set_opacity(anims[i].widget_handle, v);
        if (t >= 1.0) {
            KillTimer(NULL, anims[i].timer_id);
            // swap-delete
            anims[i] = anims[anim_count - 1];
            anim_count--;
        }
        return;
    }
}

void aether_ui_animate_opacity_impl(int handle, double target, int duration_ms) {
    if (anim_count >= anim_cap) {
        anim_cap = anim_cap == 0 ? 8 : anim_cap * 2;
        anims = (Animation*)realloc(anims, sizeof(Animation) * anim_cap);
    }
    Widget* w = widget_at(handle);
    double start = (w && w->opacity >= 0) ? w->opacity : 1.0;
    Animation* a = &anims[anim_count++];
    a->widget_handle = handle;
    a->start = start;
    a->target = target;
    a->duration_ms = duration_ms > 0 ? duration_ms : 200;
    a->start_tick = GetTickCount();
    a->timer_id = next_timer_id++;
    SetTimer(NULL, a->timer_id, 16, (TIMERPROC)anim_tick);
}

// ---------------------------------------------------------------------------
// Widget manipulation: remove/clear children.
// ---------------------------------------------------------------------------
void aether_ui_remove_child_impl(int parent_handle, int child_handle) {
    Widget* c = widget_at(child_handle);
    Widget* p = widget_at(parent_handle);
    if (c && c->hwnd) DestroyWindow(c->hwnd);
    if (p && (p->kind == WK_VSTACK || p->kind == WK_HSTACK || p->kind == WK_ZSTACK)) {
        stack_do_layout(p->hwnd);
    }
}

void aether_ui_clear_children_impl(int handle) {
    Widget* p = widget_at(handle);
    if (!p) return;
    HWND c = GetWindow(p->hwnd, GW_CHILD);
    while (c) {
        HWND next = GetWindow(c, GW_HWNDNEXT);
        DestroyWindow(c);
        c = next;
    }
    if (p->kind == WK_VSTACK || p->kind == WK_HSTACK || p->kind == WK_ZSTACK) {
        stack_do_layout(p->hwnd);
    }
}

// ---------------------------------------------------------------------------
// AetherUIDriver — Win32 adapter.
//
// The HTTP test server (socket accept, parsing, routing, JSON) lives in
// aether_ui_test_server.c and is shared with the GTK4 and AppKit
// backends. This section only provides the Win32-specific pieces:
//
//   * Backend hooks that answer widget introspection queries from the
//     server thread (widget_type, widget_text, toggle_active, etc.)
//   * UI-thread marshalling: HTTP requests fill an AetherDriverActionCtx
//     and hand it to dispatch_action(), which SendMessages it to a
//     hidden AE_WM_DRIVER window on the UI thread and blocks until the
//     action completes.
//   * Banner creation and sealing.
// ---------------------------------------------------------------------------

#include "aether_ui_test_server.h"

#define AE_WM_DRIVER (WM_USER + 0x42)

static HWND driver_host_hwnd = NULL;

// Widget-kind → short string (used as the "type" field in the driver JSON).
static const char* widget_kind_name(WidgetKind k) {
    switch (k) {
        case WK_TEXT: return "text";
        case WK_BUTTON: return "button";
        case WK_TEXTFIELD: return "textfield";
        case WK_SECUREFIELD: return "securefield";
        case WK_TEXTAREA: return "textarea";
        case WK_TOGGLE: return "toggle";
        case WK_SLIDER: return "slider";
        case WK_PICKER: return "picker";
        case WK_PROGRESSBAR: return "progressbar";
        case WK_IMAGE: return "image";
        case WK_VSTACK: return "vstack";
        case WK_HSTACK: return "hstack";
        case WK_ZSTACK: return "zstack";
        case WK_FORM: return "form";
        case WK_FORM_SECTION: return "form_section";
        case WK_NAVSTACK: return "navstack";
        case WK_SCROLLVIEW: return "scrollview";
        case WK_SPACER: return "spacer";
        case WK_DIVIDER: return "divider";
        case WK_CANVAS: return "canvas";
        case WK_WINDOW: return "window";
        case WK_SHEET: return "sheet";
        case WK_SCRIM: return "scrim";
        default: return "widget";
    }
}

// Seal APIs — delegate to the shared driver so the server sees a single
// source of truth. We also keep a per-widget `sealed` flag as a fast
// path for the WM_COMMAND handler, which fires on every button click.
void aether_ui_seal_widget_impl(int handle) {
    Widget* w = widget_at(handle);
    if (w) w->sealed = 1;
    aether_ui_test_server_seal_widget(handle);
}

void aether_ui_seal_subtree_impl(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return;
    aether_ui_seal_widget_impl(handle);
    for (HWND c = GetWindow(w->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        int ch = handle_for_hwnd(c);
        if (ch) aether_ui_seal_subtree_impl(ch);
    }
}

// ---------------------------------------------------------------------------
// Backend hooks for the shared test server.
// ---------------------------------------------------------------------------
static int hook_widget_count(void) { return widget_count; }

static const char* hook_widget_type(int handle) {
    Widget* w = widget_at(handle);
    return w ? widget_kind_name(w->kind) : "null";
}

static void hook_widget_text_into(int handle, char* buf, int bufsize) {
    Widget* w = widget_at(handle);
    if (!w || !w->hwnd) { buf[0] = '\0'; return; }
    wchar_t wbuf[1024];
    GetWindowTextW(w->hwnd, wbuf, 1024);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, bufsize, NULL, NULL);
}

static int hook_widget_visible(int handle) {
    // The widget's OWN visibility flag — parity with GTK's
    // gtk_widget_get_visible. NOT IsWindowVisible: that requires the
    // whole ancestor chain shown, and under an ssh service session the
    // top-level window never is, which zeroed "visible" app-wide.
    Widget* w = widget_at(handle);
    if (!w || !IsWindow(w->hwnd)) return 0;
    return (GetWindowLongPtrW(w->hwnd, GWL_STYLE) & WS_VISIBLE) ? 1 : 0;
}

static int hook_widget_parent(int handle) {
    Widget* w = widget_at(handle);
    if (!w) return 0;
    return handle_for_hwnd(GetParent(w->hwnd));
}

static int hook_toggle_active(int handle) {
    Widget* w = widget_at(handle);
    if (!w || w->kind != WK_TOGGLE) return 0;
    return SendMessageW(w->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
}

static double hook_slider_value(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_SLIDER) ? w->u.slider.cur_v : 0.0;
}

static double hook_progressbar_fraction(int handle) {
    Widget* w = widget_at(handle);
    return (w && w->kind == WK_PROGRESSBAR) ? w->u.progressbar.fraction : 0.0;
}

static int hook_widget_enabled(int handle) {
    Widget* w = widget_at(handle);
    return (w && IsWindowEnabled(w->hwnd)) ? 1 : 0;
}

// Window-local rect (parity with the GTK server's x/y/w/h): position is
// relative to the top-level window's CLIENT area.
static int hook_widget_rect(int handle, int* x, int* y, int* wd, int* hgt) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    RECT r;
    if (!GetWindowRect(w->hwnd, &r)) return -1;
    HWND top = GetAncestor(w->hwnd, GA_ROOT);
    POINT tl = { r.left, r.top };
    if (top) ScreenToClient(top, &tl);
    *x = tl.x;
    *y = tl.y;
    *wd = r.right - r.left;
    *hgt = r.bottom - r.top;
    return 0;
}

static void hook_widget_classes_into(int handle, char* buf, int bufsize) {
    // Registry slots outlive destroyed HWNDs (append-only): a dead row
    // must not keep reporting .aui-row-selected to specs.
    Widget* w = widget_at(handle);
    if (!w || !IsWindow(w->hwnd)) { buf[0] = '\0'; return; }
    snprintf(buf, bufsize, "%s", w->classes ? w->classes : "");
}

static int hook_focused_widget(void) {
    // GetFocus() is PER-THREAD (the HTTP thread's queue never has focus)
    // — query the UI thread's focus via GetGUIThreadInfo.
    if (app_count < 1 || !apps[0].hwnd) return 0;
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    DWORD tid = GetWindowThreadProcessId(apps[0].hwnd, NULL);
    if (!GetGUIThreadInfo(tid, &gti)) return 0;
    return handle_for_hwnd(gti.hwndFocus);
}

// dispatch_action: sends the ctx to the UI thread via AE_WM_DRIVER and
// blocks until the WndProc fills in ctx->result. Runs on the server
// thread — SendMessageW is synchronous so no explicit wait is needed.
static LRESULT CALLBACK driver_host_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    if (msg == AE_WM_DRIVER) {
        AetherDriverActionCtx* ctx = (AetherDriverActionCtx*)lp;
        if (ctx->action == AETHER_DRV_SET_STATE) {
            // Typed dispatch (sval carries the raw v=); setters walk
            // bindings, so this runs on the UI thread by construction.
            switch (aether_ui_state_type(ctx->handle)) {
                case 1: aether_ui_state_set_i(ctx->handle, atoi(ctx->sval)); break;
                case 2: aether_ui_state_set_b(ctx->handle,
                            (strcmp(ctx->sval, "true") == 0 || atoi(ctx->sval) != 0)); break;
                case 3: aether_ui_state_set_s(ctx->handle, ctx->sval); break;
                default: aether_ui_state_set(ctx->handle, ctx->dval);
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_WIN_RESIZE) {
            // Resize the app's top-level window to the given CLIENT size
            // (mirrors the GTK route's semantics).
            if (app_count > 0 && apps[0].hwnd) {
                RECT rc = { 0, 0, ctx->ival, ctx->ival2 };
                AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                         GetDpiForWindow(apps[0].hwnd));
                SetWindowPos(apps[0].hwnd, NULL, 0, 0,
                             rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_WIN_KEY) {
            // Tab / Shift+Tab move REAL focus (the same walk a dialog
            // does). Combos report fired:false — win32 has no accelerator
            // wiring yet, and a fake green here would lie about it.
            ctx->retval = 0;
            if (strcmp(ctx->sval, "Escape") == 0) {
                // Escape dismisses the topmost live overlay (the same
                // wiring GTK/macOS give it); unhandled when none is open.
                ctx->retval = w32_escape_overlays();
                ctx->result = 0;
                ctx->done = 1;
                return 0;
            }
            if (app_count > 0 && apps[0].hwnd) {
                if (strcmp(ctx->sval, "Tab") == 0 ||
                    strcmp(ctx->sval, "Shift+Tab") == 0) {
                    BOOL back = (ctx->sval[0] == 'S');
                    HWND cur = GetFocus();
                    HWND nxt = GetNextDlgTabItem(apps[0].hwnd, cur, back);
                    if (nxt) {
                        SetFocus(nxt);
                        ctx->retval = 1;
                    }
                }
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_SHUTDOWN) {
            // Same exit path a user-close takes, so the port is released and
            // the next spec in the matrix doesn't interrogate this app.
            if (app_count > 0 && apps[0].hwnd) {
                PostMessageW(apps[0].hwnd, WM_CLOSE, 0, 0);
            } else {
                PostQuitMessage(0);
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_SPLIT_POS) {
            // The Win32 splitview is still a plain stack (no divider), so
            // split_position_impl answers -1. Report that rather than a
            // fabricated 0 — a spec must be able to tell "unwired" from
            // "the divider happens to sit at zero".
            if (ctx->ival >= 0) aether_ui_split_set_position_impl(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_split_position_impl(ctx->handle);
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_TAB_SELECT) {
            // Win32 tabs is still a plain stack (no switching), so
            // tabs_selected answers -1 — the honest "unwired" signal.
            aether_ui_tabs_select(ctx->handle, ctx->ival);
            ctx->retval = aether_ui_tabs_selected(ctx->handle);
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_PICK) {
            // Real hit-test against the client area, descending to the
            // DEEPEST child at the point (ChildWindowFromPointEx only looks
            // one level down — stopping there resolved the root vstack, not
            // the button under the pointer). With a modal open, the raised
            // scrim is the topmost child at every uncovered point, so the
            // pick honestly reports the glass pane eating the click.
            POINT pt = { ctx->ival, ctx->ival2 };  // read y BEFORE ival2 becomes the out-param
            ctx->retval = 0;
            ctx->ival2 = 0;
            if (app_count > 0 && apps[0].hwnd) {
                HWND cur = apps[0].hwnd;
                for (;;) {
                    HWND hit = ChildWindowFromPointEx(cur, pt,
                        CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
                    if (!hit || hit == cur) break;
                    MapWindowPoints(cur, hit, &pt, 1);
                    cur = hit;
                }
                if (cur != apps[0].hwnd) {
                    int hh = handle_for_hwnd(cur);
                    ctx->retval = hh;
                    Widget* hw = hh ? widget_at(hh) : NULL;
                    if (hw && hw->kind == WK_SCRIM) ctx->ival2 = 1;
                }
            }
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->action == AETHER_DRV_CANVAS_CLICK
            || ctx->action == AETHER_DRV_CANVAS_MOVE
            || ctx->action == AETHER_DRV_CANVAS_RELEASE
            || ctx->action == AETHER_DRV_CANVAS_KEY) {
            // Drive the canvas hit-test hooks directly, exactly as a real
            // WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP / WM_KEYDOWN would
            // (result 3 = "no such handler wired", so a spec can tell a missed
            // click from an unwired canvas).
            ctx->result = 3;
            if (ctx->handle >= 1 && ctx->handle <= canvas_count) {
                Canvas* cv = &canvases[ctx->handle - 1];
                if (ctx->action == AETHER_DRV_CANVAS_MOVE) {
                    if (cv->on_move && cv->on_move->fn) {
                        ((void(*)(void*, double, double))cv->on_move->fn)(
                            cv->on_move->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else if (ctx->action == AETHER_DRV_CANVAS_CLICK) {
                    if (cv->on_click && cv->on_click->fn) {
                        ((void(*)(void*, double, double))cv->on_click->fn)(
                            cv->on_click->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else if (ctx->action == AETHER_DRV_CANVAS_RELEASE) {
                    if (cv->on_release && cv->on_release->fn) {
                        ((void(*)(void*, double, double))cv->on_release->fn)(
                            cv->on_release->env, ctx->dval, ctx->dval2);
                        ctx->result = 0;
                    }
                } else { // AETHER_DRV_CANVAS_KEY
                    if (cv->on_key && cv->on_key->fn) {
                        ((void(*)(void*, const char*))cv->on_key->fn)(
                            cv->on_key->env, ctx->sval);
                        ctx->result = 0;
                    }
                }
            }
            ctx->done = 1;
            return 0;
        }
        Widget* w = widget_at(ctx->handle);
        if (!w) { ctx->result = 3; ctx->done = 1; return 0; }
        if (ctx->action == AETHER_DRV_FOCUS) {
            SetFocus(w->hwnd);
            ctx->result = 0;
            ctx->done = 1;
            return 0;
        }
        if (ctx->handle == aether_ui_test_server_banner_handle()) {
            ctx->result = 2; ctx->done = 1; return 0;
        }
        if (aether_ui_test_server_is_sealed(ctx->handle)) {
            ctx->result = 1; ctx->done = 1; return 0;
        }
        switch (ctx->action) {
            case AETHER_DRV_CLICK:
                // Buttons and ANY widget with an on_click handler (listbox
                // rows are plain containers) — mirrors the GTK4 server's
                // gesture-closure fallback: invoke the handler a real click
                // would run.
                if (w->kind == WK_BUTTON || w->on_click) invoke_closure(w->on_click);
                break;
            case AETHER_DRV_SET_TEXT:
                if (w->kind == WK_TEXT || w->kind == WK_TEXTFIELD
                    || w->kind == WK_SECUREFIELD || w->kind == WK_TEXTAREA) {
                    SetWindowTextW(w->hwnd, utf8_to_wide(ctx->sval));
                }
                break;
            case AETHER_DRV_TOGGLE:
                if (w->kind == WK_TOGGLE) {
                    int cur = aether_ui_toggle_get_active(ctx->handle);
                    aether_ui_toggle_set_active(ctx->handle, !cur);
                    invoke_closure(w->on_change);
                }
                break;
            case AETHER_DRV_SET_VALUE:
                if (w->kind == WK_SLIDER)
                    aether_ui_slider_set_value(ctx->handle, ctx->dval);
                else if (w->kind == WK_PROGRESSBAR)
                    aether_ui_progressbar_set_fraction(ctx->handle, ctx->dval);
                else if (w->kind == WK_PICKER) {
                    // Driver selects a picker index via set_value (the
                    // surface-agnostic path the drawn/native picker share);
                    // route it and fire on_change so the round-trip matches
                    // GtkDropDown's notify::selected.
                    aether_ui_picker_set_selected(ctx->handle, (int)ctx->dval);
                    if (w->on_change && w->on_change->fn) {
                        ((void(*)(void*, intptr_t))w->on_change->fn)(
                            w->on_change->env, (intptr_t)(int)ctx->dval);
                    }
                }
                break;
            default: break;
        }
        ctx->result = 0;
        ctx->done = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void hook_dispatch_action(AetherDriverActionCtx* ctx) {
    SendMessageW(driver_host_hwnd, AE_WM_DRIVER, 0, (LPARAM)ctx);
}

// List direct children of a widget. Returns the number written; -1 if
// the widget itself wasn't found.
static int hook_widget_children(int handle, int* out, int max) {
    Widget* w = widget_at(handle);
    if (!w) return -1;
    int n = 0;
    for (HWND c = GetWindow(w->hwnd, GW_CHILD); c && n < max;
         c = GetWindow(c, GW_HWNDNEXT)) {
        int ch = handle_for_hwnd(c);
        if (ch > 0) {
            if (out) out[n] = ch;
            n++;
        }
    }
    return n;
}

// Screenshot the app's first window to a PNG in memory.
// Uses BitBlt + GDI+ (via the same flat-API binding we use for images).
__declspec(dllimport) int __stdcall GdipCreateBitmapFromHBITMAP(
    HBITMAP hbm, HPALETTE pal, void** bitmap);
__declspec(dllimport) int __stdcall GdipSaveImageToStream(
    void* image, void* stream, const GUID* clsid, const void* params);
__declspec(dllimport) int __stdcall GdipGetImageEncodersSize(
    unsigned int* num_encoders, unsigned int* size);
__declspec(dllimport) int __stdcall GdipGetImageEncoders(
    unsigned int num_encoders, unsigned int size, void* encoders);

static int hook_screenshot_png(unsigned char** out_data, size_t* out_len) {
    if (app_count == 0) return -1;
    HWND hwnd = apps[0].hwnd;
    if (!hwnd) return -1;
    RECT r;
    if (!GetClientRect(hwnd, &r)) return -1;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return -1;

    HDC src = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(src);
    HBITMAP bmp = CreateCompatibleBitmap(src, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    SelectObject(mem, old);

    ensure_gdiplus();
    void* gdi_bitmap = NULL;
    int rc = -1;
    if (gdiplus_started
        && GdipCreateBitmapFromHBITMAP(bmp, NULL, &gdi_bitmap) == 0
        && gdi_bitmap) {
        // Serialize to an IStream, then copy its bytes out.
        IStream* stream = NULL;
        if (CreateStreamOnHGlobal(NULL, TRUE, &stream) == S_OK && stream) {
            // PNG codec CLSID {557CF406-1A04-11D3-9A73-0000F81EF32E}
            GUID png_clsid = {0x557cf406, 0x1a04, 0x11d3,
                              {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            if (GdipSaveImageToStream(gdi_bitmap, stream,
                                       &png_clsid, NULL) == 0) {
                HGLOBAL hg;
                if (GetHGlobalFromStream(stream, &hg) == S_OK) {
                    size_t sz = GlobalSize(hg);
                    void* src_ptr = GlobalLock(hg);
                    if (src_ptr && sz > 0) {
                        *out_data = (unsigned char*)malloc(sz);
                        memcpy(*out_data, src_ptr, sz);
                        *out_len = sz;
                        rc = 0;
                    }
                    GlobalUnlock(hg);
                }
            }
            stream->lpVtbl->Release(stream);
        }
        GdipDisposeImage(gdi_bitmap);
    }

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, src);
    return rc;
}

static const AetherDriverHooks win32_driver_hooks = {
    .widget_count         = hook_widget_count,
    .widget_type          = hook_widget_type,
    .widget_text_into     = hook_widget_text_into,
    .widget_visible       = hook_widget_visible,
    .widget_parent        = hook_widget_parent,
    .toggle_active        = hook_toggle_active,
    .slider_value         = hook_slider_value,
    .progressbar_fraction = hook_progressbar_fraction,
    .dispatch_action      = hook_dispatch_action,
    .widget_children      = hook_widget_children,
    .widget_enabled       = hook_widget_enabled,
    .widget_rect          = hook_widget_rect,
    .widget_classes_into  = hook_widget_classes_into,
    .focused_widget       = hook_focused_widget,
    .screenshot_png       = hook_screenshot_png,
};

static int test_server_started = 0;
void aether_ui_enable_test_server_impl(int port, int root_handle) {
    // Idempotent — the env auto-start and an explicit app call must not
    // stack a second banner (seen as duplicate "Under Remote Control").
    if (test_server_started) return;
    test_server_started = 1;
    // Create a "Under Remote Control" banner, style it red+bold, seal it,
    // and hoist it to the top of the root stack.
    int banner = aether_ui_text_create("Under Remote Control");
    Widget* bw = widget_at(banner);
    if (bw) {
        HFONT base = (HFONT)SendMessageW(bw->hwnd, WM_GETFONT, 0, 0);
        LOGFONTW lf;
        GetObjectW(base, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        HFONT bold = CreateFontIndirectW(&lf);
        SendMessageW(bw->hwnd, WM_SETFONT, (WPARAM)bold, TRUE);
        bw->custom_font = bold;
        bw->fg.has_value = 1;
        bw->fg.color = RGB(200, 0, 0);
    }
    Widget* root = widget_at(root_handle);
    if (root && bw && root->kind == WK_VSTACK) {
        SetParent(bw->hwnd, root->hwnd);
        LONG_PTR st = GetWindowLongPtrW(bw->hwnd, GWL_STYLE);
        SetWindowLongPtrW(bw->hwnd, GWL_STYLE, st | WS_CHILD | WS_VISIBLE);
        SetWindowPos(bw->hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        stack_do_layout(root->hwnd);
    }
    aether_ui_test_server_set_banner(banner);
    aether_ui_seal_widget_impl(banner); // banner is not automatable

    // Hidden window that receives AE_WM_DRIVER on the UI thread.
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = driver_host_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AetherUIDriverHost";
    RegisterClassExW(&wc);
    driver_host_hwnd = CreateWindowExW(0, L"AetherUIDriverHost", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);

    aether_ui_test_server_start(port, &win32_driver_hooks);
}

// ---------------------------------------------------------------------------
// System tray (Group 7) — registry-only stub.
//
// Real implementation should use Shell_NotifyIcon(NIM_ADD,
// NIF_ICON | NIF_TIP | NIF_MESSAGE) with a hidden message-only window
// that handles WM_LBUTTONUP / WM_RBUTTONUP via the callback message
// id. Right-click should TrackPopupMenu against the menu_handle's
// HMENU; left-click should dispatch through
// aether_ui_tray_emit_click(id). Icon-template flag is a no-op on
// Win32 (icons are full-colour .ico resources).
//
// Cannot be authored here without a Windows host to test against;
// the registry + driver routes still validate the callback wiring.
// ---------------------------------------------------------------------------
int aether_ui_tray_create_impl(const char* name, void* boxed_left_click) {
    return aether_ui_tray_register(name, boxed_left_click);
}
void aether_ui_tray_set_tooltip_impl(int tray_id, const char* text) {
    aether_ui_tray_set_tooltip_reg(tray_id, text);
}
void aether_ui_tray_set_menu_impl(int tray_id, int menu_handle) {
    aether_ui_tray_set_menu_reg(tray_id, menu_handle);
}
void aether_ui_tray_set_icon_for_state_impl(int tray_id, int state_handle,
                                             const char* icon_clean,
                                             const char* icon_busy,
                                             const char* icon_alert) {
    aether_ui_tray_set_icon_for_state_reg(tray_id, state_handle,
                                          icon_clean, icon_busy, icon_alert);
}
void aether_ui_tray_set_icon_template_impl(int tray_id, int is_template) {
    aether_ui_tray_set_icon_template_reg(tray_id, is_template);
}
void aether_ui_tray_seal_impl(int tray_id) {
    aether_ui_tray_seal_reg(tray_id);
}

// ---------------------------------------------------------------------------
// Desktop notifications (Group 7b) — registry-only stub.
//
// Real implementation should use Windows.UI.Notifications.
// ToastNotificationManager via the WinRT C ABI: build Toast XML, set
// an AUMID via SetCurrentProcessExplicitAppUserModelID, then
// CreateToastNotificationManagerForApplication(aumid).ShowToast(xml).
// Activation handler routes back through
// aether_ui_notif_emit_click(id) via the IToastActivatedEventArgs.
// ---------------------------------------------------------------------------
int aether_ui_notify_impl(const char* title, const char* body) {
    return aether_ui_notify_register(title, body);
}
int aether_ui_notify_full_impl(const char* title, const char* body,
                                const char* icon_path, const char* tag,
                                void* boxed_click) {
    return aether_ui_notify_register_full(title, body, icon_path, tag, boxed_click);
}
int aether_ui_notify_request_permission_impl(void) {
    return aether_ui_notify_request_permission();
}

// app_run_headless on Win32: park until killed. A future native
// Shell_NotifyIcon implementation should run a GetMessage loop
// here so the message-only window receives the tray callback
// messages from the OS.
void aether_ui_app_run_headless_impl(void) {
    aether_ui_park_until_killed();
}
