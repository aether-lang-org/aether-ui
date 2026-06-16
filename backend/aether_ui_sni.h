// Aether UI — Linux StatusNotifierItem + DBusMenu over GDBus.
//
// The path-forward implementation the system-tray ask described:
//   - org.kde.StatusNotifierItem  (KDE freedesktop spec, used by
//     GNOME shell with the appindicator extension, KDE Plasma,
//     XFCE+sntray, Cinnamon, Budgie, …)
//   - com.canonical.dbusmenu      (sister protocol for the popup
//     menu that hangs off the SNI's Menu property)
//
// Both run over the session bus via GDBus (part of GIO, which is a
// transitive dep of GTK4 — so no new packages on any distro).
//
// Distro-agnostic — this beats the GTK3-only
// libayatana-appindicator path, which doesn't work under GTK4.
//
// All exposed state lives in aether_ui_system_extras.c's tray
// registry; this file is the "render the registry as an SNI on the
// session bus" backend. The registry is still the single source of
// truth — driver clicks via AetherUIDriver and real user clicks via
// SNI both bottom out in the same aether_ui_tray_emit_click() /
// aether_ui_tray_menu_activate() dispatch helpers.
//
// Build gating: included into aether_ui_gtk4.c only on Linux. No
// runtime feature flag — when StatusNotifierWatcher is missing on
// the session bus (no DE installed, headless server, …), the
// registration call fails gracefully and the tray functions degrade
// to registry-only.

#ifndef AETHER_UI_SNI_H
#define AETHER_UI_SNI_H

#ifdef __cplusplus
extern "C" {
#endif

// Publish a tray record on the session bus. Idempotent — calling
// twice for the same tray_id replaces. Returns 1 on bus registration
// success, 0 if the StatusNotifierWatcher is unavailable (caller
// keeps the registry-only path).
int aether_ui_sni_register(int tray_id);

// Notify the SNI host that the tray's icon path / tooltip / menu has
// changed since registration. The host re-reads the relevant
// property and re-renders. No-ops cleanly if the tray was never
// SNI-registered (degraded mode).
void aether_ui_sni_invalidate_icon(int tray_id);
void aether_ui_sni_invalidate_tooltip(int tray_id);
void aether_ui_sni_invalidate_menu(int tray_id);

// Tear down all SNI registrations. Called from atexit so the bus
// names are released cleanly on graceful exit; not strictly required
// (the bus daemon reclaims on disconnect) but tidy.
void aether_ui_sni_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
