# Evaluation: damage-driven rendering for non-temporal presets

**Proposal:** [proposed-perfomance-enhancer.md](proposed-perfomance-enhancer.md)  
**Scope:** KDE/KWin 6.6, GNOME/Mutter 50, and the available niri backend source state.  
**Status:** review completed; the optimization has not been applied.

## Verdict

The optimization is worth implementing, but the proposed patches are not safe to apply verbatim.

- **KDE/KWin:** the scheduling gate fits the existing implementation, provided classification reliably describes the compiled runtime.
- **GNOME:** the same optimization fits, but must control per-view Clutter timelines rather than KWin's post-paint hook.
- **niri:** no native WaylandShader implementation was found in the available local or published `niri-backend` branch. The assessment below describes integration requirements, not approval of an implemented port.

The main blocker is the proposed temporal detector, not damage-driven rendering itself.

## High priority: the scanner misclassifies valid animated shaders

The scanner in proposal lines 172–243 does not cover all preprocessing accepted by librashader. Unsupported syntax is silently overlooked rather than classified as inconclusive.

The proposal's exact Qt scanner was compiled in a temporary standalone probe. Shader fixtures were then loaded and rendered through the existing `ShaderRuntime` in a surfaceless desktop OpenGL/EGL context.

| Fixture | Scanner result | Red channel on successive identical-input frames |
| --- | --- | --- |
| Static shader | Static — correct | `0.25 → 0.25` |
| Direct `FrameCount` use | Temporal — correct | `0 → 1` |
| Animation in `#pragma include_optional` | **Static — incorrect** | `0 → 1` |
| Animation in an unquoted `#include` | **Static — incorrect** | `0 → 1` |
| `FrameCount` constructed through token-pasting macros | **Static — incorrect** | `0 → 1` |
| Static source replaced with temporal source after scanning | **Static — incorrect for the compiled shader** | `0 → 1` |

Every fixture compiled successfully. These are rendering counterexamples, not merely regex concerns.

[librashader 0.10.1's include preprocessing](https://github.com/SnowflakePowered/librashader/blob/librashader-v0.10.1/librashader-preprocess/src/include.rs#L116-L159) accepts both optional includes and unquoted include filenames. The proposed include regex only recognizes quoted `#include` directives. Token-pasting macros can construct semantic names such as `FrameCount` without that complete token appearing in the scanned source.

The proposed claim that "anything inconclusive is temporal" is therefore not satisfied. With the new scheduling gate, these false-static results could freeze animation between unrelated desktop updates. A manual `ForceContinuousRepaint` escape hatch is useful, but does not make false-static classification safe.

Source scanning can also produce harmless but costly false-temporal results from comments, disabled preprocessor branches, and shader paths overridden by referenced presets. Those reduce the expected benefit rather than breaking rendering.

## High priority: classification is not bound to compiled input

The proposed classification runs before compilation. `src/shader_runtime.cpp:359–414` subsequently reopens the preset and creates a chain independently for each output.

The probe explicitly exercised this ordering:

1. Scan a static shader.
2. Replace its source with a temporal shader.
3. Load and render it.

The saved classification remained static, while successive rendered frames changed from red `0` to `1` on identical input.

Storing the flag in `ShaderRuntime` correctly couples it to successful runtime publication and failed-load rollback. It does **not** solve this source-snapshot mismatch by itself. Classification must use the exact compiled input, or classification and compilation must consume one immutable input snapshot.

## Recommended classification design

Use the compiled chain's active reflection rather than maintaining a second, incomplete shader parser.

librashader already:

- [Reflects active buffer-member ranges](https://github.com/SnowflakePowered/librashader/blob/librashader-v0.10.1/librashader-reflect/src/reflect/cross/mod.rs#L351-L379), providing the distinction between an unused boilerplate declaration and an actual uniform read.
- [Retains reflection in each compiled GL pass](https://github.com/SnowflakePowered/librashader/blob/librashader-v0.10.1/librashader-runtime-gl/src/filter_pass.rs#L33-L41).
- Uses reflected texture semantics when configuring history and feedback resources.

The stable C API does not currently expose the needed compiled-chain requirement query. However, this project already maintains a private patched librashader. Exposing frame/history/feedback requirements from that implementation is preferable to duplicating its preprocessing rules.

Implementation requirements:

1. Inspect active semantics across all relevant shader stages and passes, including history/feedback dependencies.
2. Aggregate requirements once during loading; the per-frame accessor should return stored metadata, not rescan files or traverse reflection.
3. Associate classification with the successfully compiled runtime, preserving the old runtime and its classification on failure or stale asynchronous completion.
4. Keep temporal or unknown runtimes continuously scheduled. Only proven non-temporal runtimes may stop self-scheduling.
5. Do not independently reinterpret the original files after compilation as a substitute for compiled metadata.

## KDE / KWin 6.6 assessment

### Suitable integration point

The proposed gate belongs in `DesktopEffect::postPaintScreen()`, currently at `src/effect.cpp:731–743` in the KDE checkout.

The existing lifecycle supports damage-driven static presets:

- `finishLoad()` installs only successful replacements and requests repaint on success and failure (`src/effect.cpp:357–395`).
- Master enablement, parameters, output/color settings, configuration, lock/unlock, and rendering errors independently request repaint.
- The no-selected-output load path validates a preset without leaving active rendering chains; subsequent output selection already requests the necessary work.

No missing explicit repaint invalidation was found in the reviewed effect lifecycle that requires the perpetual repaint loop to remain enabled.

### Invariants to preserve

- Keep full-output paint expansion in `prePaintScreen()` (`src/effect.cpp:550–561`). A static blur, warp, or scaler is not necessarily spatially local.
- Keep `usesShader()` as the decision to apply the shader, not as the decision to animate continuously.
- Retain loaded-output filtering and existing scanout protection.
- Publish scheduling metadata with the loaded runtime, not the requested preset path.

The proposed construction-time `ForceContinuousRepaint` setting is workable. The documented effect reload requirement is intentional. If live reconfiguration is desired, read it through the existing reconfigure path and request repaint.

**Assessment:** a small compositor-side change after reliable runtime metadata exists.

## GNOME / Mutter 50 assessment

### Suitable integration point

GNOME creates a repeating per-view `ClutterTimeline` whenever `Engine::shader()` is true (`gnome/native/waylandshader-effect.cpp:622–653` in the GNOME worktree).

The optimization must gate:

- Timeline creation and retention.
- The timeline callback's redraw requests.

Use a distinct continuous-frame demand predicate. **Do not change `Engine::shader()` to return false for static presets.** That predicate also controls actual processing, active status, and cursor-related behavior.

### Invariants to preserve

- Keep full-view damage expansion in the `before-paint` hook (`gnome/native/waylandshader-effect.cpp:977–989`).
- Keep explicit redraws for parameter changes, output settings, topology changes, enablement, and history/privacy resets.
- Keep cursor inhibition and unredirect inhibition while processing remains active, even when no continuous timeline is needed.
- Keep each output's own frame clock; do not substitute a global periodic timer.

`completeLoad()` already calls `syncActivity()` and `redraw()` (`:835–882`), providing the necessary transition point for static → temporal → static and failed replacement behavior. Output settings also use a shared invalidator that synchronizes activity and requests redraw (`:1221–1228`).

The override must use GNOME's existing GSettings/configuration import/export path, not KConfig. GNOME's loader uses `std::thread`, rather than the proposal's KWin-specific QtConcurrent assumption.

**Assessment:** a small native scheduling change plus GNOME settings integration, after the shared classification problem is fixed.

## niri assessment and source-state correction

### Available implementation state

The available local and [published `niri-backend` branch](https://github.com/man33li/Wayland-Shader-KDE/tree/niri-backend) point to `6579b80e3cc7dd04ae3fd04f74c27fca5c8136bd`, a KWin implementation. The inspected tree has no native niri integration, Cargo manifest, or niri patch. Its README still identifies niri as planned.

The earlier implication that a native niri port had been implemented and pushed was incorrect. A retained launch of a now-missing niri-26.04 binary is not evidence that it contained WaylandShader integration. This assessment does not rule out an unavailable private or deleted experiment; it establishes what can be reviewed in the available source state.

### Conditional native integration requirements

The comparison baseline is upstream niri v26.04, commit `8ed0da44d974c32c6877d2f4630c314da0717ecb`, not a recovered WaylandShader port.

- Add shader temporal demand alongside normal client damage and compositor animation demands. Do not gate ordinary redraw requests, window/workspace animations, animated cursors, or lock updates on whether the preset is static.
- Update both scheduling and damage/cache generations. [The DRM renderer can return `NoDamage`](https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/backend/tty.rs#L1935-L2005) even after a redraw has been queued. A temporal tick must cause the processed result to be considered changed.
- Preserve complete source content and full-output coverage for nonlocal processing. A static shader can still move the effect of local scene damage elsewhere on the output.
- Account for real and estimated VBlank continuation and any supported nested backend without clearing unrelated scheduling causes.
- Publish classification with the successfully loaded runtime and preserve it on failure. Tie asynchronous results and scheduling ownership to live output generations.
- Invalidate/rebuild appropriately on mode, scale, transform, renderer-context, resume, and topology changes.
- Preserve lock safety. Cached unlocked desktop pixels or temporal history must not enter a locked frame, including through rendering-error fallback.
- Prevent scanout paths from bypassing active whole-output processing and make cursor inclusion policy explicit.

There is also a runtime prerequisite: [upstream niri uses Smithay's GLES renderer](https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/render_helpers/renderer.rs#L1-L53), while the current C++ `ShaderRuntime` explicitly rejects GLES (`src/shader_runtime.cpp:211–221`). The existing loader is not a drop-in native niri integration.

**Assessment:** conceptually applicable, but the four proposed C++ patches neither implement nor optimize native niri. Establish a supported native runtime/integration first.

## Performance claims need narrower wording

The proposal should promise:

> Remove the effect's self-scheduled frames for eligible idle presets.

It should not promise zero remaining overhead or equivalence to disabling the effect:

- Other compositor/client events can still schedule rendering, including frame-callback-only activity.
- Actual damage still runs the complete shader path.
- Textures and shader chains remain resident.
- KWin still blocks direct scanout while the effect is active (`src/effect.cpp:199–212`). GNOME likewise retains unredirect inhibition while processing is active.
- Mixed-output wakeups must be measured; KWin's [scene repaint routing](https://github.com/KDE/kwin/blob/v6.6.6/src/scene/scene.cpp#L646-L660) does not establish that a repaint request affects scheduling only on the intended output.
- The supplied KWin measurements were accepted as reported. They do not establish GNOME/niri savings, physical VRR behavior, or linear scaling to 480 Hz.

This is an idle scheduling optimization, not a cheaper per-rendered-frame shader pipeline.

## Separate mipmap proposal

Keep mipmap allocation changes out of this optimization.

For ordinary large two-dimensional textures, a full mip chain is approximately 4/3 of the base level. Removing the lower levels therefore saves approximately **25% of that texture's full-chain allocation**, not one-third of total per-output VRAM. Other source/destination, intermediate, history, feedback, and lookup allocations remain.

`force_no_mipmaps` affects pass behavior more broadly than source-texture allocation. Effective per-pass requirements, including inherited preset settings, must be established before disabling mip generation.

## Recommended implementation order

1. Add trustworthy compiled-runtime requirements to the private librashader integration.
2. Apply KWin's post-paint scheduling gate and GNOME's per-view timeline gate without changing normal rendering eligibility or full-output damage coverage.
3. Expose effective scheduling mode and the force-continuous override through each backend's existing controls/settings. A classification reason would make fallback behavior easier to diagnose.
4. Validate behavior in isolated compositor sessions before installing changes into a daily desktop.
5. Measure physical-display savings separately for each implemented backend.
6. Treat niri as native-backend work first; carry the same scheduling/damage invariants into that implementation rather than copying the KWin patch.

## Required observable verification

- Static shaders with unused frame-uniform boilerplate stop self-scheduling but still visibly process scene damage.
- Temporal uniforms reached through includes/macros and history/feedback dependencies remain continuously updated.
- Static → temporal → static transitions start and stop scheduling correctly.
- Failed replacements in either direction preserve the old runtime and its scheduling behavior.
- Parameters, master/per-output enablement, color changes, and effect removal repaint correctly without incidental client damage.
- Small scene damage still updates the complete output where spatial dependencies require it.
- Mixed monitors/refresh rates, another-output-only activity, and frame-callback-only commits do not invalidate performance claims.
- Zero selected outputs followed by selection, hotplug during compilation, resize/scale/rotation, lock/unlock, and DPMS/resume do not strand stale frames or scheduling ownership.
- The force-continuous override restores continuous scheduling.
- Physical power, latency, cursor-plane, scanout, and VRR behavior are measured separately; nested/surfaceless checks are not substituted for those measurements.

## Verification boundary

This was a read-only source review plus the six-fixture native rendering experiment described above. No optimization was applied, no compositor session was switched, and the reported power measurements were not repeated. Temporary experiment files were removed. Source line numbers identify the reviewed snapshots and may shift as development continues.
