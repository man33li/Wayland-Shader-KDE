# Proposed performance enhancer: damage-driven rendering for non-temporal presets

**Status:** proposal — not applied. Written 2026-09-10 for later evaluation.

## Summary

With any shader enabled, the effect currently forces a full repaint of that output on
**every frame**, even when nothing on screen changes. For static presets (gamma ramps,
LUTs, scalers — anything whose output depends only on the current scene) this burns GPU,
CPU, and power around the clock for no visual benefit. The proposal: detect at preset
load time whether the chain is *temporal* (reads frame uniforms, or samples
history/feedback textures) and only schedule continuous repaints for those. Static
presets fall back to normal damage-driven rendering, which is what the existing comment
in `postPaintScreen()` already claims happens.

## Measured cost (2026-09-10)

Setup: RX 6900 XT, preset `bsnes-gamma-ramp.slangp` enabled on DP-2 (1600×1200@85).
Method: sampled KWin's `drm-engine-gfx` time from `/proc/<pid>/fdinfo` (root needed —
kwin's setcap wrapper makes it non-dumpable) across `qdbus … toggleEffect waylandshader`
toggles, with `glxgears` as a damage source for the loaded phases.

| Phase              | KWin GPU time | KWin CPU time | GPU busy | Power  |
|--------------------|---------------|---------------|----------|--------|
| idle desktop, ON   | 65.3 ms/s     | 156 ms/s      | 8.9 %    | 55.0 W |
| idle desktop, OFF  | 7.9 ms/s      | 30 ms/s       | 0.6 %    | 50.9 W |
| glxgears, OFF      | 11.6 ms/s     | 45 ms/s       | 2.4 %    | 51.1 W |
| glxgears, ON       | 61.8 ms/s     | 143 ms/s      | 4.8 %    | 54.0 W |

Standing cost of the effect on a **fully static desktop**: ~57 ms/s of GPU gfx-engine
time (~0.68 ms per 85 Hz frame), ~126 ms/s of KWin CPU (~13 % of a core), ~4 W — paid
continuously. (The per-frame figure is inflated somewhat by low idle GPU clocks; the
point is that the work happens at all.) On a high-refresh output (DP-1 is 480 Hz) the
same behaviour would be proportionally worse.

## Root cause

`src/effect.cpp`, `DesktopEffect::postPaintScreen()`:

```cpp
void DesktopEffect::postPaintScreen()
{
    effects->postPaintScreen();
    if (processingEnabled()) {
        // Only temporal Slang chains need continuous frames. Static color controls
        // follow normal scene damage, and untouched monitors are not forced to redraw.
        for (const auto& [screen, state] : m_outputs) {
            if (usesShader(screen, *state)) {
                effects->addRepaint(screen->geometry());
            }
        }
    }
}
```

The comment describes the intended design, but `usesShader()` (`effect.cpp:193`) only
checks `shaderEnabled && m_runtime && output loaded` — there is no temporal-preset
check. Every painted frame schedules the next one, so a shader-enabled output renders
at its full refresh rate forever.

## Design

### Why source scanning

librashader 0.10.1's C API exposes no reflection: the only preset introspection is
`libra_preset_get_runtime_params()` (user parameters) and `libra_preset_print()`
(debug print). There is no way to ask whether a compiled chain binds
`OriginalHistory#`/`PassFeedback#` or reads frame uniforms. So classification has to
scan the preset's own sources. This runs inside `ShaderRuntime::load()`, which already
executes on a `QtConcurrent` worker — pure file I/O, no GL, no compositor-thread work.

### What counts as temporal

1. **Frame uniforms actually used**: `FrameCount`, `FrameDirection`, `FrameTimeDelta`,
   `OriginalFPS`, `CurrentSubFrame`, `TotalSubFrames`.
   Subtlety: nearly every shader in the RetroArch collection *declares*
   `uint FrameCount;` in its push-constant block as copied template boilerplate without
   ever reading it (verified: `reshade/shaders/LUT/LUT.slang`, used by the currently
   active `bsnes-gamma-ramp.slangp`, declares it and never uses it). A bare token scan
   would classify essentially the whole library as temporal. So: strip
   declaration-only lines (`uint FrameCount;` etc.) first, then search the remainder
   for the token — this catches `params.FrameCount`, bare uses inside anonymous
   blocks, and `#define` aliases.
2. **History/feedback semantics declared at all**: `OriginalHistory1…`,
   `OriginalHistorySize1…`, `PassFeedback#`, and `<PassAlias>Feedback#` for aliased
   passes. For textures, *declaration is sufficient*: librashader allocates and
   maintains history/feedback framebuffers based on reflection of declared resources,
   and shaders only declare the samplers they bind — so no declaration-vs-use
   distinction is needed (or safe) here.
3. **Anything inconclusive is temporal**: unreadable file, unresolvable path (e.g.
   RetroArch `$VID-DRV$` wildcards), include depth/file-count/size caps exceeded. A
   false "temporal" merely keeps today's behaviour; a false "static" would freeze an
   animated preset — the asymmetry drives every default in the scanner.

Scanning covers: the `.slangp` itself, `#reference`d presets (recursively), every
`shaderN = path` source, and each source's `#include` chain.

### Validation of the heuristic (run against the installed RetroArch collection)

| Shader | Expected | Detected |
|---|---|---|
| `reshade/shaders/LUT/LUT.slang` (active preset) | static | static (boilerplate `FrameCount` stripped) |
| `stock.slang` | static | static |
| `interpolation/shaders/sharp-bilinear.slang` | static | static |
| `bezel/Mega_Bezel/.../hsm-newpixie-accumulate.slang` | temporal | temporal (`PassFeedback1`, `PassFeedback16`) |
| `motionblur/shaders/motionblur-simple.slang` | temporal | temporal (`OriginalHistory1..7`) |

Regex gotcha found during validation: `Feedback\b` does **not** match `PassFeedback1`
(a digit follows, so there is no word boundary) — the pattern must allow trailing
digits: `Feedback[0-9]*\b`. The code below includes this fix.

### Why correctness is preserved

- `prePaintScreen()` (`effect.cpp:550`) already sets `PAINT_SCREEN_TRANSFORMED` and
  expands the paint region to infinite whenever the shader/color path is active — so
  *any* damage on the output still triggers a full-output render with the shader
  applied. The gate only eliminates frames with **zero** damage, where the previous
  buffer is still exact.
- Every state change that alters the image already schedules a repaint explicitly:
  `setParameter` (`addRepaintFull`, effect.cpp:433), `setEnabled` (:407),
  gamma/saturation/output setters, `finishLoad` (:385), `renderFailed` (:569),
  `saveOutputSettings` (:120), `screenLockingChanged` (:151). None of these rely on
  the continuous repaint loop.
- The validation-only load path (`outputs.empty()` in `ShaderRuntime::load`) never
  renders, so the classification result is simply unused there.
- Static chains never read `FrameTimeDelta`/`FrameCount` (by definition of the
  detection), so the irregular `elapsedMilliseconds`/`frame.number` values produced by
  damage-driven rendering are never observed.

## The code

Four small patches. No new files, no build-system changes (`QRegularExpression`,
`QFile`, `QDir`, `QSet` are all QtCore, already linked).

### 1. `src/shader_runtime.h` — accessor

```cpp
     const QList<ShaderParameter>& parameters() const;
+    // False when the loaded preset provably samples no history/feedback textures
+    // and reads no frame uniforms; such chains are pure functions of the current
+    // scene and only need re-rendering when the scene produces damage.
+    bool requiresContinuousFrames() const;
     bool render(uintptr_t output, const ShaderFrame& frame, QString& error);
```

### 2. `src/shader_runtime.cpp` — scanner, Impl field, accessor, hook

New includes (next to the existing `<QByteArray>`/`<QDebug>`/`<QFileInfo>` block):

```cpp
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
```

Inside the existing anonymous namespace (e.g. after `checkLibra`):

```cpp
// ---- Temporal preset detection ---------------------------------------------
// librashader's C API exposes no reflection, so classify the preset by scanning
// its sources. Any inconclusive scan returns "temporal": that can only fall back
// to continuous rendering, never freeze an animated chain.

constexpr int scanMaximumDepth = 16;
constexpr int scanMaximumFiles = 256;
constexpr qint64 scanMaximumBytes = 4LL * 1024 * 1024;

// Frame uniforms are boilerplate in nearly every slang template, so a bare
// declaration ("uint FrameCount;") does not make a chain temporal. Declaration
// lines are stripped before searching for real uses.
const QRegularExpression frameUniformDeclaration(QStringLiteral(
    "^[ \\t]*(?:uint|int|float)[ \\t]+"
    "(?:FrameCount|FrameDirection|FrameTimeDelta|OriginalFPS|CurrentSubFrame|TotalSubFrames)[ \\t]*;[ \\t]*$"),
    QRegularExpression::MultilineOption);
const QRegularExpression frameUniformUse(QStringLiteral(
    "\\b(?:FrameCount|FrameDirection|FrameTimeDelta|OriginalFPS|CurrentSubFrame|TotalSubFrames)\\b"));
// History and feedback are temporal as soon as they are declared: librashader
// allocates and maintains the buffers from reflection alone, and shaders only
// declare the samplers they bind. "<PassAlias>Feedback#" covers aliased passes.
const QRegularExpression historyOrFeedback(QStringLiteral(
    "\\bOriginalHistory(?:Size)?[1-9][0-9]*\\b|\\b[A-Za-z_][A-Za-z0-9_]*Feedback[0-9]*\\b"));
const QRegularExpression includeDirective(QStringLiteral(
    "^[ \\t]*#include[ \\t]+\"([^\"]+)\""), QRegularExpression::MultilineOption);
const QRegularExpression presetReference(QStringLiteral(
    "^[ \\t]*#reference[ \\t]+\"?([^\"\\r\\n]+?)\"?[ \\t]*$"), QRegularExpression::MultilineOption);
const QRegularExpression presetShaderEntry(QStringLiteral(
    "^[ \\t]*shader[0-9]+[ \\t]*=[ \\t]*\"?([^\"\\r\\n]+?)\"?[ \\t]*$"), QRegularExpression::MultilineOption);

struct PresetScan {
    QSet<QString> visited;
    bool temporal = false;
    bool complete = true;

    bool open(const QString& path, int depth, QString& text)
    {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty() || depth > scanMaximumDepth || visited.size() >= scanMaximumFiles) {
            complete = false;
            return false;
        }
        if (visited.contains(canonical)) {
            return false;
        }
        visited.insert(canonical);
        QFile file(canonical);
        if (file.size() > scanMaximumBytes || !file.open(QIODevice::ReadOnly)) {
            complete = false;
            return false;
        }
        text = QString::fromUtf8(file.readAll());
        return true;
    }

    void scanShader(const QString& path, int depth)
    {
        QString text;
        if (temporal || !open(path, depth, text)) {
            return;
        }
        if (historyOrFeedback.match(text).hasMatch()
            || frameUniformUse.match(QString(text).remove(frameUniformDeclaration)).hasMatch()) {
            temporal = true;
            return;
        }
        const QDir directory = QFileInfo(path).dir();
        for (auto it = includeDirective.globalMatch(text); it.hasNext() && !temporal;) {
            scanShader(directory.filePath(it.next().captured(1)), depth + 1);
        }
    }

    void scanPreset(const QString& path, int depth)
    {
        QString text;
        if (temporal || !open(path, depth, text)) {
            return;
        }
        const QDir directory = QFileInfo(path).dir();
        for (auto it = presetReference.globalMatch(text); it.hasNext() && !temporal;) {
            scanPreset(directory.filePath(it.next().captured(1)), depth + 1);
        }
        for (auto it = presetShaderEntry.globalMatch(text); it.hasNext() && !temporal;) {
            scanShader(directory.filePath(it.next().captured(1)), depth + 1);
        }
    }
};

bool presetNeedsContinuousFrames(const QString& presetPath)
{
    PresetScan scan;
    scan.scanPreset(presetPath, 0);
    return scan.temporal || !scan.complete;
}
```

`Impl` gains a field (safe default keeps current behaviour):

```cpp
    bool continuousFrames = true;
```

Accessor (next to `parameters()`):

```cpp
bool ShaderRuntime::requiresContinuousFrames() const
{
    return d->continuousFrames;
}
```

Hook in `ShaderRuntime::load()`, right after `impl` is created (around line 318-320) —
file I/O only, before any EGL/GL work:

```cpp
     auto impl = std::make_unique<Impl>();
     impl->display = display;
     impl->hostContext = shareContext;
+    impl->continuousFrames = presetNeedsContinuousFrames(file.canonicalFilePath());
```

### 3. `src/effect.cpp` — the gate

```cpp
 void DesktopEffect::postPaintScreen()
 {
     effects->postPaintScreen();
-    if (processingEnabled()) {
+    if (processingEnabled() && m_runtime
+        && (m_runtime->requiresContinuousFrames() || m_forceContinuousRepaint)) {
         // Only temporal Slang chains need continuous frames. Static color controls
         // follow normal scene damage, and untouched monitors are not forced to redraw.
         for (const auto& [screen, state] : m_outputs) {
             if (usesShader(screen, *state)) {
                 effects->addRepaint(screen->geometry());
             }
         }
     }
 }
```

(`usesShader` already requires `m_runtime`, so the added `m_runtime &&` only guards the
new call. The comment finally matches the code.)

### 4. `src/effect.h` / constructor — escape hatch

`effect.h`, next to the other flags:

```cpp
    bool m_forceContinuousRepaint = false;
```

`effect.cpp` constructor, where `[General]` is already read (~line 156):

```cpp
     const KConfigGroup general(m_config, QStringLiteral("General"));
+    m_forceContinuousRepaint = general.readEntry("ForceContinuousRepaint", false);
```

If detection ever misclassifies a preset, `ForceContinuousRepaint=true` under
`[General]` in `waylandshaderrc` restores the old behaviour without a rebuild (read at
effect construction; reload the effect or restart KWin to apply).

Optional, not included above: add `{ QStringLiteral("continuousRepaint"), … }` to the
per-output JSON in `status()` (effect.cpp:278) so the controller UI can show whether a
preset renders continuously.

## Expected impact

- **Idle desktop with `bsnes-gamma-ramp` (or any static preset): the standing
  ~57 ms/s GPU / ~126 ms/s CPU / ~4 W cost drops to zero.** The output stops
  compositing entirely between damage events, like with the effect off.
- Under active content the cost per *rendered* frame is unchanged by design — the fix
  removes only zero-damage frames.
- Temporal presets (motion blur, phosphor persistence, subframe/BFI, anything with
  history/feedback or real frame-uniform reads) behave exactly as today.

## Evaluation checklist

1. Build + install per the usual flow (`nix flake update waylandshader --flake
   /etc/nixos` as root, rebuild), or a local in-tree build for iteration.
2. **Static preset**: load `bsnes-gamma-ramp.slangp`, confirm the log/status shows it
   still applies visually, then re-run the fdinfo A/B measurement (method above).
   Expected: idle/ON ≈ idle/OFF (~8 ms/s), gears/ON still elevated (real damage).
3. **Temporal preset**: load `motionblur/motionblur-simple.slangp` (uses
   `OriginalHistory1..7`). Drag a window, stop — trails must keep decaying after
   movement stops (i.e. frames continue without damage). Also verify a
   `PassFeedback` preset (e.g. Mega Bezel newpixie) accumulates correctly.
4. **Escape hatch**: set `ForceContinuousRepaint=true`, reload, confirm continuous
   repaints return (fdinfo shows the old idle numbers).
5. **Transitions**: preset switch static→temporal→static via the controller; toggle
   the effect off/on; enable/disable the shader per output; unplug/replug DP-2. No
   stuck frames, no frozen animation, no leftover continuous repainting.
6. Run the repo's existing tests (`tests/`, `runtime-smoke`) — note the smoke test
   binary SIGABRTed on 2026-09-08 on this machine *before* any of these changes;
   worth re-checking independently.

## Notes / future work

- **Mip-chain allocation**: `paintScreen` always allocates the source as
  `GL_RGBA16F` with a *complete mip chain* when a shader is active
  (`effect.cpp:596-597`, `chainOptions.force_no_mipmaps = false` at
  `shader_runtime.cpp:356`). Whether a preset actually samples mips is declared in
  the `.slangp` itself (`mipmap_inputN = "true"`), which the same scanner pass could
  read; when absent, a single-level source plus `force_no_mipmaps = true` would save
  ~⅓ VRAM per output and any mip regeneration work. Left out of this proposal to keep
  it single-purpose, but the scanner is structured so this is a natural extension.
- **Upstream**: the clean long-term fix is a librashader C API that exposes chain
  requirements from its own reflection (it already knows, e.g. `required_frame_history`
  internally). Worth an upstream issue; the source scanner then becomes a fallback.
- The 0.68 ms/frame figure was measured at idle GPU clocks (~1600 MHz shader clock,
  56 W); at gaming clocks the same work takes less wall time. Don't read it as the
  preset being expensive — the problem is *when* the work runs (always), not how much
  one frame costs.
- KWin ≥ 6.7 is out of scope: this project intentionally hard-fails there; the patch
  touches no version-dependent API (`addRepaint`/`postPaintScreen` are stable across
  the supported 6.6 line).
