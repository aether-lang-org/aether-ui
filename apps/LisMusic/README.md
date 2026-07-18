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
worker thread. **Three of those four are real here:**

- **SQLite** — `lis_store` is backed by `contrib.sqlite`, faithful to the
  original's `history` schema, persisted to `history.db` across runs.
- **Audio playback** — `lis_audio` is backed by `std.audio` (0.413, vendored
  miniaudio): `load_wav` decodes a file into a source, `play`/`pause`/`seek`/
  `volume` operate on it, real device output (null backend under headless CI).
  The My-Music tab plays a bundled tone as the worked proof.
- **Async network search** — `lis_net` runs a real HTTP+JSON fetch on a
  `std.worker` background thread (`std.worker` shipped 0.413, answering an ask
  this port filed), delivering parsed results back on the loop thread so the
  window never freezes; drained via a `ui.timer` pump. `LIS_OFFLINE=1` forces a
  deterministic canned path for the driver spec.

**Cover art** (`lis_cover`) is now half-real: the DECODE+display path is
real (`ui.image_from_bytes` decodes encoded image bytes straight into a
widget — GdkTexture / GDI+ / NSImage, no temp file), so the player bar
shows a real decoded cover. Only the EXTRACTION half stubs — pulling the
embedded art *out of* an audio container (ID3 APIC / FLAC PICTURE, the
original's FFmpeg route) has no in-process extractor yet, so it shows a
bundled cover rather than the song's own art.

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

The backend subsystems are isolated behind clearly-marked **seams**
(`lis_audio`, `lis_store`, `lis_cover`, `lis_net`) — each a small module
with the original's method signatures. Three are fully **real** (`lis_store` → contrib.sqlite, `lis_audio` →
std.audio, `lis_net` → std.worker + std.http + std.json), and
`lis_cover`'s decode+display half is real too (`ui.image_from_bytes`);
only cover EXTRACTION from the audio container still stubs. Wiring a
real backend in later means implementing one seam module, not rewriting
the app — the real ones are worked examples of exactly that. Each remaining stub says so at its call site.

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
| `musicplay.{h,cpp}` | `lis_audio.ae` | **seam**: playback (**REAL** — std.audio) |
| `savehisty.{h,cpp}` | `lis_store.ae` | **seam**: SQLite persistence (**REAL** — contrib.sqlite) |
| `ffmpegsolve.{h,cpp}` | `lis_cover.ae` | **seam**: cover art — decode REAL (ui.image_from_bytes); extraction stubs |
| `netmusic*.{h,cpp}`, `musicworker.{h,cpp}` | `lis_net.ae` | **seam**: HTTP/JSON API (**REAL** — std.worker + std.http + std.json) |

## Tests

A driver spec (`tests/LisMusic/spec_lismusic.ae`, ci Phase 7 / spec_matrix
row `lismusic`) drives the running app over the AetherUIDriver and proves
the real behaviours: the three-region shell renders, sidebar nav switches
the right-page tab stack, search populates the results each-list, and the
transport play/pause + result Play buttons are wired. 5/5 — on GTK4,
AppKit, and Win32 (the win32 tab strip went real 2026-07-18; verified on
winbaz).

## Build & run

```
aeb apps/LisMusic
AETHER_UI_TEST_PORT=9222 ./target/build/apps/LisMusic/bin/LisMusic   # driver-testable
```
