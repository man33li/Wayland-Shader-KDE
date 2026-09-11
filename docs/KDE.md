# KDE / KWin: constructing the native desktop effect

[Project handbook](../PROJECT-HANDBOOK.md) · [GNOME chapter](GNOME.md) · [niri chapter](NIRI.md)

This chapter describes the KDE implementation in the **`main` branch**, inspected at `514b06c`. It explains how its pieces fit together, a dependency-ordered way to reconstruct the implementation, and how to build and operate it safely. It is not a claim that each stage was a separate commit or happened in this exact order. The initial KDE commit already contains almost all of the implementation.

## 1. Status, evidence labels, and source map

**Implemented:** a native KWin 6.6 effect for Wayland using desktop OpenGL/EGL, a patched librashader 0.10.1, a Qt settings window, a CLI, and per-monitor profiles. It processes the compositor's scene in place. It is not a capture viewer, overlay, Wine application, or generic Wayland protocol implementation.

The KDE package does not install the GNOME integration or implement niri. Read the sibling chapters for their independently established source states; do not infer backend support from a branch name or from an old cross-desktop roadmap.

Evidence labels used below:

- **[Code]** — behavior directly represented in the inspected KDE source or build configuration. This is not a new runtime result.
- **[Recorded]** — an earlier verification described in the existing guide, README, or performance evaluation. No compositor, build, test, or benchmark was run to write this chapter.
- **[Reconstruction]** — a useful dependency order derived from the finished implementation, not a recovered engineering diary.
- **[Recommendation]** — proposed maintenance or development work, not shipped behavior.
- **[Unverified]** — a remaining question not established by the available record.

All implementation paths in the following table belong to **KDE `main`**. They are deliberately code paths rather than relative hyperlinks because this chapter is mirrored unchanged into other backend worktrees.

| Path | Responsibility and useful entry points |
| --- | --- |
| `CMakeLists.txt` | C++20 targets, dependency/version checks, KWin plugin installation, optional real-GL tests |
| `default.nix`, `shell.nix` | Package and development environment; the private shader-library derivation is **inline in `default.nix`** |
| `src/plugin.cpp`, `src/metadata.json` | KWin factory, support predicate, effect ID `waylandshader`, disabled-by-default discovery metadata |
| `src/effect.h`, `src/effect.cpp` | `DesktopEffect`, output state, scene capture/final draw, asynchronous publication, KConfig, D-Bus methods |
| `src/shader_runtime.h`, `src/shader_runtime.cpp` | `ShaderRuntime`, C API ownership, shared EGL context, one filter chain per selected output, GPU synchronization |
| `src/output_settings.h` | Shader/color toggles and gamma/saturation bounds |
| `src/controller_client.h`, `src/controller_client.cpp` | Asynchronous D-Bus requests, JSON status validation, controlled effect loading |
| `src/controller_main.cpp` | Qt Widgets application, parameter editors, monitor selection, refresh/edit safeguards |
| `src/ctl_main.cpp` | CLI grammar, load-completion waiting, timeout and exit-code contract |
| `data/org.waylandshader.Controller.desktop` | Application-menu entry |
| `tests/shader_runtime_test.cpp`, `tests/fixtures/` | Actual GL pixel assertions and the multipass temporal/LUT/mipmap fixture |
| `patches/librashader-gl-lifetime.patch` | Rust GL ownership, partial-construction cleanup, temporal initialization/reset and framebuffer fixes |
| `WaylandShader-Guide.txt`, `README.md` | Earlier native verification and user guidance; use the qualifications below for stale statements |
| `proposed-perfomance-enhancer.md`, `performance-enhancer-evaluation.md` | Unapplied idle-scheduling proposal and its counterexample-based evaluation |

### Compatibility envelope

**[Code]** CMake requires CMake 3.25+, ECM 6.22+, Qt 6.10+ with Core, Gui, Widgets, DBus, Concurrent and **CorePrivate**, KF6 CoreAddons/Config, libepoxy, and KWin 6.6. It explicitly rejects KWin **6.7 or newer**. The Nix derivation asserts librashader **exactly 0.10.1** before applying the patch.

The effect's support predicate requires OpenGL compositing, a current EGL display/context, desktop GL, and GL 3.3+. Loading additionally requires texture storage: OpenGL 4.2 or `GL_ARB_texture_storage`. OpenGL ES is explicitly rejected. A successful package build does not establish that the running compositor meets these runtime requirements.

KWin's effect/rendering API and the Qt private headers are not a stable cross-release binary interface. Matching only the string “KWin 6.6” is not a substitute for using the compositor's actual package set, including relevant downstream patches.

## 2. Architecture: pixels, controls, and ownership

### 2.1 Native bootstrap

**[Code]** `KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED` in `src/plugin.cpp` creates `DesktopEffect` only through KWin's effect infrastructure and delegates support checking to `DesktopEffect::supported()`. The factory's default-enabled result is false; metadata also says `EnabledByDefault: false`. CMake installs the plugin into the KDE installation namespace `kwin/effects/plugins` and teaches AUTOMOC about the factory macro.

Once instantiated, the effect:

1. Registers `/WaylandShader` on the session bus with interface `org.waylandshader.Effect` under KWin's existing `org.kde.KWin` service.
2. Connects output-add/remove, screen-locking, and asynchronous-load completion signals.
3. Discovers non-placeholder outputs and reads their profiles.
4. Reads `kwinrc`'s `Plugins/waylandshaderEnabled` and the saved preset from `waylandshaderrc`.
5. Schedules restoration of a saved preset on the event loop rather than doing the load in construction.

The requested effect-chain position is 1, intended to wrap the other desktop effects and filter the composed output rather than each window individually. It creates no replacement application surface and does not intercept application input.

### 2.2 Pixel path and color management

```text
KWin's output scene, including downstream desktop effects
    -> full-output GL_RGBA16F source texture described as SDR sRGB
    -> optional output-specific librashader chain
    -> optional monitor gamma/saturation fused into final draw
    -> KWin destination color conversion and output render target
```

**[Code]** `prePaintScreen()` expands paint to the full output when shader or non-neutral color processing is active. It marks the screen transformed/background-first and removes region-only painting. This is important even for a static shader: a blur, warp, or scaler can propagate local scene damage to distant pixels.

`paintScreen()` sizes allocations using `viewport.scaledRenderRect().size()`, not an assumption that logical monitor dimensions equal physical pixels. It creates a source framebuffer and delegates ordinary scene rendering into a `RenderTarget` described as `ColorDescription::sRGB`. Shader inputs receive complete mip storage; color-only inputs receive a single level. Source and shader destination are distinct `GL_RGBA16F` textures.

For a selected shader output, the runtime receives texture handles, format, size, output frame count, refresh rate converted from mHz to Hz, elapsed presentation milliseconds, and a history-reset flag. It does not read pixels back to the CPU. The final quad uses librashader's default MVP mapping from `[0,1]`, rather than an identity matrix or an extra Y flip; the guide records upright native rendering.

The final KWin draw maps either the shader result or, for color-only operation, the source texture. Gamma applies to sRGB-encoded color, respecting premultiplied alpha; then saturation uses linear-sRGB Rec.709 luminance coefficients. KWin's colorimetry transform, tonemapping, and destination encoding remain in the final path, with `RelativeColorimetricWithBPC` rendering intent. Gamma `1.0` and saturation `1.0` are neutral. Non-neutral color work is fused into this presentation draw rather than introducing a separate full-output intermediate.

**Boundary:** floating-point textures and color-managed presentation do **not** make this an HDR-preserving shader pipeline. The shader's input contract is display-referred SDR sRGB. Wide-gamut/HDR preservation, physical rotation, cursor-plane inclusion and physical-display correctness require separate evidence. The software controls are neither ICC calibration nor hardware gamma-ramp management.

### 2.3 C++ / Rust / C ABI boundary

**[Code]** C++20 is the native integration language for KWin and Qt. Rust librashader supplies the actual Slang preset machinery: preset parsing, shader compilation, multipass execution, lookup textures, mipmaps, feedback and history. C++ calls its C API through `librashader.h`, rather than trying to reproduce that runtime in a custom GLSL wrapper. GLSL supplies GPU shader programs; Nix supplies a reproducible dependency/package boundary. See the [handbook](../PROJECT-HANDBOOK.md) for the shared language rationale and general patch workflow.

`ShaderRuntime` wraps preset handles, parameter-list ownership and C API error objects with C++ lifetime management. It converts and frees error messages, releases preset/list objects on partial failure, and frees every chain while its private GL context is current. Runtime options set ABI structure versions, original-aspect and frametime uniforms, the supported GLSL version, DSA when GL is at least 4.5, and leave mipmaps and the shader cache enabled.

The Nix package links the effect against **`libwaylandshader-rashader.so.2`**, not an arbitrary system `librashader.so`. `default.nix` patches the dependency, renames its installed library/symlink, and sets the private SONAME. CMake searches for `waylandshader-rashader`. This keeps the corrected dependency separate from other librashader users inside the compositor process; merely making an unpatched library available under the requested name would not satisfy the ownership contract.

### 2.4 EGL isolation and lifetime

**[Code]** One loaded `ShaderRuntime` owns a private EGL context shared with the original KWin context; its per-output chains use that runtime context. Compilation runs on a worker while rendering later enters the same private context from the compositor thread. Publication occurs only after worker completion, so these are not concurrent users of one current context.

Context construction tries desktop core GL 4.6, 4.5, then 3.3, with ordinary and robust-reset variants to find a share-compatible reset-notification policy. It prefers surfaceless rendering because KWin's GBM display may have no pbuffer configuration, retaining a 1×1 pbuffer fallback for older EGL displays. These are implementation fallbacks, not a claim that every EGL driver has been exercised.

`CurrentContext` saves/restores the caller's EGL API, display, context, and read/draw surfaces, including a worker that began with no current context. It never terminates KWin's EGL display. On each frame:

1. The host emits a fence and flushes before entering the private context.
2. The private context GPU-waits on that fence and invokes the chain.
3. It emits and flushes an outgoing fence **even if the chain reports failure**, because failed work may still have queued texture access.
4. The original context is restored and GPU-waits before KWin reuses the textures.

These are GPU ordering operations, not CPU readback or an intended per-frame CPU wait. The separate context isolates librashader's bindings/state from KWin. The final draw that does run directly in KWin separately preserves blend/scissor enablement, active texture, texture/sampler binding and the shader-manager stack.

Shared resources need explicit lifetime management: destroying a context does not automatically make every shared allocation disappear while another sharing context survives. The Rust patch adds owning `GlObjects` collections for buffers, vertex arrays, LUT textures, samplers, shaders and programs, including partially initialized constructors. Framebuffer error paths are corrected too. The creating GL context must be current when owners drop.

On output removal, the effect retains output state still referenced by the current chain until replacement frees that runtime. On resize, it retains the previous destination until the next successful frame has moved the chain away from it. Successful runtime replacement releases retired output/destination state after the old runtime is gone. During destruction, the effect waits for the loading worker before the shared context group can disappear, then destroys runtimes before their host textures. Context/display loss is a hard boundary: if the private context cannot be entered, cleanup logs a critical error rather than running its GL destructors in an unsafe host context. Do not describe this as guaranteed leak-free recovery from driver loss.

### 2.5 Per-output state, history, and asynchronous replacement

**[Code]** Persistent monitor identity uses KWin's output UUID, with `connector:<name>` as fallback. Runtime chain lookup instead uses live `LogicalOutput*` values converted to IDs. These are different identity domains: a persistent settings key is not a lifetime guarantee for an output object.

Each selected output gets an independent chain. The effect also tracks its own source/destination, presentation timestamps, frame counter and reset flag. Preset path and parameter values are shared, but histories and frame progression are not. Resizing, master enablement changes, shader selection, reconfiguration, lock changes and successful replacement invalidate the appropriate temporal state. Processing is bypassed while the screen is locked; the next processed frame requests cleared history.

Loading follows this control path:

```text
GUI/CLI -> D-Bus loadPreset -> absolute/readable .slangp validation
    -> snapshot selected output IDs and saved parameter overrides
    -> QtConcurrent worker + private EGL context + per-output compilation
    -> QFutureWatcher completion on effect thread
    -> install successful runtime OR retain previous working runtime
    -> repaint + statusChanged + topology reconciliation when needed
```

Only one preset load runs at a time: another request receives `Busy`, not an unbounded queue or implicit cancellation. Live parameter mutations are also rejected while loading. Existing output chains continue to render while replacements compile. When no shader outputs are selected, the loader still compiles one temporary chain to validate the preset and obtain parameters, then frees it; a successful load is not a fake no-op. Selecting outputs later triggers real chain construction.

**Important stale-load qualification:** KDE does **not** implement a generation-token “discard every stale completion” protocol. Output changes during loading set `m_reloadOutputs`; a successful completion is installed with the output-ID list it actually compiled, then `finishLoad()` queues a reload if that list is outdated or a change was recorded. `usesShader()` filters eligibility by the loaded-ID list. Removed output state is retained, and the deferred topology check also considers retired outputs to account for pointer-address reuse. This is reconciliation after completion, not cancellation. The source does not establish a stronger generation-safe guarantee for every possible remove/re-add interleaving; preserve the distinction when comparing GNOME's lifecycle or designing a refinement.

Parameters are validated against metadata after converting to the runtime's float domain, including decimal boundaries such as `0.1`. Repeated parameter metadata follows the runtime's last-pass-wins map. Overrides are checked for unknown names and finite/in-range values. Updates preflight every chain before setting values because librashader 0.10.1's setter can insert an unknown name before returning an error; an update failure attempts rollback on chains already changed. When building multiple chains, differing resolved parameter values cause rejection. This is not a complete immutable snapshot of every shader file: the loader reopens presets for separate outputs.

### 2.6 D-Bus, GUI, CLI, and persistence

**[Code]** Both frontends share `ControllerClient`. It subscribes to `statusChanged`, uses asynchronous calls, validates the JSON schema and numeric bounds, and notices KWin service-owner loss. Per-call D-Bus transport timeout is 10 seconds; CLI `--timeout` separately controls the overall operation, including compilation.

Read-only `status`, `parameters`, and `outputs` never load a missing effect. The GUI and mutation commands may request **only** `waylandshader` through KWin's `/Effects`, `org.kde.kwin.Effects.loadEffect`. They do not restart KWin. A successful `loadPreset` D-Bus boolean means accepted for compilation, not finished. The CLI waits for terminal status, querying again after acceptance so an earlier status signal is not mistaken for completion.

Status contains `enabled`, `active`, `loading`, `preset`, `requestedPreset`, `error`, `parameters`, and `outputs`. Each output reports `id`, `name`, `shaderEnabled`, `colorEnabled`, `gamma`, `saturation`, `shaderActive`, and `colorActive`. A selected shader can be inactive while its chain is unavailable; an enabled color toggle can be inactive when values are neutral. `active: false` alone is therefore not an error diagnosis.

The Qt window has a master switch, preset picker, shared parameter editors, monitor selector, independent shader/color switches and numeric color controls. Refresh avoids overwriting focused edits. Rebinding a monitor discards pending spin-box text with signals blocked so hotplug cannot commit it to another monitor. Parameter edits are debounced and guarded against stale editor callbacks. These are correctness measures for asynchronous state updates, not just UI polish.

Under `$XDG_CONFIG_HOME` (normally `~/.config`):

- `waylandshaderrc`: `General/Preset`; per-canonical-preset parameter groups keyed by SHA-256; per-output groups keyed by SHA-256 of the stable output ID.
- `kwinrc`: standard `Plugins/waylandshaderEnabled`, deliberately shared with Plasma rather than duplicated as another persistent master flag.

Invalid saved color values disable that output's color adjustment and restore neutral values with an error. Invalid saved parameter overrides reject loading rather than silently changing the intended preset. Settings remain editable while rendering is bypassed.

## 3. Piece-by-piece build reconstruction

The following stages are **[Reconstruction]**. Each names the part to construct and the evidence that would distinguish it from a plausible but incomplete implementation.

1. **Choose the insertion point before building a viewer.** Establish a KWin-native whole-output effect. A capture-overlay design would need capture permissions, DMA-BUF negotiation, input/focus policy and feedback-loop prevention, yet still would not universally replace the compositor's displayed scene. The finished code instead participates in KWin's paint chain. The output of this stage is a discoverable, supported-only effect, not a separate fullscreen window.
2. **Build the plugin and dependency boundary.** Add C++20/Qt/KF/KWin targets, metadata, AUTOMOC support, and the inline Nix derivation for the private patched shader library. The artifact is a KWin plugin plus clients tied to a known dependency set. Discovery after installation/login is distinct from compilation; neither `nix-build` nor launching the GUI changes KWin's ABI.
3. **Make a trustworthy offscreen output path.** Capture the downstream scene at viewport-scaled dimensions into an SDR sRGB intermediate; present it using KWin's destination colorspace. Preserve full-output coverage and orientation. The guide's identity/inversion RGB checks and upright CRT presentation are stronger evidence than seeing “effect loaded” on D-Bus.
4. **Integrate complete presets, not one GLSL pass.** Wrap librashader through its C ABI, validate metadata/parameters, compile real chains, and pass frame semantics. The retained fixture uses two passes, a half-scale floating-point intermediate, mipmap inputs and a generated LUT. Its expected pixels exercise actual runtime behavior.
5. **Make GL ownership and temporal state safe.** Isolate contexts and synchronize shared textures, free partial/successful allocations, initialize all history slots and mip levels, reset feedback as well as history, and retain host textures until chains no longer reference them. These requirements cross the C++ wrapper and Rust dependency; a wrapper-only cleanup would miss allocations owned inside librashader.
6. **Separate outputs and handle topology.** Add independent chains/counters/history, UUID-backed profiles, resize-sensitive allocations, retired output state, and reconciliation when the selected output set changes. The guide records resize, fractional scale and disable/re-enable checks; it does not prove physical rotation or every hotplug/load race.
7. **Move compilation away from frame rendering.** Retain the old runtime during a worker load, publish only complete successful runtimes, surface `Busy`/failure status, and reconcile topology after completion. Validate even with zero selected outputs. The resulting contract is usable while compiling and transactional for ordinary failed preset replacement, without pretending compilation cannot contend for GPU resources.
8. **Expose asynchronous controls and persist intent.** Share one client between the Qt window and CLI, distinguish acceptance from completion, validate server replies and parameter bounds, and maintain output identity during UI refresh. Test actual mouse/keyboard interaction and visible pixel changes rather than only method replies. The guide records these native checks.
9. **Add independent software color controls.** Keep shader and color enablement separate, fuse adjustments into final presentation, expose neutral versus active status, and persist profiles. A valid color-only startup without a preset establishes that this path is independent of chain compilation.
10. **Evaluate optimization only after correctness.** Recognize continuous Slang repaint as present behavior, then test any proposed static classification against actual rendered animation. The later performance evaluation rejects the proposed scanner's safety; it does not ship an optimization.

### What git history actually supports

| Commit on KDE `main` | Supported conclusion |
| --- | --- |
| `7a5c019` — “Nearly fully featured Wayland shader in KDE” | Already introduces the plugin, runtime, clients, Nix/CMake setup, dependency patch, fixtures, guide and roadmap together. It cannot identify a separate commit for each reconstruction stage. |
| `ff42c5f` — “Added a readme” | Adds the README and its recorded verification summary. |
| `4d67833` — “Initial commit”; `6579b80` — license merge | Introduce/merge repository licensing, not another rendering milestone. The old README's “no LICENSE” wording is stale. |
| `c2fec31` — “Optimization proposal” | Adds `proposed-perfomance-enhancer.md`, not application changes. |
| `514b06c` — “Added an evaluation” | Adds `performance-enhancer-evaluation.md`, including recorded rendering counterexamples. No idle-scheduling implementation is added. |

The separate local GNOME commit `e1bcfc0` is not a KDE rendering stage. See [GNOME](GNOME.md) for that branch. Patch-file timestamps are not proof of an exact development sequence or the date a failure was first discovered.

## 4. Numbered build, install, and use guide

These are instructions for a reader to execute deliberately. **They were not run for this documentation task.** Paths in commands are examples and must be replaced with the reader's actual checkout/package-set paths.

### 1. Establish the backend and matching package set

Use the KDE `main` source and a KWin 6.6 Wayland session with desktop GL/EGL. Check the installed compositor's version and use the same NixOS `pkgs` value that builds it. Include the same overlays/configuration where applicable; do not silently build against a newer unrelated channel to satisfy a missing dependency.

The short commands `nix-build` and `nix-shell` default to `<nixpkgs>`. They are appropriate only when that is the matching package set. For an explicitly available matching nixpkgs source, a standalone example is:

```bash
nix-build default.nix --out-link ../waylandshader-kde-result \
  --arg pkgs 'import /absolute/path/to/matching-nixpkgs { }'
nix-shell shell.nix --arg pkgs 'import /absolute/path/to/matching-nixpkgs { }'
```

The empty import arguments above are sufficient only if the system does not need additional overlays/configuration. For an existing NixOS system, importing the package with that configuration's already constructed `pkgs` is the safer contract. A KWin 6.7 rejection or librashader-version assertion is a compatibility stop, not a check to delete.

### 2. Build the package

From the KDE checkout, with matching `<nixpkgs>`:

```bash
nix-build --out-link ../waylandshader-kde-result
```

Or use the explicit `--arg pkgs` variant from step 1. Success produces the external `../waylandshader-kde-result` symlink with the native plugin, `waylandshader-controller`, `waylandshaderctl`, application entry and installed `WaylandShader-Guide.txt`. Without `--out-link`, plain `nix-build` instead creates `result` inside the checkout. CMake also builds internal static libraries for the runtime and common client. A build alone does **not** install/discover/activate the plugin in the current desktop.

There is no standalone distribution packaging recipe here beyond Nix. A non-Nix packager must supply the same guarded KWin/Qt development interfaces and genuinely patched private library, not substitute stock librashader.

### 3. Optional developer build and actual runtime test switches

Enter the matching shell as above, then:

```bash
cmake -S . -B ../waylandshader-kde-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/../waylandshader-kde-install" \
  -DWAYLANDSHADER_BUILD_TESTS=ON
cmake --build ../waylandshader-kde-build --parallel
ctest --test-dir ../waylandshader-kde-build --output-on-failure
```

`WAYLANDSHADER_BUILD_TESTS` defaults **OFF**; enabling it is the actual switch in this branch. The project's CMake file registers **two** tests: `shader_runtime` and `shader_runtime_gl33`. The latter sets `MESA_GL_VERSION_OVERRIDE=3.3`. Both invoke the same real rendering program, which exercises ordinary and robust EGL hosts internally.

**No-renderer caveat:** when the required Mesa surfaceless platform extensions are absent, the program returns **77** and CTest reports **Skipped**. A skip is not a rendering pass. EGL initialization, context/configuration creation or shader failures after that extension gate fail the program; they are not all converted into skips. A green CTest summary must be read together with the number of executed versus skipped checks. These runtime tests do not load the KWin plugin or exercise the GUI.

A third CTest entry, `appstreamtest`, comes from ECM: `include(KDECMakeSettings)` enables `BUILD_TESTING` by default and registers it whenever `appstreamcli` is on `PATH`, which it is in `shell.nix` (via `appstream-qt`, confirmed with `nix-shell shell.nix --run 'command -v appstreamcli'` on 2026-09-11). It reads `install_manifest.txt` and validates any installed `.metainfo.xml`/`.appdata.xml`; this project installs none, so it prints “Not installed yet, skipping” or validates nothing and passes. That is the README's “3/3”: two real-GL checks plus one trivially passing metadata check. Do not count it as rendering evidence.

For a developer staging tree, `cmake --install ../waylandshader-kde-build` populates the configured external `../waylandshader-kde-install` prefix. That is still a staging installation, not a promise that an already-running NixOS KWin searches it. Prefer normal package installation for desktop discovery.

### 4. Install with the system's `pkgs`

For a non-flake NixOS configuration, add this to the appropriate module using its existing `pkgs`:

```nix
environment.systemPackages = [
  (import /absolute/path/to/WaylandShader/default.nix { inherit pkgs; })
];
```

For a flake-managed system, declare the checkout as a non-flake input:

```nix
inputs.waylandshader = {
  url = "path:/absolute/path/to/WaylandShader";
  flake = false;
};
```

Pass `waylandshader` from the flake's outputs arguments to the NixOS module, for example via `specialArgs = { inherit waylandshader; };`, then use:

```nix
{ pkgs, waylandshader, ... }:
{
  environment.systemPackages = [
    (import "${waylandshader}/default.nix" { inherit pkgs; })
  ];
}
```

Local `path:` flake inputs are content-addressed: documentation edits and build artifacts inside a locked checkout can change its NAR hash even without an application-code change. The examples above keep build directories, staging prefixes and out-links outside the checkout where practical. Before a later manual rebuild, refresh **only the changed named input** using the name in your system flake. For the example name `waylandshader` and a system flake at `/etc/nixos`:

```bash
sudo nix flake update waylandshader --flake /etc/nixos
```

**Manual host-changing step:** run your normal NixOS rebuild/switch procedure for that configuration. This updates the system, not just this repository. **Manual session-ending step:** save all work, log out of Plasma, and log back in so KWin's plugin search paths include the installation. Never replace or restart the live Wayland compositor to test a plugin. Rebuilding a file on disk also does not replace a library already mapped into KWin.

### 5. Discover and activate deliberately

```bash
waylandshaderctl status
waylandshader-controller
```

A missing-effect error from the first command can be expected before loading: `status` is intentionally read-only. The settings application may request the installed effect and then query its interface. Success is a connected settings window and valid status, followed by visible processing after configuration. A failure to load points first to matching ABI, installation/search paths, a fresh login, support requirements, and the reported D-Bus error—not to a need to restart KWin.

Presets are not bundled. Obtain a `.slangp` with all of its referenced shaders, includes and lookup textures, preserving relative layout and its license. Choose a simple non-warping preset first; unfamiliar/expensive presets and integration changes belong in an isolated nested test session before daily use.

### 6. Load a preset and configure monitors

```bash
waylandshaderctl --timeout 120 load /absolute/path/to/preset.slangp
waylandshaderctl enable
waylandshaderctl status
waylandshaderctl outputs
waylandshaderctl parameters
```

`load` waits for compilation and preserves master enablement; `enable` is a separate operation. Use actual output IDs or unique exact names from `outputs`, and actual parameter names/bounds from `parameters`:

```bash
waylandshaderctl output DP-1 shader off
waylandshaderctl output DP-1 color on
waylandshaderctl output DP-1 gamma 1.1
waylandshaderctl output DP-1 saturation 0.8
waylandshaderctl param PARAMETER_NAME 0.5
```

Replace `DP-1` and `PARAMETER_NAME`; these are not universal identifiers. Gamma ranges from `0.1` to `5.0` (higher brightens), saturation from `0.0` to `2.0` (`0.0` is grayscale). Monitor commands never turn on the master switch. For color-only use, set non-neutral color values, turn on the output's color toggle and enable the master; no preset is needed.

Observe actual output pixels as well as status: selected versus active flags describe different states. Preset parameters apply across all selected shader outputs; different presets per monitor are not implemented. A CRT preset's own desktop-resolution cutoff may suppress scanlines, so inspect its declared parameters before concluding that loading failed.

### 7. Bypass, re-enable, and handle errors

```bash
waylandshaderctl disable
waylandshaderctl status
waylandshaderctl enable
```

Master disable bypasses both shader and color paths without discarding the preset/profiles; it is not an unload command. Re-enable resets temporal state. Per-output shader deselection can leave color processing active. Screen lock also bypasses processing and invalidates temporal history.

An ordinary rejected replacement retains the old working runtime. Inspect `error`, `preset`, `requestedPreset`, and `loading`; do not equate an accepted asynchronous request with a successful replacement. A **rendering** failure instead latches a global processing bypass and repaints the ordinary scene. In that state, `enable` is rejected until a working preset reload clears the render-failure latch. For color-only allocation/shader failures, the inspected API likewise has no separate “clear error” method; do not promise that toggling alone recovers it.

The CLI returns: `0` success; `1` rejected operation/shader failure; `2` usage error; `3` unavailable effect, transport or protocol error; `4` timeout. `--timeout` accepts `1`–`3600` seconds and defaults to `120`. Timeout **does not cancel** the operation inside KWin. Query status before retrying; another load while compiling is `Busy`.

If a native fault prevents controls from responding, this bypass API is not a crash-containment boundary. Follow normal desktop/session recovery procedures manually; do not use a live compositor replacement as an update mechanism.

## 5. Successes, failures, and lessons

| Outcome | Evidence | What it establishes / lesson |
| --- | --- | --- |
| Multipass, LUT, mipmaps, independent history, feedback reset and live parameters | **[Code]** retained fixture/assertions; **[Recorded]** prior real-GL regression success | Pixel values are checked across frame sequences, two chain IDs, two load cycles and parameter edits, including `0.1`. This is stronger than compilation-only validation; it is not physical-output coverage. |
| Caller EGL context and viewport restoration | **[Code]** explicit assertions on every test render; ordinary/robust hosts and GL 3.3 CTest variant | Shared-context isolation must preserve the host, not just render the right texture. The retained assertion checks context and viewport, not every possible KWin state. |
| Native two-output desktop filtering | **[Recorded]** guide: isolated nested KWin 6.6.6, Mesa, AMD Radeon RX 6800 XT | Real surfaces, upright `crt/crt-easymode.slangp`, identity RGB and inversion after parameter edits were inspected. Isolated D-Bus/configuration separated these checks from the daily session. |
| Input and GUI edits remained native | **[Recorded]** guide: KWin EIS text entry/button clicks, actual preset/parameter and monitor controls | No capture viewer had to proxy application input. Pixel warps still do not remap hit-testing coordinates. |
| Broken replacement rejected without losing working pixels | **[Recorded]** guide: deliberately broken preset followed by still-filtered RGB samples; **[Code]** retain-on-load-failure branch | Keep load failure separate from rendering failure, and validate the previous image rather than just an error string. |
| Resize, fractional scale, output disable/re-enable and restoration | **[Recorded]** guide: two-output native checks, parameter/profile persistence and color-only startup | Positive evidence for tested topology transitions, not physical rotation, arbitrary races or DPMS/resume correctness. |
| No selected shader outputs still validates presets | **[Recorded]** guide; **[Code]** temporary validation chain | Deselecting all outputs must not make invalid presets appear successful; later reselection must construct usable chains. |
| Focused monitor edits survive refresh without moving to another monitor | **[Recorded]** guide: removal/reconnection checks; **[Code]** signal-blocked rebinding | Async status and hotplug are data-integrity concerns in the GUI. |
| Owned GL objects and partial failures need explicit cleanup | **[Code]** Rust patch; **[Recorded]** guide's separate repeated load/free GPU probes | Context destruction alone is insufficient for shared objects. Probe reports are historical; the standalone probe harness is not retained as a current CTest entry. |
| History/feedback could expose undefined or stale pixels without patch fixes | **[Code]** history initialization, feedback reset, all-mip clearing and owned-attachment restoration in patch | Temporal reset must cover every referenced history slot/mip and must not clear an attached caller texture. Do not invent a dated user-visible incident from the patch alone. |
| Silent framebuffer format fallback is removed | **[Code]** GL3 and DSA patch paths now return framebuffer-init errors instead of retrying as RGBA8 | Preserve requested format semantics and report failure; a silently changed intermediate format is not correct compatibility. |
| Invalid parameters and malformed replies are rejected | **[Code]** validation, setter preflight/rollback and client schema checks; retained parameter rejection assertions | Validate at the runtime boundary as well as in the GUI. An upstream setter's mutation-on-error requires care beyond trusting its return code. |
| Proposed lexical temporal scanner gives false-static results | **[Recorded]** performance evaluation's rendered fixtures | Optional/unquoted includes, token-pasting and file replacement after scanning all produced changing frames despite “static” classification. A missed repaint could freeze a valid shader. |
| Physical HDR, rotation, power, latency and VRR conclusions remain absent | **[Unverified]** guide/roadmap; performance evaluation narrows measurement claims | Nested output pixels and surfaceless GL cannot substitute for physical display/driver measurements. Supplied power reports discussed by the evaluation are not a new validated benchmark here. |

The recorded guide describes native verification but does not retain a complete automated nested-session launcher and every probe in this checkout. Reproduce that level of evidence using an isolated compositor bus/configuration and actual visible surfaces; do not claim that running the retained `shader_runtime_test` reproduces those desktop checks.

## 6. Future refinements: correct scheduling before performance claims

### Damage-driven static presets are not implemented

**[Code]** `DesktopEffect::postPaintScreen()` schedules repaint for **every loaded selected Slang chain** while processing is enabled. There is no compiled temporal/static classifier or implemented `ForceContinuousRepaint` option in this branch. The comment about temporal chains does not change that predicate. Color-only processing already follows ordinary damage and does not self-schedule animation.

**[Recorded]** the performance evaluation tested the proposal's exact scanner against real rendering. Direct `FrameCount` was detected, but animation behind `#pragma include_optional`, unquoted `#include`, token-pasting macros and source replacement after scanning was falsely classified static. Each compiled and changed from red `0` to `1` on successive identical-input frames. These are rendering counterexamples, not speculative regex objections. A manual override would help diagnostics but would not make incorrect automatic classification safe.

**[Recommendation]** preserve the evaluation's implementation order:

1. Expose requirements derived from the **successfully compiled chain's active reflection**, including relevant frame/time, history and feedback semantics. The current stable C API does not expose the needed query; the private dependency patch is a possible place to add it.
2. Aggregate/store requirements during loading, using the same compiled input rather than rescanning files. Unknown must remain continuous. Preserve metadata with the old runtime on failed replacement, and reconcile it with output/runtime lifecycle.
3. Gate self-scheduling in `postPaintScreen()` without changing `usesShader()`, full-output paint expansion, ordinary invalidation or scanout protection. Static processing must still run when content/settings change.
4. Integrate any override and effective scheduling status through KConfig and the existing controls; a proposal-only setting is not a current user instruction.
5. Exercise static/temporal transitions, failed replacement in both directions, settings changes without incidental client damage, small nonlocal damage, mixed refresh/output activity, zero-output selection, hotplug during compilation, lock/unlock and resume. Measure physical savings separately.

This would remove eligible idle **self-scheduled frames**, not all effect overhead. Shader work still runs on real damage, textures/chains remain resident and `blocksDirectScanout()` currently returns the effect's global `isActive()` result. Do not promise per-output-only scanout effects or independent-output wakeup savings without measuring KWin's actual routing.

Keep mipmap optimization separate: a full ordinary 2D mip chain is approximately 4/3 of its base level, so dropping lower levels saves about 25% of that texture's full allocation—not one-third of all per-output VRAM. Source, destination, pass intermediates, LUTs, feedback and history are separate costs. The current runtime intentionally leaves `force_no_mipmaps` false; effective compiled pass requirements must justify changes.

### Other refinements

- **[Recommendation]** make output-generation/stale-completion behavior explicit before claiming immunity to every hotplug/address-reuse race; KDE currently reconciles rather than discarding all stale results.
- **[Recommendation]** establish physical installation, transform/rotation, HDR/wide-gamut behavior, lock/DPMS/resume, cursor policy, scanout, VRR, latency and power evidence on representative hardware. Document unsupported behavior as such rather than upgrading SDR presentation to an HDR claim.
- **[Recommendation]** retain useful observable reproductions for context/resource faults and topology/load races, not tests of incidental implementation text. Keep the real-GL boundary and desktop/hardware boundary separate.
- **[Recommendation]** port to a new KWin API deliberately when needed, and evaluate upstreaming the dependency fixes so the private maintenance burden can shrink. No KWin 6.7 port is present.

## 7. Maintenance and patch / ABI rules

The [handbook](../PROJECT-HANDBOOK.md) owns the general contribution and patch workflow. KDE-specific rules are:

1. **Treat KWin/Qt upgrades as ports.** Keep the CMake upper guard until paint signatures, viewport/output types, color-management helpers, effect factory/discovery and private headers have been reviewed and exercised against the new target. Rebuild against the installed compositor's matching `pkgs`; never load a guessed-compatible native binary into a daily session.
2. **Keep the private dependency real and versioned.** KDE's derivation lives inline in `default.nix`; there is no KDE `shader-library.nix` to edit. A librashader bump requires porting `patches/librashader-gl-lifetime.patch`, checking C API/options compatibility and retaining the distinct library name/SONAME. Do not remove the `0.10.1` assertion merely to make evaluation proceed.
3. **Audit both GL paths.** The patch changes GL3 and GL46/DSA program, buffer, quad, LUT and framebuffer implementations. Preserve RAII ownership on partial failure, shader compile/link cleanup, framebuffer cleanup, owned attachment restoration, mip clearing, first-use history initialization and feedback resets. Passing only a modern DSA path is insufficient evidence for the fallback.
4. **Respect context-before-resource destruction.** Free chains in their private context before destroying host textures/share groups. Keep worker completion ordering on teardown and GPU ordering even after failed frames. Never call `eglTerminate` on the compositor display. Context loss must remain a reported boundary, not an unsafe “cleanup fallback.”
5. **Preserve user-visible transactions.** A rejected load must leave the old preset/parameters/pixels usable; zero selected outputs must still validate; CLI timeouts must not imply cancellation; parameter failures must not silently split settings across outputs. Any new load metadata belongs to the installed runtime, not the requested filename alone.
6. **Preserve color and privacy contracts.** Keep full-output coverage for spatial effects, destination color conversion and locked-state bypass/history reset. Do not optimize only the repaint loop while allowing stale unlocked content, incomplete output damage or a scanout path to bypass processing.
7. **Maintain all control surfaces together.** A changed status field or method must be reflected in `DesktopEffect`, `ControllerClient`, CLI, Qt controls and user guidance. Preserve stable output IDs, numeric validation, focus-safe editor refresh and the distinction between configured and active flags.
8. **Collect evidence at the right layer.** For a future patch, use optional GL checks for chain/context behavior, isolated KWin checks for actual compositor output/input/lifecycle, and physical hardware for display/power/latency claims. Record skips and exact versions; a “3/3” CTest summary includes ECM's trivially passing `appstreamtest`. For this documentation work, no such checks were rerun.

When reporting a failure, include the source revision, KWin/Qt/nixpkgs and private librashader versions, GPU/Mesa or proprietary-driver version, native versus nested session, preset and referenced assets/settings, connected-output layout/scale, exact steps, and `waylandshaderctl status` output if the service is available. Separate build/discovery failure, compilation rejection, render-failure bypass, transport timeout and native crash: their remedies and evidentiary strength differ.

## 8. Evidence and further reading

### Local evidence

- KDE `main` `src/effect.cpp`: `supported`, `prePaintScreen`, `paintScreen`, `postPaintScreen`, `loadPreset`, `finishLoad`, `outputsChanged`, `resetHistory`, persistence and status.
- KDE `main` `src/shader_runtime.cpp`: `CurrentContext`, `Impl::createContext`/destruction, `ShaderRuntime::load`, `setParameter`, `render`.
- KDE `main` `default.nix`, `CMakeLists.txt`, `shell.nix`: exact dependency guards, inline patched-library packaging, plugin installation and optional test switches.
- KDE `main` `patches/librashader-gl-lifetime.patch`: ownership and temporal fixes in both backend paths; source evidence of responsibilities, not a standalone chronological bug diary.
- KDE `main` `tests/shader_runtime_test.cpp` and `tests/fixtures/`: retained observable runtime assertions and exact skip behavior.
- KDE `main` `WaylandShader-Guide.txt`, “Verified native behavior”: earlier isolated two-output native checks and their limits. The README's test count and old cross-backend/license statements require the qualifications in this chapter.
- KDE `main` `performance-enhancer-evaluation.md`: earlier six-fixture rendering experiment, immutable-input/reflection recommendation and explicit “optimization has not been applied” status. The companion proposal filename is intentionally spelled `proposed-perfomance-enhancer.md` in the repository.
- Commit subjects and file boundaries: `7a5c019`, `ff42c5f`, `4d67833`, `6579b80`, `c2fec31`, `514b06c`. These support the coarse chronology above, not invented intermediate commits.

### Primary upstream references

- [KWin 6.6.6 source](https://github.com/KDE/kwin/tree/v6.6.6) — the versioned compositor API/reference used by the historical native record.
- [librashader](https://github.com/SnowflakePowered/librashader) and [0.10.1 source](https://github.com/SnowflakePowered/librashader/tree/librashader-v0.10.1) — runtime and dependency-patch baseline.
- [librashader compatibility notes](https://github.com/SnowflakePowered/librashader/blob/master/BROKEN_SHADERS.md) — upstream compatibility is not universal preset support.
- [libretro Slang presets](https://github.com/libretro/slang-shaders) — preserve each preset's dependencies and license.
- [Portal ScreenCast contract](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html) and [PipeWire DMA-BUF sharing](https://docs.pipewire.org/devel/page_dma_buf.html) — why a portable capture viewer is a different design from this in-place KWin effect.
- [Wayland input-region semantics](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface-request-set_input_region) — input regions are not a general replacement for compositor-native input/focus integration.

Continue with the [shared handbook](../PROJECT-HANDBOOK.md), [GNOME](GNOME.md), and [niri](NIRI.md) for cross-backend language choices, patch workflow and the separate development stories.
