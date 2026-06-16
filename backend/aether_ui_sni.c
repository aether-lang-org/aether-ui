// Aether UI — Linux StatusNotifierItem + DBusMenu via GDBus.
// See aether_ui_sni.h for the contract and rationale.

#include "aether_ui_sni.h"
#include "aether_ui_system_extras.h"

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Per-tray SNI state. Capped at the same TRAY_MAX as the registry;
// indexed by tray_id - 1.
// ---------------------------------------------------------------------------
#define SNI_MAX 64

typedef struct {
    int   in_use;
    int   tray_id;
    char  bus_name[128];        // org.kde.StatusNotifierItem-<pid>-<n>
    guint owner_id;             // g_bus_own_name handle
    guint sni_reg_id;           // object reg id for /StatusNotifierItem
    guint menu_reg_id;          // object reg id for /MenuBar
    GDBusConnection* conn;      // session bus conn (borrowed)
    int   registered_with_watcher;  // RegisterStatusNotifierItem succeeded
    int   menu_revision;        // bumped on every LayoutUpdated emit
} SniState;

static SniState g_sni[SNI_MAX];

static SniState* sni_for(int tray_id) {
    if (tray_id < 1 || tray_id > SNI_MAX) return NULL;
    SniState* s = &g_sni[tray_id - 1];
    return s->in_use ? s : NULL;
}

// ---------------------------------------------------------------------------
// Interface XML — kept inline so this file is self-contained. The
// canonical specs:
//   https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/
//   https://github.com/AyatanaIndicators/libdbusmenu/blob/master/libdbusmenu-glib/dbus-menu.xml
//
// We declare only the methods/properties we implement. Hosts tolerate
// missing members on either interface.
// ---------------------------------------------------------------------------

static const char* SNI_XML =
"<node>"
"  <interface name='org.kde.StatusNotifierItem'>"
"    <property name='Category' type='s' access='read'/>"
"    <property name='Id' type='s' access='read'/>"
"    <property name='Title' type='s' access='read'/>"
"    <property name='Status' type='s' access='read'/>"
"    <property name='WindowId' type='i' access='read'/>"
"    <property name='IconName' type='s' access='read'/>"
"    <property name='IconThemePath' type='s' access='read'/>"
"    <property name='ItemIsMenu' type='b' access='read'/>"
"    <property name='Menu' type='o' access='read'/>"
"    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
"    <method name='Activate'>"
"      <arg name='x' type='i' direction='in'/>"
"      <arg name='y' type='i' direction='in'/>"
"    </method>"
"    <method name='SecondaryActivate'>"
"      <arg name='x' type='i' direction='in'/>"
"      <arg name='y' type='i' direction='in'/>"
"    </method>"
"    <method name='ContextMenu'>"
"      <arg name='x' type='i' direction='in'/>"
"      <arg name='y' type='i' direction='in'/>"
"    </method>"
"    <method name='Scroll'>"
"      <arg name='delta' type='i' direction='in'/>"
"      <arg name='orientation' type='s' direction='in'/>"
"    </method>"
"    <signal name='NewIcon'/>"
"    <signal name='NewTitle'/>"
"    <signal name='NewToolTip'/>"
"    <signal name='NewStatus'>"
"      <arg name='status' type='s'/>"
"    </signal>"
"  </interface>"
"</node>";

static const char* MENU_XML =
"<node>"
"  <interface name='com.canonical.dbusmenu'>"
"    <property name='Version' type='u' access='read'/>"
"    <property name='TextDirection' type='s' access='read'/>"
"    <property name='Status' type='s' access='read'/>"
"    <property name='IconThemePath' type='as' access='read'/>"
"    <method name='GetLayout'>"
"      <arg name='parentId' type='i' direction='in'/>"
"      <arg name='recursionDepth' type='i' direction='in'/>"
"      <arg name='propertyNames' type='as' direction='in'/>"
"      <arg name='revision' type='u' direction='out'/>"
"      <arg name='layout' type='(ia{sv}av)' direction='out'/>"
"    </method>"
"    <method name='GetGroupProperties'>"
"      <arg name='ids' type='ai' direction='in'/>"
"      <arg name='propertyNames' type='as' direction='in'/>"
"      <arg name='properties' type='a(ia{sv})' direction='out'/>"
"    </method>"
"    <method name='GetProperty'>"
"      <arg name='id' type='i' direction='in'/>"
"      <arg name='name' type='s' direction='in'/>"
"      <arg name='value' type='v' direction='out'/>"
"    </method>"
"    <method name='Event'>"
"      <arg name='id' type='i' direction='in'/>"
"      <arg name='eventId' type='s' direction='in'/>"
"      <arg name='data' type='v' direction='in'/>"
"      <arg name='timestamp' type='u' direction='in'/>"
"    </method>"
"    <method name='EventGroup'>"
"      <arg name='events' type='a(isvu)' direction='in'/>"
"      <arg name='idErrors' type='ai' direction='out'/>"
"    </method>"
"    <method name='AboutToShow'>"
"      <arg name='id' type='i' direction='in'/>"
"      <arg name='needUpdate' type='b' direction='out'/>"
"    </method>"
"    <method name='AboutToShowGroup'>"
"      <arg name='ids' type='ai' direction='in'/>"
"      <arg name='updatesNeeded' type='ai' direction='out'/>"
"      <arg name='idErrors' type='ai' direction='out'/>"
"    </method>"
"    <signal name='ItemsPropertiesUpdated'>"
"      <arg type='a(ia{sv})'/>"
"      <arg type='a(ias)'/>"
"    </signal>"
"    <signal name='LayoutUpdated'>"
"      <arg type='u'/>"
"      <arg type='i'/>"
"    </signal>"
"    <signal name='ItemActivationRequested'>"
"      <arg type='i'/>"
"      <arg type='u'/>"
"    </signal>"
"  </interface>"
"</node>";

static GDBusNodeInfo* g_sni_node_info  = NULL;
static GDBusNodeInfo* g_menu_node_info = NULL;

// ---------------------------------------------------------------------------
// Menu-item id mapping. SNI/DBusMenu addresses items by integer id;
// our registry addresses them by (menu_handle, label). We synthesize
// 1-based ids per-tray by listing the items at registration / refresh
// time. The mapping lives on the tray's SniState via a side-array.
//
// Item id 0 is the menu root by DBusMenu convention. Real menu items
// start at id 1.
// ---------------------------------------------------------------------------

#define MENU_ITEMS_MAX 64

typedef struct {
    int  id;       // 1-based DBusMenu id
    char label[128];
    int  is_sep;
} MenuItemRef;

typedef struct {
    int          count;
    MenuItemRef  items[MENU_ITEMS_MAX];
} SniMenu;

static SniMenu g_sni_menus[SNI_MAX];

// Re-snapshot the menu items associated with this tray's menu_handle.
// Called on initial register + every aether_ui_sni_invalidate_menu().
// Pulls from the menu_item registry via aether_ui_menu_item_*; that
// registry only exposes invoke-by-label, so we mirror the records
// here. To keep this small we just walk the registry's count and
// shadow each item.
static void sni_refresh_menu_snapshot(int tray_id);

// Extern: the menu_item registry doesn't expose a list-by-handle
// accessor; it only exposes invoke-by-label. To enumerate items for
// DBusMenu's GetLayout we add a tiny enumerator here. We can't reach
// in to system_extras.c's static array, so we use a forwarder
// helper. Add the helper to system_extras and declare here:
extern int aether_ui_menu_item_count_for(int menu_handle);
extern const char* aether_ui_menu_item_label_at(int menu_handle, int index);

// ---------------------------------------------------------------------------
// SNI method + property handlers
// ---------------------------------------------------------------------------

static void sni_method_call(GDBusConnection* conn, const char* sender,
                             const char* object_path, const char* iface_name,
                             const char* method_name, GVariant* params,
                             GDBusMethodInvocation* invocation,
                             gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)iface_name; (void)params;
    int tray_id = GPOINTER_TO_INT(user_data);

    if (strcmp(method_name, "Activate") == 0
        || strcmp(method_name, "SecondaryActivate") == 0) {
        // Primary or secondary click on the icon itself. Map both to
        // the left-click closure — the existing widget surface has no
        // separate "secondary activate" hook today.
        aether_ui_tray_emit_click(tray_id);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (strcmp(method_name, "ContextMenu") == 0) {
        // The host normally renders the menu itself via the Menu
        // property — this is the fallback for hosts that explicitly
        // ask the SNI to handle context-menu rendering. We don't, so
        // just return; the host will fall back to the Menu property.
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (strcmp(method_name, "Scroll") == 0) {
        // No scroll handler today; acknowledge to avoid the host
        // marking us as broken.
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation,
            G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown method %s", method_name);
    }
}

static GVariant* sni_get_property(GDBusConnection* conn, const char* sender,
                                   const char* object_path,
                                   const char* iface_name,
                                   const char* property_name,
                                   GError** error, gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)iface_name; (void)error;
    int tray_id = GPOINTER_TO_INT(user_data);

    if (strcmp(property_name, "Category") == 0)
        return g_variant_new_string("ApplicationStatus");
    if (strcmp(property_name, "Id") == 0)
        return g_variant_new_string(aether_ui_tray_name(tray_id));
    if (strcmp(property_name, "Title") == 0)
        return g_variant_new_string(aether_ui_tray_name(tray_id));
    if (strcmp(property_name, "Status") == 0)
        return g_variant_new_string("Active");
    if (strcmp(property_name, "WindowId") == 0)
        return g_variant_new_int32(0);
    if (strcmp(property_name, "IconName") == 0) {
        const char* icon = aether_ui_tray_current_icon(tray_id);
        // SNI's IconName is conventionally a freedesktop theme name,
        // but every host we target (KDE, GNOME-shell-appindicator,
        // XFCE-sntray, Cinnamon, Budgie) also accepts an absolute
        // path here in practice. Path → file load; theme name → icon
        // theme resolution. Fall back to "application-x-executable"
        // so the icon never silently disappears mid-update.
        return g_variant_new_string((icon && *icon) ? icon
                                    : "application-x-executable");
    }
    if (strcmp(property_name, "IconThemePath") == 0)
        return g_variant_new_string("");
    if (strcmp(property_name, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (strcmp(property_name, "Menu") == 0) {
        SniState* s = sni_for(tray_id);
        if (s && s->menu_reg_id) return g_variant_new_object_path("/MenuBar");
        // No menu wired — return an unused path; some hosts insist on
        // a non-empty object path here even when no menu is set.
        return g_variant_new_object_path("/NO_DBUSMENU");
    }
    if (strcmp(property_name, "ToolTip") == 0) {
        // ToolTip is (icon_name: s, icon_pixmap: a(iiay), title: s, body: s).
        // Aether-ui currently only ships a flat tooltip string; map
        // it to the title slot, leave icon + body empty.
        const char* tt = aether_ui_tray_tooltip(tray_id);
        GVariantBuilder pixmap_b;
        g_variant_builder_init(&pixmap_b, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(sa(iiay)ss)", "", &pixmap_b, tt ? tt : "", "");
    }
    return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
    sni_method_call,
    sni_get_property,
    NULL,    // no writable props
    { NULL }
};

// ---------------------------------------------------------------------------
// DBusMenu method + property handlers
// ---------------------------------------------------------------------------

static GVariant* menu_build_layout_for(int tray_id) {
    // (parentId, properties, children-as-variants)
    SniMenu* sm = &g_sni_menus[tray_id - 1];

    GVariantBuilder children_b;
    g_variant_builder_init(&children_b, G_VARIANT_TYPE("av"));

    for (int i = 0; i < sm->count; i++) {
        MenuItemRef* it = &sm->items[i];

        GVariantBuilder props_b;
        g_variant_builder_init(&props_b, G_VARIANT_TYPE("a{sv}"));
        if (it->is_sep) {
            g_variant_builder_add(&props_b, "{sv}",
                "type", g_variant_new_string("separator"));
        } else {
            g_variant_builder_add(&props_b, "{sv}",
                "label", g_variant_new_string(it->label));
            g_variant_builder_add(&props_b, "{sv}",
                "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(&props_b, "{sv}",
                "visible", g_variant_new_boolean(TRUE));
        }

        GVariantBuilder kids_b;
        g_variant_builder_init(&kids_b, G_VARIANT_TYPE("av"));

        GVariant* item = g_variant_new("(ia{sv}av)",
            it->id, &props_b, &kids_b);
        g_variant_builder_add(&children_b, "v", item);
    }

    GVariantBuilder root_props_b;
    g_variant_builder_init(&root_props_b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&root_props_b, "{sv}",
        "children-display", g_variant_new_string("submenu"));

    return g_variant_new("(ia{sv}av)", 0, &root_props_b, &children_b);
}

static void menu_method_call(GDBusConnection* conn, const char* sender,
                              const char* object_path, const char* iface_name,
                              const char* method_name, GVariant* params,
                              GDBusMethodInvocation* invocation,
                              gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)iface_name;
    int tray_id = GPOINTER_TO_INT(user_data);
    SniState* s = sni_for(tray_id);
    if (!s) {
        g_dbus_method_invocation_return_error(invocation,
            G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "tray gone");
        return;
    }

    if (strcmp(method_name, "GetLayout") == 0) {
        GVariant* layout = menu_build_layout_for(tray_id);
        // `@(ia{sv}av)` consumes the pre-built layout variant.
        // Plain `(ia{sv}av)` would expect the three constituent args
        // here, not a pre-built variant — that mismatch was the
        // segfault in g_variant_builder_end.
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(u@(ia{sv}av))", s->menu_revision, layout));
        return;
    }

    if (strcmp(method_name, "GetGroupProperties") == 0) {
        // params: (ai ids, as propertyNames). We return the property
        // blob for every requested id; ids we don't know about are
        // omitted (the host falls back to GetLayout).
        GVariant* ids_v = NULL;
        GVariant* names_v = NULL;
        g_variant_get(params, "(@ai@as)", &ids_v, &names_v);

        GVariantBuilder out_b;
        g_variant_builder_init(&out_b, G_VARIANT_TYPE("a(ia{sv})"));

        gsize n_ids = g_variant_n_children(ids_v);
        SniMenu* sm = &g_sni_menus[tray_id - 1];
        for (gsize k = 0; k < n_ids; k++) {
            gint32 id;
            g_variant_get_child(ids_v, k, "i", &id);
            GVariantBuilder props_b;
            g_variant_builder_init(&props_b, G_VARIANT_TYPE("a{sv}"));
            if (id == 0) {
                g_variant_builder_add(&props_b, "{sv}",
                    "children-display", g_variant_new_string("submenu"));
            } else {
                MenuItemRef* hit = NULL;
                for (int i = 0; i < sm->count; i++) {
                    if (sm->items[i].id == id) { hit = &sm->items[i]; break; }
                }
                if (hit) {
                    if (hit->is_sep) {
                        g_variant_builder_add(&props_b, "{sv}",
                            "type", g_variant_new_string("separator"));
                    } else {
                        g_variant_builder_add(&props_b, "{sv}",
                            "label", g_variant_new_string(hit->label));
                        g_variant_builder_add(&props_b, "{sv}",
                            "enabled", g_variant_new_boolean(TRUE));
                        g_variant_builder_add(&props_b, "{sv}",
                            "visible", g_variant_new_boolean(TRUE));
                    }
                }
            }
            g_variant_builder_add(&out_b, "(ia{sv})", id, &props_b);
        }
        g_variant_unref(ids_v);
        g_variant_unref(names_v);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(a(ia{sv}))", &out_b));
        return;
    }

    if (strcmp(method_name, "GetProperty") == 0) {
        gint32 id;
        const gchar* name;
        g_variant_get(params, "(i&s)", &id, &name);
        SniMenu* sm = &g_sni_menus[tray_id - 1];
        for (int i = 0; i < sm->count; i++) {
            if (sm->items[i].id != id) continue;
            if (strcmp(name, "label") == 0) {
                g_dbus_method_invocation_return_value(invocation,
                    g_variant_new("(v)", g_variant_new_string(sm->items[i].label)));
                return;
            }
            if (strcmp(name, "enabled") == 0 || strcmp(name, "visible") == 0) {
                g_dbus_method_invocation_return_value(invocation,
                    g_variant_new("(v)", g_variant_new_boolean(TRUE)));
                return;
            }
        }
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(v)", g_variant_new_string("")));
        return;
    }

    if (strcmp(method_name, "Event") == 0) {
        // (i id, s eventId, v data, u timestamp). We care about the
        // "clicked" event. The host fires it when the user activates
        // an item.
        gint32 id;
        const gchar* eventId = NULL;
        GVariant* data_v = NULL;
        guint32 ts;
        g_variant_get(params, "(i&svu)", &id, &eventId, &data_v, &ts);
        if (eventId && strcmp(eventId, "clicked") == 0) {
            SniMenu* sm = &g_sni_menus[tray_id - 1];
            for (int i = 0; i < sm->count; i++) {
                if (sm->items[i].id == id && !sm->items[i].is_sep) {
                    aether_ui_tray_menu_activate(tray_id, sm->items[i].label);
                    break;
                }
            }
        }
        if (data_v) g_variant_unref(data_v);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (strcmp(method_name, "EventGroup") == 0) {
        // a(isvu) batched Event calls. Same dispatch as Event.
        GVariantIter iter;
        g_variant_iter_init(&iter, g_variant_get_child_value(params, 0));
        gint32 id;
        const gchar* eventId;
        GVariant* data_v;
        guint32 ts;
        SniMenu* sm = &g_sni_menus[tray_id - 1];
        GVariantBuilder err_b;
        g_variant_builder_init(&err_b, G_VARIANT_TYPE("ai"));
        while (g_variant_iter_next(&iter, "(i&svu)",
                                   &id, &eventId, &data_v, &ts)) {
            if (eventId && strcmp(eventId, "clicked") == 0) {
                for (int i = 0; i < sm->count; i++) {
                    if (sm->items[i].id == id && !sm->items[i].is_sep) {
                        aether_ui_tray_menu_activate(tray_id, sm->items[i].label);
                        break;
                    }
                }
            }
            if (data_v) g_variant_unref(data_v);
        }
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(ai)", &err_b));
        return;
    }

    if (strcmp(method_name, "AboutToShow") == 0) {
        // Returns true when the host should re-fetch the menu before
        // displaying. We always re-snapshot defensively (menu items
        // can be added dynamically post-registration) and return
        // FALSE to indicate the layout the host already has is still
        // valid (LayoutUpdated would have been fired on change).
        sni_refresh_menu_snapshot(tray_id);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", FALSE));
        return;
    }

    if (strcmp(method_name, "AboutToShowGroup") == 0) {
        GVariantBuilder upd_b, err_b;
        g_variant_builder_init(&upd_b, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&err_b, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(aiai)", &upd_b, &err_b));
        return;
    }

    g_dbus_method_invocation_return_error(invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
        "Unknown method %s", method_name);
}

static GVariant* menu_get_property(GDBusConnection* conn, const char* sender,
                                    const char* object_path,
                                    const char* iface_name,
                                    const char* property_name,
                                    GError** error, gpointer user_data) {
    (void)conn; (void)sender; (void)object_path; (void)iface_name; (void)error;
    (void)user_data;
    if (strcmp(property_name, "Version") == 0)        return g_variant_new_uint32(3);
    if (strcmp(property_name, "TextDirection") == 0)  return g_variant_new_string("ltr");
    if (strcmp(property_name, "Status") == 0)         return g_variant_new_string("normal");
    if (strcmp(property_name, "IconThemePath") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        return g_variant_new("as", &b);
    }
    return NULL;
}

static const GDBusInterfaceVTable menu_vtable = {
    menu_method_call,
    menu_get_property,
    NULL,
    { NULL }
};

// ---------------------------------------------------------------------------
// Menu snapshot from the menu_item side-store
// ---------------------------------------------------------------------------
static void sni_refresh_menu_snapshot(int tray_id) {
    SniState* s = sni_for(tray_id);
    if (!s) return;
    int menu_handle = aether_ui_tray_menu_handle(tray_id);
    SniMenu* sm = &g_sni_menus[tray_id - 1];
    sm->count = 0;
    if (menu_handle <= 0) return;
    int n = aether_ui_menu_item_count_for(menu_handle);
    for (int i = 0; i < n && sm->count < MENU_ITEMS_MAX; i++) {
        const char* lbl = aether_ui_menu_item_label_at(menu_handle, i);
        MenuItemRef* r = &sm->items[sm->count];
        r->id = sm->count + 1; // 1-based, distinct from root id 0
        r->is_sep = 0;
        if (lbl) {
            strncpy(r->label, lbl, sizeof(r->label) - 1);
            r->label[sizeof(r->label) - 1] = '\0';
        } else {
            r->label[0] = '\0';
        }
        sm->count++;
    }
}

// ---------------------------------------------------------------------------
// Bus-name acquisition + RegisterStatusNotifierItem call
// ---------------------------------------------------------------------------

static void on_register_status_notifier_item_done(GObject* source,
                                                    GAsyncResult* res,
                                                    gpointer user_data) {
    int tray_id = GPOINTER_TO_INT(user_data);
    GError* err = NULL;
    GVariant* v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                  res, &err);
    SniState* s = sni_for(tray_id);
    if (err) {
        fprintf(stderr,
            "aether-ui SNI: RegisterStatusNotifierItem failed for tray %d: %s\n",
            tray_id, err->message);
        g_error_free(err);
        if (s) s->registered_with_watcher = 0;
        return;
    }
    if (v) g_variant_unref(v);
    if (s) s->registered_with_watcher = 1;
}

static void on_bus_acquired(GDBusConnection* conn, const gchar* name,
                              gpointer user_data) {
    int tray_id = GPOINTER_TO_INT(user_data);
    SniState* s = sni_for(tray_id);
    if (!s) return;
    s->conn = conn;

    GError* err = NULL;
    GDBusInterfaceInfo* sni_iface =
        g_dbus_node_info_lookup_interface(g_sni_node_info,
                                           "org.kde.StatusNotifierItem");
    s->sni_reg_id = g_dbus_connection_register_object(conn,
        "/StatusNotifierItem", sni_iface, &sni_vtable,
        GINT_TO_POINTER(tray_id), NULL, &err);
    if (err) {
        fprintf(stderr, "aether-ui SNI: register /StatusNotifierItem failed: %s\n",
                err->message);
        g_clear_error(&err);
        return;
    }

    GDBusInterfaceInfo* menu_iface =
        g_dbus_node_info_lookup_interface(g_menu_node_info,
                                           "com.canonical.dbusmenu");
    s->menu_reg_id = g_dbus_connection_register_object(conn,
        "/MenuBar", menu_iface, &menu_vtable,
        GINT_TO_POINTER(tray_id), NULL, &err);
    if (err) {
        fprintf(stderr, "aether-ui SNI: register /MenuBar failed: %s\n",
                err->message);
        g_clear_error(&err);
    }

    sni_refresh_menu_snapshot(tray_id);
}

static void on_name_acquired(GDBusConnection* conn, const gchar* name,
                              gpointer user_data) {
    int tray_id = GPOINTER_TO_INT(user_data);
    SniState* s = sni_for(tray_id);
    if (!s) return;

    g_dbus_connection_call(conn,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher",
        "RegisterStatusNotifierItem",
        g_variant_new("(s)", name),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_register_status_notifier_item_done, GINT_TO_POINTER(tray_id));
}

static void on_name_lost(GDBusConnection* conn, const gchar* name,
                          gpointer user_data) {
    (void)conn; (void)user_data;
    fprintf(stderr, "aether-ui SNI: lost bus name %s\n", name);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void ensure_node_info(void) {
    if (!g_sni_node_info) {
        GError* err = NULL;
        g_sni_node_info = g_dbus_node_info_new_for_xml(SNI_XML, &err);
        if (err) {
            fprintf(stderr, "aether-ui SNI: SNI XML parse failed: %s\n",
                    err->message);
            g_clear_error(&err);
        }
    }
    if (!g_menu_node_info) {
        GError* err = NULL;
        g_menu_node_info = g_dbus_node_info_new_for_xml(MENU_XML, &err);
        if (err) {
            fprintf(stderr, "aether-ui SNI: Menu XML parse failed: %s\n",
                    err->message);
            g_clear_error(&err);
        }
    }
}

int aether_ui_sni_register(int tray_id) {
    if (tray_id < 1 || tray_id > SNI_MAX) return 0;
    ensure_node_info();
    if (!g_sni_node_info || !g_menu_node_info) return 0;

    SniState* s = &g_sni[tray_id - 1];
    if (s->in_use) return s->registered_with_watcher;
    s->in_use = 1;
    s->tray_id = tray_id;
    s->menu_revision = 1;

    // Per-process unique bus name. The host watches for any name
    // matching this prefix on the bus.
    snprintf(s->bus_name, sizeof(s->bus_name),
             "org.kde.StatusNotifierItem-%d-%d",
             (int)getpid(), tray_id);

    s->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, s->bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired, on_name_acquired, on_name_lost,
        GINT_TO_POINTER(tray_id), NULL);
    return 1;
}

static void emit_signal_safe(int tray_id, const char* iface,
                              const char* sig, const char* path) {
    SniState* s = sni_for(tray_id);
    if (!s || !s->conn) return;
    g_dbus_connection_emit_signal(s->conn, NULL, path, iface, sig,
                                   NULL, NULL);
}

void aether_ui_sni_invalidate_icon(int tray_id) {
    emit_signal_safe(tray_id, "org.kde.StatusNotifierItem", "NewIcon",
                      "/StatusNotifierItem");
}

void aether_ui_sni_invalidate_tooltip(int tray_id) {
    emit_signal_safe(tray_id, "org.kde.StatusNotifierItem", "NewToolTip",
                      "/StatusNotifierItem");
}

void aether_ui_sni_invalidate_menu(int tray_id) {
    SniState* s = sni_for(tray_id);
    if (!s) return;
    s->menu_revision++;
    sni_refresh_menu_snapshot(tray_id);
    if (!s->conn) return;
    g_dbus_connection_emit_signal(s->conn, NULL, "/MenuBar",
        "com.canonical.dbusmenu", "LayoutUpdated",
        g_variant_new("(ui)", (guint32)s->menu_revision, 0), NULL);
}

void aether_ui_sni_shutdown(void) {
    for (int i = 0; i < SNI_MAX; i++) {
        SniState* s = &g_sni[i];
        if (!s->in_use) continue;
        if (s->sni_reg_id && s->conn)
            g_dbus_connection_unregister_object(s->conn, s->sni_reg_id);
        if (s->menu_reg_id && s->conn)
            g_dbus_connection_unregister_object(s->conn, s->menu_reg_id);
        if (s->owner_id) g_bus_unown_name(s->owner_id);
        s->in_use = 0;
    }
}
