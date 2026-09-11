# WaylandShader project handbook

Shared reference for the three WaylandShader versions. Backend detail lives in the chapters:

- [KDE / KWin](docs/KDE.md) — implemented, `main`
- [GNOME Shell / Mutter](docs/GNOME.md) — implemented, `gnome-backend`
- [niri](docs/NIRI.md) — not implemented; research and GPU probes recorded, `niri-backend`

This handbook and `docs/` are edited on `main` and copied unchanged into the other two worktrees, so relative links inside the chapters point at chapter files, while source paths are written as plain code paths qualified by branch. Written 2026-09-11 against the revisions in the table below. It records what the sources and retained verification establish; it does not report new builds, tests, or compositor runs except where a command is explicitly named.

## 1. The three versions

One git repository, three checked-out branches (`git worktree list`):

| Version | Branch, local worktree | Revision documented | Status | Package entry point |
| --- | --- | --- | --- | --- |
| KDE Plasma / KWin 6.6 | `main`, `/home/knd/AI/WaylandShader` | `514b06c` | Implemented; real-GL regressions and isolated two-output native checks recorded | `default.nix` (private shader library inline) |
| GNOME Shell / Mutter 50 (ABI 18) | `gnome-backend`, `/home/knd/AI/WaylandShader-gnome` | `e1bcfc0` plus uncommitted working-tree fixes dated 2026-09-11 | Implemented; installed on this host through the `waylandshader-gnome` input; native checks in disposable Shell sessions and a physical KMS-scanout check recorded | `gnome/default.nix`; the branch's `default.nix` still builds KWin |
| niri | `niri-backend`, `/home/knd/AI/WaylandShader-niri` | `6579b80` (identical to the KWin baseline) | Not implemented; renderer research, a GLES-versus-desktop-GL bridge probe and stock nested niri launches survive in a private handoff archive | none for niri; `default.nix` there builds the KWin package |

Shared by every version, byte-identical or branch-adapted:

| Piece | Where | Notes |
| --- | --- | --- |
| Shader host (`ShaderRuntime`) | `src/shader_runtime.{h,cpp}` | Desktop OpenGL/EGL host for librashader chains, per-output chain ownership, host-context save/restore. The GNOME branch extended it with parameter validation shared by live and bypassed edits. |
| Control clients | `src/controller_client.*`, `src/controller_main.cpp`, `src/ctl_main.cpp` | Qt Widgets GUI and JSON CLI. Built once per backend with a backend selection; the D-Bus service/path differs per compositor. |
| Private librashader patch | `patches/librashader-gl-lifetime.patch` | Identical in all three worktrees: sha256 `d749aea180cfa2ac1722ca3c4b9fa7eec5c3ab18431f91fb279dce8a5c7c7799`. Targets librashader **0.10.1** only. |
| Private library packaging | `default.nix` (KDE, inline) and `shader-library.nix` (GNOME branch) | Same recipe: assert 0.10.1, apply patch, rename to `libwaylandshader-rashader.so.2`, set that SONAME. The GNOME branch factored it into a file; `main` has not. |
| Real-GL fixture and test | `tests/shader_runtime_test.cpp`, `tests/fixtures/` | Multipass, LUT, mipmap, history and parameter pixel assertions against Mesa surfaceless EGL. |
| License | `LICENSE` | MIT, Copyright (c) 2026 man33li, merged at `6579b80`. The README sentence “no `LICENSE` file is currently included” predates that merge and is stale on all three branches. |

`TODO.md` on `main` is the original roadmap and still lists GNOME as planned; the GNOME branch's copy was updated. Treat the chapter status tables, not `TODO.md`, as the current state.

## 2. How the versions were built, incrementally

Dates are commit dates or recorded session dates; the granularity is coarse because the first KDE commit already carried nearly the whole implementation.

| Date | Event | Evidence |
| --- | --- | --- |
| 2026-09-09 | `7a5c019` “Nearly fully featured Wayland shader in KDE”: effect, runtime, GUI/CLI, CMake/Nix, dependency patch, fixtures, guide | `git log main` |
| 2026-09-09 | `ff42c5f` README with recorded native verification | `git log main` |
| 2026-09-10 | `4d67833`/`6579b80` license merged; `niri-backend` branched at the same revision to isolate a second compositor from the working KDE line | `git log`, `git worktree list` |
| 2026-09-10 | niri renderer/lifecycle research; throwaway probe comparing direct GLES with a desktop-GL/EGLImage bridge (GLES failed clamp-to-border, bridge passed); stock nested niri 26.04 launched and screenshotted; temporary probe tree removed; branch published unchanged | private handoff archive, see [niri chapter](docs/NIRI.md#5-successes-failures-and-limits) |
| 2026-09-10 | `e1bcfc0` “Gnome version first take” on `gnome-backend`: native `ClutterEffect`, GIR/typelib, GJS extension, GNOME package, `shader-library.nix` extraction | `git log gnome-backend` |
| 2026-09-10 | `c2fec31` idle-scheduling proposal (document only) | `git log main` |
| 2026-09-11 | `514b06c` evaluation of that proposal: rendered counterexamples, optimization **not** applied | `performance-enhancer-evaluation.md` |
| 2026-09-11 | GLF-OS module switched to install the GNOME package when the desktop is GNOME; stale user-local extension removed; fresh-session discovery | `/etc/nixos/customConfig/waylandshader.nix`, [GNOME chapter](docs/GNOME.md) |
| 2026-09-11 | Bypassed-parameter validation fix and GTK file-picker runtime fix in the GNOME working tree; retained `gnome/tests/parameter-bypass.js` | uncommitted `gnome-backend` changes, [GNOME chapter](docs/GNOME.md) |
| 2026-09-11 | This documentation set | `docs/`, this file |

The pattern that repeats across versions:

1. **One compositor first, natively.** Capture-overlay and portal designs were rejected before any code: they cannot replace the presented scene, they need capture permissions and feedback-loop prevention, and they do not carry input. Every version processes whole outputs inside the compositor's own frame path.
2. **Extract only what two backends actually share.** The runtime, clients, fixtures and dependency patch moved unchanged; KWin-specific ABI (effect factory, output UUIDs, `kwinrc`) did not. The GNOME branch factored `shader-library.nix`; KDE still inlines the same recipe.
3. **Branch per backend, keep the working one buildable.** Each branch's default build remains the KWin package until a native backend exists.
4. **Probe the GPU boundary before committing to a backend.** The niri work stopped after establishing which context/API route preserves shader semantics, rather than integrating an untested route.
5. **Evaluate optimizations against rendered pixels.** The static-shader scheduling proposal was tested with real fixtures and rejected in its proposed form.

## 3. Operating rules common to every version

Backend-specific commands are in the chapters. These rules apply everywhere.

1. **Build against the compositor's package set.** Compositor effect APIs (KWin, Mutter) are not binary-stable. Use the same `pkgs` that built the running compositor: on this host `(builtins.getFlake "/etc/nixos").nixosConfigurations.GLF-OS.pkgs`; elsewhere a matching `<nixpkgs>` or an explicit `--arg pkgs`. Version guards (`KWin < 6.7`, Mutter major `50`, librashader `== 0.10.1`) are compatibility stops, not obstacles to delete.
2. **Keep build outputs outside the checkouts.** `/etc/nixos/flake.nix` declares `waylandshader` and `waylandshader-gnome` as `path:` inputs with `flake = false`. Path inputs are content-addressed: any file inside the checkout, including documentation or a `result` link, changes the locked NAR hash. Use `--out-link` targets and build directories outside the tree. `.gitignore` excludes `/build/`, `/result`, `/result-*`, `/compile_commands.json` for the cases where that is impractical.
3. **Refresh only the changed input.** `sudo nix flake update <input> --flake /etc/nixos`, then `nixos-rebuild build` / `dry-activate` to review, then a manual `switch`. `glf-update` refreshes both inputs but stages the result for the next boot; it is not a switch. `customConfig/waylandshader.nix` picks the package from `glf.environment.type` (`gnome` → GNOME package, `plasma` → KDE package) and warns instead of failing when an input is missing.
4. **Never restart or replace the live Wayland compositor.** A native module update needs a fresh desktop login; a rebuilt file on disk does not replace a library already mapped into the compositor. Controller-only fixes need only a relaunched controller.
5. **Test in disposable sessions.** Nested/headless KWin or GNOME Shell with a private D-Bus, private `XDG_*` directories and its own outputs. Never hand a live-effect test the working desktop's bus address.
6. **Read-only status never enables anything.** `status`/`outputs`/`parameters` do not load or enable the effect; GUI launch and mutation commands may enable only this project's effect/extension.
7. **Presets are user-supplied GPU programs.** They include files, allocate GPU memory and exercise the driver compiler inside the compositor process. A private GL context isolates state, not the process or the GPU. Load trusted presets with all referenced assets; expensive or unknown presets go to a disposable session first.
8. **Distinguish the failure classes** before acting: build/discovery failure, compilation rejection (old preset retained), render-failure bypass, transport/timeout, native crash. Their remedies differ; a timeout in the CLI does not cancel work inside the compositor.

## 4. Why C++ was chosen

Decision: C++20 for the compositor-side effects, the shared shader host and the Qt controls; GJS for the GNOME Shell glue that GNOME mandates; Rust recommended for any future niri integration. This is a per-layer choice driven by what each host can load, not a language preference.

### Constraints that fixed the choice

- **KWin effects are C++ objects.** A KWin effect is a subclass of KWin's C++ `Effect` classes instantiated through `KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED`, using Qt's object system (moc/AUTOMOC), KWin's private effect/rendering headers and `Qt6::CorePrivate`. There is no C, Rust, Python or JavaScript entry point for a GL-processing effect. KWin's JavaScript/QML "scripted effects" animate window properties; they cannot own offscreen textures or run a multipass shader chain.
- **librashader is Rust with a C ABI.** The preset machinery (Slang compilation, multipass, LUTs, mipmaps, history, feedback) is consumed through `librashader.h`. A C-ABI-capable host was required; reimplementing the runtime in the host language was never on the table.
- **GNOME Shell extensions are GJS.** Shell loads `extension.js`. Native code reaches it only as a GObject type exposed through GObject Introspection (GIR/typelib). Mutter/Clutter/Cogl are C GObject APIs; the effect subclasses `ClutterEffect`.
- **niri is Rust/Smithay.** Its renderer traits, output lifetimes and scheduling are Rust; a compositor patch there is Rust by construction.

### Alternatives weighed

| Alternative | Why not, for the layers that exist today |
| --- | --- |
| Rust for the KWin/Mutter effects | No bindings to KWin's C++ effect classes or Qt private headers; a cxx/FFI shim would be as large as the effect and still C++ at the boundary. For Mutter it would need `gobject`/`clutter` crates plus GIR generation for a Rust GObject; possible in principle, but it would separate the effect from the C++ `ShaderRuntime` and Qt clients with no compositor-side gain. Rust remains the right answer for niri, where the host is Rust. |
| C | Feasible for GObject/Cogl code, impossible for KWin. The runtime's correctness depends on scope-bound ownership: `CurrentContext` saves and restores the caller's EGL API/display/context/surfaces on every path, chains are freed only while their context is current, partial construction unwinds. C would rebuild that with `goto` cleanup and lose the single shared runtime across backends. |
| KWin scripted effects (JavaScript/QML) | Animation-only API. No offscreen render targets, no custom GL, no C-ABI calls to librashader. |
| GJS only (no native module) for GNOME | Cogl through GI does not expose the raw EGL context, GL texture IDs and fence objects librashader's GL runtime needs, and GJS cannot call the librashader C API directly. A native GObject module was unavoidable; writing it in C++ let it reuse `ShaderRuntime` unchanged. |
| Python or other out-of-process languages | No in-process plugin path in KWin or Mutter. An external process can only be a client (D-Bus) or a capture viewer, and the capture-viewer design was rejected on rendering/input grounds. |
| QML or GTK for the controller | Qt Widgets shares `ControllerClient` (async D-Bus, schema/bounds validation, service-owner tracking) with the CLI in one language. Under GNOME the widget dialogs use the platform theme's GTK file picker, which is why the GNOME package wraps GTK 3 schemas. |

### What the choice costs, and how it is contained

- **ABI fragility.** KWin private headers, Qt private headers and Mutter ABI 18 are not stable. Contained by version guards in CMake/Nix, building against the compositor's exact package set, and treating upgrades as ports.
- **Manual ownership across FFI.** GL objects created inside Rust librashader, host textures owned by the compositor, fences shared between contexts. Contained by RAII wrappers in the host, the lifetime patch adding owning collections inside librashader, and the real-GL regression asserting host context/viewport restoration.
- **No memory safety inside the compositor process.** A fault in the effect is a compositor fault. Contained by catching errors at the C/GObject boundary (no exceptions into GJS or Clutter), rejecting invalid parameters before they reach the runtime, and retaining last-known-good runtimes on failed replacement.

## 5. Verified successes and observed failures

Each chapter carries a full table with evidence labels. The labels differ slightly by chapter (KDE: Code/Recorded/Reconstruction/Recommendation/Unverified; GNOME: code/recorded/observed/open; niri: SOURCE/HISTORICAL RECORD/PROPOSED/UNVERIFIED). They map onto the same three questions: is it in the source, was it exercised and when, or is it still open.

Cross-version summary:

| Established | Version | Where |
| --- | --- | --- |
| Multipass/LUT/mipmap/history pixel assertions on real GL, incl. host context restoration and GL 3.3 variant | all (shared test) | [KDE §5](docs/KDE.md#5-successes-failures-and-lessons) |
| Native two-output filtering, upright presentation, RGB identity/inversion, failed replacement keeps pixels, resize/fractional scale/disable-re-enable | KDE | KDE §5 |
| Per-view processing, small redraws without feedback, unsupported-output isolation, synthetic PQ/HDR identity, bypass-edit regression before/after fix | GNOME | [GNOME](docs/GNOME.md#successes-failures-and-lessons) |
| Physical primary-monitor result confirmed by direct KMS scanout; monitor screencasts capture the **unfiltered** scene on this system | GNOME | GNOME |
| Desktop-GL/EGLImage bridge preserves clamp-to-border where direct GLES did not (Mesa 26.1.8) | niri probe | [niri §5](docs/NIRI.md#5-successes-failures-and-limits) |

Observed failures worth remembering:

| Failure | Version | Lesson |
| --- | --- | --- |
| Silent RGBA8 fallback and unowned GL objects in upstream librashader | dependency | Fixed in the private patch; do not substitute stock librashader |
| Proposed lexical static-shader scanner classified animated shaders as static | KDE evaluation | Only compiled-runtime metadata may gate scheduling |
| Mutter 50 header forward-declares `ClutterPaintFlag` in a way C++ rejects | GNOME | Narrow build-local header copy with a failing guard, not an SDK edit |
| GNOME package built while the host module still selected KWin; `glf-update` staged a next-boot generation | GNOME deployment | Build ≠ deploy ≠ switch ≠ fresh-login discovery |
| Bypassed parameter edits rejected after the runtime was released | GNOME | Keep the parameter contract when releasing GPU state |
| GTK file picker aborted the controller (`org.gtk.Settings.FileChooser` missing) | GNOME | Declare indirect UI runtime dependencies; do not switch dialogs to hide it |
| Direct GLES sampled `0.75,0.25,0.75` outside the border where black was expected | niri probe | A compiling shader can still be semantically wrong; test pixels |
| Stock niri X11 nesting panicked on missing `libXcursor.so.1` | niri environment | Environment failure, not a shader bug; separate the two |

Open for every version: physical HDR/wide-gamut, rotation, hotplug/load races, lock/DPMS/resume matrix, VRR, latency and power. No numbers exist; none should be quoted.

## 6. Future refinements

Ordered by dependency, not by appeal.

1. **Compiled-runtime temporal metadata before any idle-scheduling optimization.** All backends currently self-schedule every selected Slang chain. The stable librashader C API does not expose the reflection needed; the private patch is the place to add a query. Unknown must stay continuous. Then gate self-scheduling per output in KWin `postPaintScreen()` / GNOME per-view timelines without touching damage expansion or invalidation. Details: [KDE §6](docs/KDE.md#6-future-refinements-correct-scheduling-before-performance-claims), [GNOME](docs/GNOME.md#future-refinements).
2. **Commit and publish the GNOME working-tree fixes.** `gnome-backend` carries the bypass-validation and GTK-picker changes uncommitted; no published `gnome-backend` ref was found during inventory.
3. **Unify the private-library packaging.** Move `main`'s inline derivation to `shader-library.nix` so both branches diff cleanly and a librashader bump is one change.
4. **Physical validation matrix** on representative hardware for both implemented backends: HDR/wide-gamut with real presets, rotation, hotplug during compilation, lock/unlock with temporal history, suspend/resume, VRR, latency, idle/active power. Record method, versions, preset and baseline for each measurement.
5. **niri native backend** following the gates in the [niri chapter](docs/NIRI.md#6-proposed-native-implementation-sequence-and-readiness-gates): pinned CachyOS baseline, reproducible probe, one final-output seam on both nested and DRM paths, per-output lifecycle, atomic replacement, damage/temporal scheduling with commit-generation advance, lock safety, then packaging.
6. **Ports, deliberately.** KWin 6.7+ and Mutter 51 are rejected by guards today. Port when needed by reviewing paint signatures, color-management helpers, effect discovery and private headers against the new target; do not widen the guard first.
7. **Upstream the librashader fixes** (owning GL collections, partial-construction cleanup, all-mip history init, feedback reset, framebuffer-format error instead of RGBA8 fallback) so the private patch can shrink.

## 7. Maintenance and patches

### Patch inventory and rules

- **One patch, one base.** `patches/librashader-gl-lifetime.patch` against librashader 0.10.1, applied by `pkgs.librashader.overrideAttrs` with the version assertion. The output is renamed `libwaylandshader-rashader.so.2` with that SONAME so it never shadows a distribution `librashader.so` for other applications inside the compositor process.
- **Edit in one worktree, copy to all three, verify identity.** After changing the patch: `sha256sum` across the three worktrees must agree. Rebuild both packages (`default.nix`, `gnome/default.nix`) with the matching `pkgs`.
- **Bumping librashader** is a port: re-apply the patch to the new base, re-check the C API/`libra_*` option structures used by `ShaderRuntime`, keep the private name/SONAME, update the assertion, re-run the real-GL CTest and the GNOME lifecycle test, then disposable-session native checks. Do not remove the assertion to make evaluation proceed.
- **The GNOME build-local Mutter header copy is not a dependency patch.** It adapts a C forward declaration for C++ at configure time and fails configuration if the upstream declaration changes. Keep its removal criterion (upstream fix) separate from the librashader patch's (upstreamed fixes).

### Verification ladder

| Layer | Command or method | Proves | Does not prove |
| --- | --- | --- | --- |
| Real-GL regression | KDE: `cmake -DWAYLANDSHADER_BUILD_TESTS=ON` then `ctest` (`shader_runtime`, `shader_runtime_gl33`; ECM's `KDECMakeSettings` adds `appstreamtest` when `appstreamcli` is on PATH, which it is in `shell.nix`, so three entries appear) | Chain semantics, ownership, host context restoration on Mesa surfaceless EGL | Plugin loading, GUI, compositor behavior |
| GNOME GI lifecycle | `gnome_effect_lifecycle` in the package build / `ctest` | Object ownership, pending-disable delivery, shutdown | Rendering |
| Live bypass regression | `gjs -m gnome/tests/parameter-bypass.js <private bus>` against a disposable headless Shell | Edit-while-bypassed contract on a real effect | Physical output |
| Isolated compositor session | Nested/headless KWin or Shell with private bus/XDG, real surfaces, post-paint or scanout readback | Actual presented pixels, input routing, replacement failure behavior | Physical display, HDR, power |
| Physical hardware | KMS scanout capture, instrumented power/latency | Display-level claims | Nothing beyond the measured configuration |

A CTest summary must be read with its executed-versus-skipped counts: the real-GL test returns 77 (Skipped) when Mesa surfaceless extensions are absent.

### Documentation maintenance

- Edit `PROJECT-HANDBOOK.md` and `docs/*.md` on `main`; copy them unchanged to the `gnome-backend` and `niri-backend` worktrees. Chapters therefore name branches and code paths, never relative links into source.
- Each chapter states the revision it documents. When a backend commit changes behavior described there, update the chapter's status line and evidence table in the same change set.
- Keep evidence labels honest: a source fact, a recorded verification with its date, or an open item. Never promote a build result to a runtime claim.
- The per-branch `README.md` files still describe the pre-license, pre-GNOME state in places (status table on `main`/`niri-backend`, license paragraph on all three). Reconcile them when those branches are next touched for code; the chapters are current.

### Branch hygiene

- Commit backend changes on their own branch. Do not merge `main`'s inline shader-library derivation over the GNOME branch's `shader-library.nix` without reconciling to the factored form.
- `niri-backend` is intentionally identical to the KWin baseline plus documentation; it is not a place to accumulate KDE changes.
- A rebuilt `path:` input needs `nix flake update <input>`; committing is not required for the host to pick up a working-tree change, which is convenient and also why a stale working tree can silently be what the system runs. State the worktree status when reporting host behavior.

### Reporting

Include: branch and revision (`git rev-parse HEAD`, working-tree status), compositor and version (KWin/Mutter/niri), Qt/nixpkgs/librashader package versions, GPU and Mesa/driver version, native versus nested session, preset with its assets and settings, output layout/scale, exact steps, and the backend CLI's `status` output. State which failure class applies.

## 8. Evidence and references

- Repository: [`main`](https://github.com/man33li/Wayland-Shader-KDE/tree/main), [`gnome-backend` package path](https://github.com/man33li/Wayland-Shader-KDE/tree/gnome-backend), [`niri-backend` at `6579b80`](https://github.com/man33li/Wayland-Shader-KDE/tree/6579b80e3cc7dd04ae3fd04f74c27fca5c8136bd). The GNOME link is the URL recorded in the extension metadata; no published `gnome-backend` ref was confirmed during inventory.
- Local host wiring: `/etc/nixos/flake.nix` (inputs `waylandshader`, `waylandshader-gnome`), `/etc/nixos/customConfig/waylandshader.nix` (desktop-based selection), `~/system-flake.nix` (re-merge source when GLF migrations drop custom inputs).
- Upstream: [librashader](https://github.com/SnowflakePowered/librashader), [KWin](https://invent.kde.org/plasma/kwin), [Mutter](https://gitlab.gnome.org/GNOME/mutter), [GNOME Shell](https://gitlab.gnome.org/GNOME/gnome-shell), [niri](https://github.com/niri-wm/niri), [Smithay](https://github.com/Smithay/smithay), [Extra CMake Modules `KDECMakeSettings`](https://api.kde.org/ecm/kde-module/KDECMakeSettings.html).
- Project records on `main`: `README.md`, `WaylandShader-Guide.txt`, `TODO.md`, `proposed-perfomance-enhancer.md`, `performance-enhancer-evaluation.md`.
