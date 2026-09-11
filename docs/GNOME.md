# GNOME: native Shell/Mutter backend

[Project handbook](../PROJECT-HANDBOOK.md) · [KDE](KDE.md) · [niri](NIRI.md)

## Status and source map

**Status recorded on 2026-09-11:** implemented for GNOME Shell/Mutter **50**, specifically built and exercised with **50.4 / Mutter ABI 18**. This is a native stage effect, not a capture-overlay application. GNOME 51, other Mutter ABI majors, GLES, and GLX are not supported by this backend's current guards.

The owning checkout is `gnome-backend`, locally `/home/knd/AI/WaylandShader-gnome`. Commit `e1bcfc0` records the first GNOME implementation. Subsequent bypass-parameter and GTK-picker fixes were observed and implemented in the working tree on 2026-09-11; do not pretend they have separate published commits. No published `gnome-backend` reference was returned during the documentation inventory. The source paths below are relative to that checkout, not to every branch carrying this documentation.

| Piece | Source | Responsibility |
| --- | --- | --- |
| Package and development shell | `gnome/default.nix`, `gnome/shell.nix` | Matching Mutter dependency set, native checks, Qt/GTK runtime wrapping |
| Build and native interface generation | `gnome/CMakeLists.txt` | ABI guard, shared module, GIR/typelib, extension schemas, separate binaries |
| Shell integration | `gnome/extension/extension.js` | Load the native type, attach to the original stage, discover monitors, expose D-Bus, save settings, clean up |
| Native effect | `gnome/native/waylandshader-effect.cpp`, `.h` | GObject/Clutter lifecycle, frame hooks, Cogl resources, color conversion, scheduling and worker ownership |
| Shared shader host | `src/shader_runtime.cpp`, `.h` | Desktop OpenGL/EGL runtime, per-output chains, parameters and context restoration |
| Shared frontend source | `src/controller_client.cpp`, `src/controller_main.cpp`, `src/ctl_main.cpp` | Qt GUI/CLI built with the GNOME backend selection |
| Private shader library | `shader-library.nix`, `patches/librashader-gl-lifetime.patch` | Pinned librashader 0.10.1 with private GL lifetime corrections |
| Persistent configuration | `gnome/extension/schemas/org.gnome.shell.extensions.waylandshader.gschema.xml` | Versioned native configuration JSON in GSettings |
| Retained regressions | `gnome/tests/effect-lifecycle.js`, `gnome/tests/parameter-bypass.js` | GI ownership/pending-disable and real bypass/rebuild behavior respectively |

## Architecture: three processes/layers, not one application

```text
Qt controller or CLI (separate process)
    -> session D-Bus: org.gnome.Shell, /WaylandShader
    -> org.waylandshader.Effect methods in the GJS extension
    -> native WaylandShader.Effect GObject / ClutterEffect
    -> original stage paint, per-view Cogl processing, presentation
```

The renderer lives inside **GNOME Shell**. Closing or crashing the controller does not disable it. Master bypass is a separate setting; the CLI can change it even after the GUI closes. This separation explains why the desktop effect survived the Browse crash.

The GJS layer is deliberately small. It loads `WaylandShader-1.0` using the extension's `native/` search paths, subscribes to monitor/session/shutdown signals, and exports the control object. The C++ layer owns GL/EGL state, resources, compilation and rendering. C++ was reused for the existing runtime and Qt frontend; GJS remains the natural Shell integration language. The [language decision](../PROJECT-HANDBOOK.md#4-why-c-was-chosen) is not a recommendation to write every GNOME extension entirely in C++.

### What happens to a frame

1. A real stage `before-paint` hook discovers the host EGL display/context, including frames that might otherwise use direct scanout. Constructors and D-Bus calls do not invent a renderer context.
2. Connector IDs are mapped to active physical monitors, CRTCs and matching `ClutterStageView` geometry. Tiled monitors, shared/missing CRTCs and mismatched views are reported and passed through rather than guessed at.
3. The selected view's damage is expanded **before culling**. The paint hook requires the matching frame number and the view's original framebuffer. A framebuffer substituted by another stage effect is not processed again.
4. The original scene paints using its original context, preserving recorder watches. The result is copied into an effect-owned raw surface.
5. When necessary, Cogl transforms that scene into the shader's sRGB working color state. FP16 resources and mastering headroom avoid an implicit SDR tone-map in the tested HDR round trip.
6. A selected shader runs through that output's own librashader chain, with its frame number, measured/nominal refresh cadence and history-reset state. Color-only operation needs no shader preset.
7. Gamma and saturation are applied, and Cogl converts back to the target color state. Pixel-space blits preserve the compositor's view/monitor transform contract.
8. If processing fails after writing the destination, the raw scene is restored where possible and an error is reported. Native failures are caught at the C/GObject boundary; exceptions must not unwind into GJS or Clutter.

This is GPU-side work, but it still includes copies, intermediate surfaces, conversions and synchronization. It is not evidence of zero cost, universal HDR correctness or lower latency.

### Scheduling and ownership invariants

- Each shader-active view has a timeline tied to **its own frame clock**. Color-only processing does not self-animate an otherwise idle desktop.
- Current Slang scheduling is conservative: shader-active views continue repainting. The proposed static-shader optimization is not installed; see [refinement priorities](../PROJECT-HANDBOOK.md#6-future-refinements).
- Unredirect/direct-scanout inhibition is held before the first requested processed frame. Software-cursor requests and unredirect inhibition must be balanced on every stop/error path.
- A replacement compiles on a worker. A generation number prevents stale results from replacing current state. A topology/privacy reset invalidates results but does not free a job that its worker still owns.
- Destroy old chains before textures they may retain. Resized destinations can remain in a retired-resource slot until the runtime no longer references them.
- Stop timelines before detaching views. Drain the worker and completion source before releasing the host context/display references.
- A failed replacement retains the known working preset; if a topology reset already retired its GPU chains, the old selection can be rebuilt rather than stranded.

## How it was built, piece by piece

This is a dependency-order reconstruction from current source and recorded fixes. The first GNOME commit is coarse-grained; these steps are not claimed to be individually dated commits.

### 1. Preserve the working KDE backend

A separate `gnome-backend` worktree kept the KWin implementation available. The default top-level build stayed KWin. GNOME gained its own CMake entry point, package and executable names rather than replacing the KDE plugin with a compatibility shim.

### 2. Extract the reusable shader dependency

The pinned, patched librashader derivation moved into `shader-library.nix` on this branch. Both branch-local backend builds can use it. The private SONAME prevents these GL ownership changes from silently replacing the system's librashader for unrelated applications.

### 3. Reuse the runtime without assuming a Qt event loop in Shell

`gnome_shader_runtime` builds the existing C++ shader host as a position-independent static library. The native effect uses a standard worker thread and schedules completion onto the owning GLib main context. GNOME Shell is not converted into a Qt application.

### 4. Adapt to Mutter's real native API

A `ClutterEffect` subclass and GObject-facing header make the renderer available through GIR/typelib to GJS. The build requires `libmutter-18` and rejects incompatible major versions.

One concrete C++ integration obstacle was Mutter 50's C header forward-declaration of `ClutterPaintFlag`. The CMake build moves the unchanged enum definition into **build-local header copies**. It does not edit the installed SDK or alter enum values/layout. If that declaration changes upstream, configuration deliberately fails for review.

### 5. Attach to the original stage and map outputs

The extension adds the effect to `global.stage`, not an overlay actor showing a screenshot. It records connector IDs and geometry. Native mapping validates the actual monitor/CRTC/view relationship so one unsupported output does not silently shade the wrong monitor.

### 6. Establish correct capture, color and repaint boundaries

The renderer added full-view damage expansion, frame/framebuffer identity checks, raw-scene recovery, Cogl color conversion, FP16 working surfaces, and per-view clocks. These prevent seemingly successful tests from hiding stale pixels, repeated filtering, wrong output routing or lost color headroom.

### 7. Add control and persistence

The shared Qt frontend is compiled with `WAYLANDSHADER_GNOME`. The extension exports the same logical effect interface at GNOME's service/path and persists one versioned configuration string. `_settingsLoaded` prevents early status callbacks from overwriting saved settings before import; startup and preset restoration were tested separately from steady-state operation.

### 8. Verify native behavior before installing into a working session

Recorded isolated tests used real GNOME/Mutter 50.4 views, real application surfaces and post-paint framebuffer checks. They covered Slang identity/parameter behavior, independent color, bypass, small redraws, unsupported-output isolation and a synthetic PQ/HDR round trip. These are stronger than a build result, but not substitutes for physical HDR, power or latency measurements.

### 9. Integrate the correct package with GLF-OS

The local NixOS module was changed to select `gnome/default.nix` for `glf.environment.type = "gnome"`, while preserving the original KWin input for Plasma. A stale user-local extension link was removed after disabling it so it would not shadow the system package. The installed system extension was then discovered and exercised in a fresh disposable GNOME session.

### 10. Refine behavior found during actual use

Two later failures were corrected without changing the renderer's safety contract:

- **Bypassed parameter edits:** disabling rendering released the runtime, but the UI still offered the retained parameter definitions. Validation was moved to `ShaderParameter::validateValue`, shared by live and cached edits. Previously loaded, known parameters can now be edited while bypassed; unknown, nonfinite or out-of-range values are rejected. GPU history is still released, and in-progress compilation still rejects edits.
- **Browse crashed the GUI:** the GTK 3 file picker aborted because `org.gtk.Settings.FileChooser` was missing from the controller's environment. The GNOME package now declares GTK 3, uses `wrapGAppsHook3`, and merges its arguments into the Qt wrapper with `dontWrapGApps = true`. The fix supplies the dependency instead of forcing a non-native dialog or suppressing the fatal error.

## Step-by-step build, install and use

### 1. Confirm the source and compositor

Use the **GNOME checkout**, not the similarly named niri branch or the KDE package's default entry point. These read-only commands help identify a mismatch:

```bash
gnome-shell --version
printf '%s\n' "$XDG_SESSION_TYPE" "$XDG_CURRENT_DESKTOP"
```

Expected target: a GNOME 50 Wayland session with desktop OpenGL/EGL. A matching package set is more important than the newest package set.

### 2. Build with the target system's dependencies

From the GNOME repository root on the documented GLF-OS host:

```bash
mkdir -p "$HOME/.cache/waylandshader"
nix build --impure \
  --out-link "$HOME/.cache/waylandshader/gnome-result" \
  --expr 'import ./gnome/default.nix {
    pkgs = (builtins.getFlake "/etc/nixos").nixosConfigurations.GLF-OS.pkgs;
  }'
```

`GLF-OS` and `/etc/nixos` are this host's configuration name/path; substitute the target system's actual ones. On another matching Nix setup, `nix-build gnome/default.nix` uses `<nixpkgs>`, which must itself match the compositor. Building is not installation or activation.

Expected products: `waylandshader-gnome-controller`, `waylandshader-gnomectl`, and `share/gnome-shell/extensions/waylandshader@man33li.github.com/` containing the JavaScript, native library, GIR/typelib and compiled extension schema. The package also runs `gnome_effect_lifecycle`.

### 3. Add the GNOME source as a distinct system input

The documented host uses `waylandshader-gnome` as a non-flake source input. A module imports:

```nix
(import "${waylandshader-gnome}/gnome/default.nix" { inherit pkgs; })
```

This expression belongs in the module's package list with the source supplied through `specialArgs` or `inputs`; it is not a standalone NixOS module. Keep the KWin source/input separate. Use the existing desktop-selection module instead of installing the KWin package and expecting GNOME to load it.

After changes to the locked local source, including documentation:

```bash
sudo nix flake update waylandshader-gnome --flake /etc/nixos
sudo nixos-rebuild build --flake /etc/nixos#GLF-OS
sudo nixos-rebuild dry-activate --flake /etc/nixos#GLF-OS
```

Review the changes before manually running `nixos-rebuild switch`. If a proposed activation restarts the display manager or otherwise ends the session, save work and schedule it rather than blindly continuing. `glf-update` stages a next-boot generation; it is not equivalent to `switch`.

### 4. Let Shell discover the installed native module

After first installation or a **native-module code/ABI update**, save work, log out and start a new GNOME login. Do not restart/replace the live Wayland compositor. Toggling an extension cannot safely replace a native GI type already loaded into the process.

A **controller-wrapper-only fix**, such as the GTK-picker dependency correction, needs only a newly launched controller. The existing native effect can keep running.

Check discovery:

```bash
gnome-extensions info waylandshader@man33li.github.com
waylandshader-gnomectl outputs
```

The system installation should not be shadowed by an old copy under `~/.local/share/gnome-shell/extensions/`. Inspect that path before removing anything; preserve unrelated/user-managed extensions. Read-only CLI commands do not enable an absent effect. Launching the GUI or issuing a mutation may enable only this extension, provided GNOME permits extensions.

### 5. Start with color-only controls

```bash
waylandshader-gnome-controller
waylandshader-gnomectl outputs
```

Choose an actual output ID returned by `outputs`. For example, **only if `DP-1` is the intended monitor**:

```bash
waylandshader-gnomectl output DP-1 color on
waylandshader-gnomectl output DP-1 gamma 1.1
```

Gamma and saturation controls do not force the master switch on. Neutral values are `1`; a color checkbox can be enabled without requiring a non-neutral color pass. Preserve the settings of other monitors while testing.

### 6. Browse, select and explicitly load a preset

Browse uses Qt's native picker—GTK 3 under GNOME's default Qt theme—not a Nautilus or Dolphin process. Selecting a `.slangp` only changes the path field. Press **Load preset** to compile/apply it, or use:

```bash
waylandshader-gnomectl load /absolute/path/to/preset.slangp
waylandshader-gnomectl parameters
```

Use parameter names/ranges from `parameters`, and a known, trusted preset with all its referenced shaders/LUTs. Presets are not sandboxed workloads: an expensive or faulty native/driver path can affect the compositor.

### 7. Bypass and recover deliberately

```bash
waylandshader-gnomectl disable
waylandshader-gnomectl status
```

This bypasses shader and color processing; closing the window alone does not. `enable` restores processing from saved settings. Previously loaded parameters remain editable during bypass and take effect when the runtime is rebuilt. A rejected replacement should not replace a working preset.

For diagnosis, inspect status first, then narrowly scoped journal/coredump evidence. A controller crash and a GNOME Shell crash are different incidents; do not reset the renderer merely because a file picker failed.

## Verification recipes and their limits

### Retained CTest check

From the GNOME checkout, enter the matching `gnome/shell.nix` environment and keep the build outside the locked source directory:

```bash
nix-shell gnome/shell.nix --arg pkgs \
  '(builtins.getFlake "/etc/nixos").nixosConfigurations.GLF-OS.pkgs'
```

Inside that shell:

```bash
build_dir="$HOME/.cache/waylandshader/build-gnome"
cmake -S gnome -B "$build_dir" -G Ninja -DWAYLANDSHADER_BUILD_TESTS=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure
```

`gnome_effect_lifecycle` creates a real GI object, queues preset/disable work, forces garbage collection, and checks delivery/retention and shutdown. It **does not attach to a running stage or prove GPU rendering**. Recorded package builds passed this check 1/1.

### Real bypass regression in a disposable Shell

The retained `gnome/tests/parameter-bypass.js` controls a live effect. Never give it the working desktop's bus address. Its refusal to overwrite an existing active preset is a secondary guard, not permission to use the real session.

In a matching GNOME development shell, **terminal A**, from the GNOME checkout:

```bash
test_root=$(mktemp -d /tmp/waylandshader-gnome-check.XXXXXX)
printf 'Disposable test root: %s\n' "$test_root"
mkdir -p "$test_root"/{runtime,config,data,cache,state}
chmod 700 "$test_root/runtime"
(
  export XDG_RUNTIME_DIR="$test_root/runtime"
  export XDG_CONFIG_HOME="$test_root/config" XDG_DATA_HOME="$test_root/data"
  export XDG_CACHE_HOME="$test_root/cache" XDG_STATE_HOME="$test_root/state"
  export XDG_DATA_DIRS="$HOME/.cache/waylandshader/gnome-result/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
  export GSETTINGS_BACKEND=keyfile LIBGL_ALWAYS_SOFTWARE=1
  export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME
  unset DISPLAY WAYLAND_DISPLAY
  exec dbus-run-session -- bash -c '
    printf "%s\n" "$DBUS_SESSION_BUS_ADDRESS" > "$XDG_RUNTIME_DIR/bus-address"
    gsettings set org.gnome.shell disable-user-extensions false
    gsettings set org.gnome.shell enabled-extensions "[\"waylandshader@man33li.github.com\"]"
    exec gnome-shell --wayland --headless --no-x11 \
      --wayland-display waylandshader-check \
      --virtual-monitor 640x480@60 --virtual-monitor 800x600@75
  '
)
```

Wait for **GNOME Shell started**, not just creation of `bus-address`. The XDG variables must be set **before** `dbus-run-session` so activated services use the disposable state too. This recipe assumes the package was built at the cache link above; do not replace the installed extension in your working session to run it.

In **terminal B**, in the same checkout and matching development environment:

```bash
read -r -p 'Paste the disposable test root printed in terminal A: ' test_root
private_bus=$(cat "$test_root/runtime/bus-address")
gjs -m gnome/tests/parameter-bypass.js "$private_bus"
```

The test loads a real generated Slang preset, edits while disabled, rejects out-of-range/NaN/unknown values, re-enables and checks the saved value after reconstruction. It passed in fresh disposable sessions, including discovery from the system extension directory. Its result is not a physical-display, suspend or HDR certification.

Stop only terminal A's disposable process tree when finished. Confirm its private bus/services and any mounts under that printed temporary root have stopped before removing that root. Never use `killall gnome-shell`, broad dconf resets, or unmount the working session's `/run/user/.../doc` or `gvfs` mounts. The earlier managed tests used a supervisor for this lifecycle; the two-terminal recipe exposes the same required separation without silently running against the desktop.

### Real GUI and presentation checks

A useful acceptance pass opens Browse, cancels, reopens, selects a path containing spaces, and verifies the path without loading an unwanted preset. Then compare saved effect settings and close only the test controller. The GTK fix was verified this way in the real GNOME session, visually and through native accessibility interaction, then repeated using the installed launcher.

On the recorded system, **GNOME monitor screencasts captured the unfiltered scene**. They proved that the GUI/file picker rendered, not that final native processing was correct. Direct KMS scanout was used for the physical primary-output processing check. KMS access is privileged, device/plane-specific and format/driver-sensitive; do not copy another machine's plane ID. Allow authentication/OSD animations to settle before comparing images. No exact gamma calibration claim is made from those captures.

## Successes, failures and lessons

Evidence labels: **code** = inspectable implementation; **recorded** = earlier verification record; **observed** = this project's 2026-09-11 GNOME sessions; **open** = not yet established.

| Event | Evidence and result | Lesson / remaining boundary |
| --- | --- | --- |
| First native backend | **Code:** `e1bcfc0`, separate native library, GIR/typelib, GJS and Qt binaries | Port the compositor integration, not just the application branding |
| Mutter C-header/C++ incompatibility | **Code:** guarded build-local enum-definition move | Private SDKs require narrow, reviewable compatibility treatment; never weaken a guard blindly |
| Cold activation/restoration races | **Recorded + code:** real frame-hook context discovery and settings-import guard | Test startup, direct scanout and restored settings, not only an already-active effect |
| Isolated per-view rendering | **Recorded:** real Slang/color/bypass, small redraws, independent views, unsupported-output isolation | Strong integration evidence, not a physical-display guarantee |
| Synthetic PQ/HDR identity | **Recorded + code:** preserved mastering headroom through the FP16 working path | Arbitrary HDR presets and real HDR displays remain **open** |
| Wrong installed backend | **Observed:** GNOME build was staged while the system module still selected KWin; desktop-matched selection installed GNOME | Build success does not prove correct desktop deployment |
| Apparent updater failure | **Observed:** `glf-update` had selected a next-boot generation | Distinguish build, boot selection, system switch and fresh native-module discovery |
| Bypass rejected edits | **Observed:** old module rejected a valid edit with no GPU runtime; retained regression failed before and passed after the metadata-validation fix | Release history/resources without discarding the loaded parameter contract |
| Browse aborted controller | **Observed:** fatal missing `org.gtk.Settings.FileChooser`; native GTK picker works after runtime wrapping | Declare indirect UI runtime dependencies; do not hide the symptom with a different dialog |
| Effect survived GUI closure | **Observed:** renderer remained active, no error, settings unchanged | Intended process separation, not another bug |
| Screen capture looked unfiltered | **Observed:** monitor screencast vs direct scanout saw different stages | Identify the observation point before diagnosing rendering |
| Physical rotation/hotplug/lock/suspend/VRR/power | **Open** as a comprehensive hardware matrix | Isolated checks and hook presence do not establish these behaviors |

## Future refinements

1. Retain the bypass and ownership checks; use a real native GUI smoke check for packaging changes, especially Browse/Open/Cancel and clean-launch environment differences.
2. Complete physical multi-monitor hotplug/scaling, lock/unlock, suspend/resume and fresh-login upgrade checks without disturbing a working session during development.
3. Validate real HDR/wide-gamut output and representative presets; distinguish preserved working headroom from end-to-end display correctness.
4. Implement damage-driven static scheduling only after compiled-runtime metadata can reliably prove non-temporal behavior. Preserve full-view damage expansion and per-view clocks.
5. Measure frame cost, idle/active power, presentation latency and direct-scanout/VRR effects under controlled comparable conditions. Do not advertise unmeasured savings.
6. Revisit unsupported tiled/shared-view configurations only with an explicit ownership/routing design and independent-output tests; do not map by position heuristics.

## Maintenance and patch rules

- Keep Mutter's Nix version assertion and CMake ABI check consistent. A major update is a port/review task, not a version-string edit.
- Review Cogl color APIs, original-framebuffer/frame matching, stage hooks, output mapping and native GI ownership whenever GNOME changes.
- Treat the build-local SDK header adaptation separately from the private librashader patch. They solve different problems and have different upstream removal criteria.
- Keep GTK schema wrapping for the Qt native chooser. The extension's own `configuration` schema and GTK's file-chooser schema are **different dependencies**.
- Keep generated GIR/typelib and native `.so` from the same build. Avoid stale user-local copies shadowing the system extension.
- Preserve the GSettings JSON version and import compatibility deliberately. Do not migrate KWin UUID profiles to GNOME connector profiles by assuming identities match.
- Back up only the extension's configuration when needed, rather than dumping/resetting all desktop settings. For example, `dconf dump /org/gnome/shell/extensions/waylandshader/` is read-only; restoration should be deliberate and performed with the effect disabled.
- Follow the [shared patch/release workflow](../PROJECT-HANDBOOK.md#7-maintenance-and-patches) before upgrading librashader, changing ownership, or publishing a backend update.

## Evidence and further reading

- Current source paths and coarse commit listed above; retained GJS tests describe exactly what they assert.
- `README.md` and `WaylandShader-Guide.txt` on `gnome-backend` retain earlier rendering verification context.
- The 2026-09-11 sessions recorded the bypass pre-fix failure/post-fix pass, system-extension discovery, GTK fatal diagnostic, real Browse/Open/Cancel checks, and unchanged effect configuration. Temporary captures/scripts were removed; they are not a shipped test harness.
- [GNOME Shell source](https://gitlab.gnome.org/GNOME/gnome-shell), [Mutter source](https://gitlab.gnome.org/GNOME/mutter), [GObject Introspection](https://gi.readthedocs.io/), and [Qt file-dialog documentation](https://doc.qt.io/qt-6/qfiledialog.html) explain the upstream contracts. Always consult the versions matching the build.
