# LisMusic (远征队音乐) — Aether-UI port

A port of **LisMusic**, a NetEase-Cloud-Music-style desktop music player,
from its original **Qt 6 / QML + C++** implementation to the Aether-UI
toolkit.

## Credit

The original LisMusic is by **SLTwitness**
(<https://github.com/SLTwitness>, 2039935260@qq.com) — see the upstream
repository for the Qt/QML source, screenshots, and release binaries. All
of the design, the UI, the feature set, and the artwork are theirs; this
directory is a structural translation of that work onto a different
toolkit. The original carries no explicit licence.

The Chinese UI strings of the original have been translated to English
here (the sidebar sections, the player chrome, the settings labels, etc.).

## What this port is — and is not

The original is a full client: it plays audio (QtMultimedia), persists
history and local-library paths in **SQLite**, extracts cover art with the
**FFmpeg** CLI, and pulls songs from a third-party HTTP+JSON API on a
worker thread. **Aether-UI has no peer for four of those subsystems**
(audio playback, SQLite, an image-decode-from-bytes path, and a
threaded-network story wired into the widget loop), so a faithful,
*working* clone is not achievable on this toolkit today.

What this port **does** deliver, faithfully, is the **UI and its
structure**:

- the same **decomposition** — one Aether module per original QML/C++
  concern (see the file map below);
- the real **three-region shell**: left sidebar (nav sections + "My
  Music"), right stack-paged content, bottom player bar;
- the **Config event-bus** — the original's `Config.qml` singleton
  (signals + shared state) rebuilt on Aether-UI's typed state + bindings,
  which is the same observer/event-bus pattern;
- the page-stack navigation, the search field, the login popup as a modal
  overlay, and the now-playing chrome (title / artist / transport /
  seek / volume).

The four missing subsystems are isolated behind clearly-marked **seams**
(`lis_audio`, `lis_store`, `lis_cover`, `lis_net`) — each is a small
module with the original's method signatures and an honest stub body
(e.g. `lis_audio.start_play()` logs and no-ops). Wiring a real backend in
later means implementing one seam module, not rewriting the app. Every
stub says so at its call site.

## File map (original → port)

| Original (QML/C++) | This port | Concern |
|---|---|---|
| `Config.qml` (singleton) | `lis_config.ae` | event bus + shared state |
| `LisMusic.qml`, `main.qml` | `lismusic.ae` | window + three-region shell |
| `LeftPage.qml` | `lis_leftpage.ae` | sidebar nav |
| `RightPage.qml`, `RightTopTitle.qml` | `lis_rightpage.ae` | top bar + page stack |
| `BottomSection.qml` | `lis_bottom.ae` | now-playing player bar |
| `Searchtop.qml`, `SearchData.qml` | `lis_search.ae` | search field + results |
| `Loginpop.qml`, `OtherLoginpop.qml` | `lis_login.ae` | login popup (framework only, as in the original) |
| `ButtonHover.qml`, `ChoseButton.qml`, `SelectBox.qml` | `lis_widgets.ae` | reusable UI bits |
| `musicplay.{h,cpp}` | `lis_audio.ae` | **seam**: playback (stub) |
| `savehisty.{h,cpp}` | `lis_store.ae` | **seam**: SQLite persistence (stub) |
| `ffmpegsolve.{h,cpp}` | `lis_cover.ae` | **seam**: cover extraction (stub) |
| `netmusic*.{h,cpp}`, `musicworker.{h,cpp}` | `lis_net.ae` | **seam**: HTTP/JSON API (stub) |

## Build & run

```
aeb apps/LisMusic
AETHER_UI_TEST_PORT=9222 ./target/build/apps/LisMusic/bin/LisMusic   # driver-testable
```
