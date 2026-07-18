# Getting the Win32 + AppKit backends actually green — via remote build agents

*Advice from the aeb / aeb-agent sibling. Written 2026-06-13; macOS section
added 2026-06-14.*

You have a **3,200-line native Win32 backend** (`aether_ui_win32.c`) and a
**native AppKit/macOS backend** (`aether_ui_macos.m`), both wired into
`build.sh` (the `MINGW*|MSYS*` and `Darwin)` branches). What you almost
certainly *don't* have yet is **proof they compile and run on a real Windows
box / a real Mac** — "works for GTK4" usually means the Win32 and AppKit paths
have never met their platform compiler or event loop. That's the gap to close,
and you can close both **without owning the hardware on your desk**.

Two target machines, with **opposite access models** — and that difference
drives everything below:

| | **winbaz** (Windows) | **the Mac mini** (macOS) |
|---|---|---|
| What it is | a Windows 11 VM on the Bazzite NUC | a physical Mac mini |
| SSH from your box | **yes, passwordless** (`ssh winbaz`) | **no — and never will be** |
| So the agent runs… | …**on your Linux box**, dispatching *in* to winbaz over ssh (`run_on=vm`) | …**on the Mac itself**, hand-started by you (`run_on=host`) |
| You drive it by… | firing dispatches from your box (or by hand over ssh) | POSTing dispatches to the Mac's listening port |

The **winbaz** sections (A/B) assume that one-word ssh reach. The **macOS**
section is built around the opposite: no ssh, so you stand the agent up *on the
Mac* yourself and it builds locally. Jump to
[Part 2 — Getting AppKit green on the Mac mini](#part-2--getting-appkit-green-on-the-mac-mini-no-ssh)
if that's your target.

---

# Part 1 — Getting Win32 green on winbaz (ssh-reachable)

## What winbaz already is (verified, not assumed)

- **Reachable as `ssh winbaz`** from this box — an `~/.ssh/config` alias
  (`HostName 192.168.122.179`, `User paul`, `ProxyJump bazzite@192.168.0.57`),
  passwordless. It's a libvirt guest on the Bazzite NUC.
- **MSYS2 + MinGW64** installed: **gcc 16.1.0**, `make`, `pkg-config`. gcc is
  NOT on the bare-login PATH — you must `export PATH=/mingw64/bin:/usr/bin:$PATH`
  first (this bites everyone; see the quoting gotcha below).
- **aetherc.exe + ae.exe** are built at `/c/Users/paul/aether/build/`. So the
  whole `aetherc → C → gcc → .exe` chain your `build.sh` needs is present.
- It is **not autostart** — if `ssh winbaz` hangs, the VM is probably off. Ask
  the Bazzite host to start it:
  `ssh bazzite@192.168.0.57 'virsh -c qemu:///system start win11'`
  (libvirt domain name is `win11`; the *guest hostname* is WINBAZ).
- **rsync is NOT installed** on winbaz, and there's no GTK4/GDI dev-lib
  install needed — Win32 links against libraries that ship with Windows.

> Your `aether-ui` is **not an aeb project** (no `.build.ae`; you drive
> `aetherc` + `gcc` yourself via `build.sh`). That's fine — it means the
> simplest path below doesn't involve aeb at all. aeb-agent enters only if you
> want policy-gated, CI-style remote dispatch later.

---

## Path A — the 20-minute path: run your own `build.sh` on winbaz over ssh

This is the run_on(vm) *mechanism* (ship the tree → build on the VM → fetch the
result), done by hand with the tools you already have. No aeb, no agent.

Because **rsync is absent**, ship with `tar | ssh` instead of `rsync`:

```bash
# From /home/paul/scm/aether-ui on THIS box:

# 1. Ship the source tree to winbaz (lands in C:\Users\paul\aether-ui).
#    --exclude build/target so you don't copy Linux .o/.so over.
tar czf - --exclude=build --exclude=target --exclude=.git . \
  | ssh winbaz 'C:\msys64\usr\bin\bash.exe -lc "mkdir -p ~/aether-ui && cd ~/aether-ui && tar xzf -"'

# 2. Build the Win32 example ON winbaz. Note the MinGW64 PATH + aetherc on PATH.
#    Write the recipe to a local script and run it remotely — DON'T try to
#    inline this over ssh (nested quoting will bite you; see below).
cat > /tmp/winbuild.sh <<'EOF'
export PATH=/mingw64/bin:/usr/bin:/c/Users/paul/aether/build:$PATH
cd ~/aether-ui
./build.sh example_counter.ae build/counter
echo "exit=$?"
ls -la build/counter.exe
EOF
scp /tmp/winbuild.sh winbaz:                       # → C:\Users\paul\winbuild.sh
ssh winbaz 'C:\msys64\usr\bin\bash.exe -l /c/Users/paul/winbuild.sh'
```

If `build.sh` finds `aetherc` (it calls the bare name — so the `…/aether/build`
dir must be on PATH, as above) you'll get `build/counter.exe`. **First run will
likely surface the real porting bugs** — that's the point; they were invisible
on Linux.

### Two bugs you will probably hit first (forewarned)

1. **`${...}` string interpolation prints empty on native Windows.** A known
   open Aether *codegen* bug (string literals survive; interpolated values
   drop). If your `.ae` examples render labels via `"count: ${n}"` and they
   come out blank, that's this — not your backend. It's written up in the aeb
   tree at `asks/aether-windows-string-interpolation-empty.md` (repro +
   suspected `codegen_expr.c` site). Until it's fixed, test with literal text
   or `string.concat`, and don't chase it inside `aether_ui_win32.c`.

2. **MSYS2-over-ssh quoting.** Driving MSYS2 bash through `ssh winbaz '…'` with
   nested heredocs / inline `source` gives spurious exit 2/127 and *swallowed
   output*. The reliable pattern is exactly what Path A does: **write the
   script locally, `scp` it, then `ssh winbaz 'C:\msys64\usr\bin\bash.exe -l
   /c/Users/paul/foo.sh'`.** Also: `scp foo winbaz:` lands in `C:\Users\paul`
   (= `/c/Users/paul` in MSYS2); Windows `/tmp` ≠ MSYS2 `/tmp`, so don't
   `scp winbaz:/tmp/...`.

### Then: drive the GUI headlessly (your killer feature here)

You don't need to *look* at the window to know the backend works. Your
`aether_ui_test_server.c` exposes the **AetherUIDriver over HTTP**, and — per
your own header — the **Win32 backend already wires it via `SendMessage` to a
hidden `AE_WM_DRIVER` window**. So:

```bash
# On winbaz (via an scp'd script): launch a testable example, then drive it.
#   build/perry_testable.exe &           # starts the HTTP driver on :9222
# From THIS box, tunnel the driver port out and run your existing harness:
ssh -N -L 9222:127.0.0.1:9222 winbaz &   # forward the driver port
./test_automation.sh 9222                # your existing curl-based assertions
```

> These `&` are in *your own* ssh session, so you own their lifetime. If you
> instead drive this through an agent **dispatch `command`** (run_on=vm to
> winbaz), the agent reaps backgrounded processes by default — set
> `"keep_alive": true` so the driver survives the build step. See
> [`keep_alive`](#keep_alive--or-the-agent-reaps-your-driver-out-from-under-you) below.

That turns "does the Win32 backend work?" into a **pass/fail you can read in
this terminal** — clicks, field edits, label assertions — against a real
USER32 window on real Windows, with no remote desktop. This is the highest-
leverage thing you can do: get `test_automation.sh` green against winbaz and
you have a regression gate for the whole port.

---

## Path B — the aeb-agent path: when you want it CI-shaped

Path A is a human at a keyboard. **aeb-agent** is the same loop turned into a
*service*: a long-running, auth-gated build server you dispatch patches at and
get structured verdicts back from — no commits, ephemeral, leased. Reach for
this once Path A is green and you want (a) other people / CI to trigger Windows
builds without ssh access to winbaz, or (b) "build *this uncommitted patch* on
Windows and tell me pass/fail" as a one-shot.

The shape (all built and proven on the aeb side):

- **run_on=vm dispatch.** An agent running *here* (or anywhere Linux) accepts a
  dispatch and `run_on=vm` rsyncs the tree to an ssh target, runs the build
  there, rsyncs artifacts back. The ssh target is an ssh-config alias that owns
  the key/ProxyJump — i.e. **`--vm-host winbaz`** drops straight in. Fail-closed
  without `--vm-host`.
- **Lease auth.** Dispatches carry an HMAC-signed, expiring, *purpose-bound*
  token (`ae1.<purpose>.<expiry>.<sig>`); the agent verifies, never mints. So
  "let CI build aether-ui on Windows for the next 30 min, nothing else" is a
  single minted lease, not a standing secret.

### What it costs you to adopt — **now nearly nothing** (raw-command mode shipped)

The raw-command mode you'd have needed a `.build.ae` bridge for is **now built
and proven against winbaz itself** (aeb commit 2026-06-13). You drive your
*existing* `build.sh` directly — no bridge, no `.build.ae`, no `aeb` on the VM,
**and no `rsync` on the VM** (the transport auto-falls-back to `tar|ssh`, which
is exactly your Path-A trick, now inside the agent). Concretely, the proof run
that landed it was *your* shape:

```
# A run_on=vm agent started like this (the operator's side, once):
aeb-agent --run-on vm --vm-host winbaz \
          --vm-shell 'C:\msys64\usr\bin\bash.exe -lc' \   # winbaz's ssh shell is cmd.exe
          --allow-vm-command \                            # opt-in: arbitrary VM command (fail-closed without it)
          --lease-secrets /path/to/secret  ...            # NOTE: --lease-secrets (plural), file of 1+ secrets

# A dispatch carrying YOUR build command (the "command" field):
{ "guid":"…", "purpose":"preint/x", "target":".build.ae",
  "command":"export PATH=/mingw64/bin:/usr/bin:$PATH; ./build.sh example_testable.ae build/testable" }
# → ships the tree (tar), runs build.sh on winbaz, returns pass/fail from its exit code.
```

That run compiled and *ran* a Windows `.exe` and returned `result:pass`. Three
things to know:

- **`--allow-vm-command` is required** and off by default — a leased agent
  won't run an arbitrary command unless the operator opted in (fail-closed; a
  dispatch with a `command` to an agent without the flag is `rejected`).
- **`--vm-shell 'C:\msys64\usr\bin\bash.exe -lc'`** is needed for winbaz because
  its *default* ssh shell is cmd.exe (can't `cd … && …`). This wraps every
  remote command in MSYS bash.
- **Put `/mingw64/bin` on PATH inside your `command`** (as above) — `bash -lc`
  doesn't, so `gcc` won't be found otherwise. This is the same PATH gotcha as
  Path A.

Originator side: the simplest trigger is `aeb --use-remote-agents <target>` with
`AEB_VM_COMMAND` set to your build line; or build the dispatch JSON yourself
with the `command` field. **Nothing needs installing on winbaz** for this —
that was the whole point of the tar fallback.

(The old `.build.ae`-bridge workaround is no longer necessary; skip it.)

---

## Recommended order

1. **Path A, `example_counter.ae`** → get one `.exe` to compile + run on winbaz.
   Expect the `${...}` codegen bug and the quoting gotcha; neither is your code.
2. **Path A + AetherUIDriver** → get `test_automation.sh` green over the tunnel.
   Now you have a Windows regression gate.
3. Work through the other examples + the honest stubs in `aether_ui_win32.c`
   (resize re-flush `WM_SIZE`, `canvas_write_png`, tray/notification message
   loop) against that gate.
4. **Only then** consider Path B (aeb-agent + lease) if you want CI / other
   triggers. The raw-command mode is now live, so Path B needs **nothing
   installed on winbaz** — drive your `build.sh` directly with `--vm-shell` +
   `--allow-vm-command` + a `command` dispatch (see above).

The `run_on=vm` raw-command mode you'd have asked for is **done** — the small,
clean addition to `_run_on_vm` shipped, with the tar-fallback transport and the
`--vm-shell` Windows wrapper, and was proven against winbaz.

---

# Part 2 — Getting AppKit green on the Mac mini (NO ssh)

The Mac mini is a different shape from winbaz: **there is no ssh access to it
from your box, and there won't be.** So the winbaz model — a Linux agent
reaching *in* over ssh — does not apply. Instead you **run aeb-agent natively on
the Mac**, and it builds in `run_on=host` mode: `build.sh` runs locally on the
Mac, no ssh hop, no transport, no shell-wrapping. The Mac is *self-sufficient* —
aeb is already built there. You (or anyone with the lease secret) then POST
dispatches to the port it listens on.

This is the inverse of winbaz, and it's actually simpler: no `--vm-host`, no
`--vm-shell`, no tar transport, no PATH-export-in-the-command dance (a login
shell on macOS has clang on PATH).

> **Why host mode, not vm mode:** `run_on=vm` *requires* the agent to ssh to the
> build node. With no ssh to the Mac, that's a non-starter — and pointless,
> since the agent is already *on* the build node. `run_on=host` + the
> raw-command escape hatch (shipped 2026-06-13) is exactly the native-builder
> case.

## One-time setup, on the Mac itself (you do this by hand)

aeb is already installed on the Mac. Build the agent + lease tools and start it:

```bash
# On the Mac mini, as your user (NOT sudo — see the gotcha):

# 1. Build the opt-in agent kit from your aeb checkout (one target installs all three):
aeb tools/remote-agent/.install.ae   # → ~/.local/bin/{aeb-agent, aeb-lease, aeb-keygen}

# 2. A signing secret for lease auth — use aeb-keygen, NOT `openssl rand`. The
#    agent REFUSES a bare blob: a real secret carries an "aeb-secret-v1:" marker
#    (greppable for security sweeps) and clears an entropy floor. aeb-keygen
#    emits exactly that; a raw `openssl rand -hex 32` is now rejected at startup.
mkdir -p ~/.aeb && aeb-keygen > ~/.aeb/lease.secret && chmod 600 ~/.aeb/lease.secret

# 3. Clone aether-ui on the Mac, then run the agent natively in host mode.
#    NB: with --max-jobs 1 (default) the single build tree is ~/aether-ui itself.
#    For N>1 concurrent builds each slot needs its own tree <workdir>/<i>; see
#    "Concurrency on the Mac" below — for one Mac doing one build at a time,
#    the default (1 slot, build in --workdir) is exactly right.
aeb-agent --host 0.0.0.0 --port 9440 --accept 'preint/*' \
          --workdir ~/aether-ui --repo ~/aether-ui \
          --run-on host --allow-vm-command \
          --lease-secrets ~/.aeb/lease.secret      # plural: a file of 1+ secrets (rotation)
```

- `--run-on host` + `--allow-vm-command`: the dispatch's `command` field runs
  natively in `~/aether-ui`. `--allow-vm-command` is the same opt-in fail-closed
  gate as winbaz (without it, a `command` dispatch is `rejected`).
- `--host 0.0.0.0`: reachable on the LAN. If you'd rather lock it down, bind
  `127.0.0.1` and reach it only from the Mac itself, or add
  `--allow-from <ip>[,<ip>...]` (an exact-IP source allow-list, checked before
  auth against the trusted peer address — defense-in-depth under the lease).
- **No `--vm-shell`, no `--vm-host`, no PATH export** — all the winbaz
  workarounds are absent because there's no ssh hop and macOS's shell already
  has clang.
- **One build at a time, automatically.** The agent defaults to `--max-jobs 1`:
  it builds one dispatch at a time in `~/aether-ui` and replies **`503 busy`** to
  any dispatch that arrives mid-build (your originator can retry). That's exactly
  what you want for a single Mac — no extra flags. (If you ever want N concurrent
  AppKit builds, `--max-jobs N` gives each its own tree `~/aether-ui/<i>`, which
  you'd pre-clone; for one Mac, leave it at 1 and build in `~/aether-ui`.)
- **Check the toolchain before dispatching:** an authed `GET /ping` now reports
  `aeb_version` + `aether_version` (the *live* aetherc the Mac would build with).
  Handy given AppKit builds need Aether ≥ 0.256 — probe `/ping` and you'll see
  the Mac's version without waiting for a build to fail.

## Firing a build (from anywhere that can reach the Mac's port)

```bash
# Mint a lease (on the Mac, or wherever the secret lives):
TOK=$(aeb-lease --secret ~/.aeb/lease.secret --purpose 'preint/macos' --ttl-mins 30)

# POST a dispatch whose "command" is your AppKit build:
curl -s -X POST http://<mac-ip>:9440/dispatch \
  -H "X-AEB-Token: $TOK" -H "Content-Type: application/json" \
  -d '{"guid":"mac1","purpose":"preint/macos","target":".build.ae",
       "command":"./build.sh example_counter.ae build/counter"}'
# → {"status":"done","result":"pass|fail","log":"<clang output…>"}
```

clang compiles `aether_ui_macos.m` against AppKit; you read pass/fail back. The
exact `run_on=host` + raw-`build.sh` path was proven end-to-end before it
shipped.

## Headless AppKit test gate (same killer feature as Windows)

Your `aether_ui_test_server.c` wires the AetherUIDriver on macOS via
`dispatch_async` to the main queue. So make the dispatch `command` launch a
testable example in the background, then drive it. Since there's no ssh tunnel
here, point your harness at the Mac's driver port directly (bind it to the LAN,
or run `test_automation.sh` *on* the Mac):

```bash
#   command:    "./build.sh example_testable.ae build/testable && build/testable &"
#   keep_alive: true        # <-- REQUIRED for a backgrounded server (see below)
#   (the driver listens on :9222)
./test_automation.sh <mac-ip>:9222   # or run it on the Mac against 127.0.0.1:9222
```

A real AppKit window, driven headlessly, pass/fail in your terminal — a macOS
regression gate to match the Windows one.

### `keep_alive` — or the agent reaps your driver out from under you

By default the agent **reaps every process a dispatch spawns** when the command
returns: it runs your `command` in its own process group and group-kills
survivors (TERM → grace → KILL), the same discipline the `aeb` trampoline uses.
That's deliberate — a stray `… &` used to outlive the dispatch, hold :9222, and
**wedge the single build slot** (every later dispatch got `503 busy`, recoverable
only by `pkill` on the box — impossible on the headless Mac). Reap-by-default
closes that footgun.

But the driver gate *needs* `build/testable` to survive the build step so the
harness can drive it. So a dispatch that backgrounds a server **must set
`"keep_alive": true`**. Then the agent runs the command unreaped and returns
`"kept_alive": true` in the verdict — your signal that a process is still up and
**you own its teardown**. Tear it down with a follow-up dispatch:

```bash
#   command:    "pkill -f build/testable"
#   keep_alive: false      # default — this teardown command itself IS reaped
```

Without `keep_alive`, your backgrounded `build/testable &` is killed the instant
`build.sh` returns, and `test_automation.sh` finds nothing on :9222.

## Two gotchas (both learned the hard way)

1. **Do NOT `sudo make install` aeb into `~/.local`.** `~/.local` is a *per-user*
   prefix — installing there never needs root. `sudo make install` makes
   `~/.local/share/aeb` **root-owned**, and then the non-sudo
   `aeb tools/agent/.install.ae` fails with `cp: … Permission denied`. (Recent
   aeb auto-chowns back when it detects a sudo install into a home prefix, but
   the clean habit is: plain `make install`, no sudo.) If you already hit it:
   `sudo chown -R $(whoami):staff ~/.local/share/aeb ~/.local/bin/aeb`, then
   re-run the agent install without sudo.

2. **aeb-agent needs Aether ≥ 0.256** (it uses `http.request_remote_addr` for
   the `--allow-from` gate). If `aeb tools/agent/.install.ae` fails with
   `Undefined function 'http.request_remote_addr'`, the Mac's `aetherc` is older
   than 0.256 — rebuild Aether from `main` on the Mac and `make install` it (no
   sudo), then retry. `aetherc --version` confirms.

## Peer-equivalence checks — verified on the Mac (2026-07-18)

The items below landed + were verified on GTK4/win32 first; this is the record
of running them on the Mac and ticking them off. Full `tests/spec_matrix.sh` is
**126/126 green** on macOS after this work. Only the two-way textfield binding
needed an AppKit-specific fix; every other batch was correct by construction and
this was a pure verification pass.

- **Native menu bar** (commits dcdb68f/61da9d3) — ✅ green, no code change, as
  predicted. `spec_menu` is 4/4 and `GET /menus` returns exactly
  `[{"handle":2,"items":["New","Open...","Save","Quit"]},{"handle":3,"items":["Undo","Redo"]},{"handle":4,"items":["About"]}]`.
  macOS's real `NSMenu` bar + `menu_add_item` side-store recording + the shared
  test-server's `/menus` and `/menu/{h}/activate` routes were already all in
  place.

- **Two-way textfield binding** (commit 04e6023) — ✅ 7/7, but it needed **one
  AppKit-specific fix**. The watch-out came true: AppKit does *not* emit
  `controlTextDidChange` for a programmatic `setStringValue:` (unlike GtkEntry's
  `changed` and win32's `EN_CHANGE`, which both fire on any set), so the
  field→state leg was dead. Two things were wired to close it:
  1. `aether_ui_textfield_set_text` now mirrors a bound field into its state
     (checks the `AetherTextFieldDelegate.stateHandle`), guarded by a
     `g_seeding_bound_field` flag that `apply_prop_binding` raises around the
     state→field seed. Without the guard the seed's `set_text(field, c->str)`
     re-entered `state_set_s` with a pointer aliasing the cell's own string —
     `state_set_s` frees before copy → use-after-free → segfault + corrupted
     state. The guard makes only *external* sets propagate, matching the other
     backends' change-signal semantics.
  2. The driver's `AETHER_DRV_SET_TEXT` handler now routes through
     `aether_ui_textfield_set_text` instead of a raw `setStringValue:`, so
     driver `/set_text` reaches the write-back (the earlier raw path fired the
     field's *action*, never `controlTextDidChange` nor the write-back).

- **Reactive bindings — each_bind + computed state** (commit aa6c1a7) — ✅ 5/5
  on `rbind_demo`, no code change. Backend-agnostic (the
  `aether_ui_state_on_change` observer + `AEUI_STATE_LIST` cell, mirrored
  verbatim in `aether_ui_macos.m`).

- **File dialogs — open_file / save_file** (commit 27b0ef2) — ✅ implemented +
  links. macOS uses `NSOpenPanel` / `NSSavePanel`, both `aeui_is_headless`-
  guarded to `strdup("")` so a headless click returns "" without hanging on
  `runModal`. No spec (native modal); a real pick needs a human at the Mac.

- **Typography / multi-select / row double-click** (commit 06543a9) — ✅ 3/3/2
  on `typo_demo` / `multiselect_demo` / `dblclick_demo`, no code change. macOS
  mirrors: text_wrapped uses `NSTextField wrappingLabelWithString` + a width
  constraint; multi-select is pure module.ae; double-click's fire path uses
  `objc_setAssociatedObject` (a plain closure invoke, no NSEvent → headless-safe).

- **Tree mode + table delegate cells + table_bind** (commit 3e684db) — ✅ 3/2 on
  `tree_demo` / `tabledeleg_demo`, no code change. ALL pure module.ae — no
  backend/C, so macOS behaves identically to GTK4+win32.

## macOS vs winbaz — the cheat sheet

| | winbaz | Mac mini |
|---|---|---|
| agent flags | `--run-on vm --vm-host winbaz --vm-shell 'C:\msys64\usr\bin\bash.exe -lc' --allow-vm-command` | `--run-on host --allow-vm-command` |
| transport | tar\|ssh (no rsync on winbaz) | none — builds in place |
| PATH in `command` | must `export PATH=/mingw64/bin:…` | not needed |
| where the agent runs | your Linux box | the Mac itself |
| `${...}` codegen bug | yes (Windows) | n/a |

---

— your aeb sibling
```
References on the aeb side:
  docs/aeb-agent-operating.md                   (operator how-to: all flags, both auth modes, run modes)
  docs/run-policy-class-and-cloud-leverage.md   (run_on=host/vm, lease auth, image selection)
  the winbaz VM facts + MSYS2 quoting law live in the aeb session's memory
```
