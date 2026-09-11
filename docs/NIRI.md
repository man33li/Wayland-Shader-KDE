# niri: inherited baseline, recovered experiments, and the native backend still to build

[Project handbook](../PROJECT-HANDBOOK.md) · [KDE development](KDE.md) · [GNOME development](GNOME.md)

## 1. Status and evidence boundaries

**Native WaylandShader support for niri is not implemented in the available branch. Meaningful niri research and GPU interoperability experiments were completed and survive in the private handoff. These are different facts.**

The inspected local `/home/knd/AI/WaylandShader-niri` worktree and its `origin/niri-backend` reference resolve to `6579b80e3cc7dd04ae3fd04f74c27fca5c8136bd`. The published branch was also recorded at that revision. It contains the KWin baseline, not a shader-enabled niri compositor. The handoff's closing publication message explicitly says so. A branch name, an existing `result` symlink, and a successful build of the inherited package are not evidence of a port.

This chapter uses the following labels:

- **[SOURCE]**: directly inspected current source, archive structure, or git history.
- **[HISTORICAL RECORD]**: retained experiment commands, code, output, or a contemporaneous report; not a new execution.
- **[PROPOSED]**: a concrete implementation or verification requirement that has not been delivered.
- **[UNVERIFIED]**: an outcome for which the available evidence is insufficient.

No builds, tests, package installations, compositor launches, or performance measurements were run for this documentation task. No native CachyOS experiment is established by the available handoff. No SSH hosts or `~/.ssh/config` are available for accessing the reported laptop. This limits the recoverable evidence; it does not prove that an unavailable private or deleted experiment never existed.

### Source and artifact map

Paths in this table are source locations, not promises that every mirrored branch contains them.

| Location | What it establishes |
| --- | --- |
| `niri-backend` at `6579b80`: `CMakeLists.txt` | C++20, Qt 6.10, ECM 6.22, KWin 6.6, a KWin effect plugin installed under `kwin/effects/plugins`; KWin 6.7 is rejected. No native niri target. |
| `niri-backend`: `default.nix`, `shell.nix` | KDE/KWin dependencies and private patched librashader 0.10.1 packaging, not a niri package or CachyOS PKGBUILD. |
| `niri-backend`: `src/effect.cpp`, `src/plugin.cpp` | KWin paint hooks, outputs, settings, and plugin entry point. These are not compositor-neutral ABI glue. |
| `niri-backend`: `src/shader_runtime.cpp`, `src/shader_runtime.h` | Existing desktop-OpenGL/EGL runtime host; `createContext()` explicitly rejects an OpenGL ES host. |
| `niri-backend`: `src/controller_main.cpp`, `src/controller_client.cpp`, `src/ctl_main.cpp` | Reusable UI and control concepts, but the existing transport addresses `org.kde.KWin`, `/WaylandShader`, `org.waylandshader.Effect`. It cannot already control niri. |
| `niri-backend`: `patches/librashader-gl-lifetime.patch`, `tests/fixtures/` | Inherited resource-lifetime/temporal fixes and shader fixtures; neither is a niri compositor patch. |
| `/home/knd/waylandshader-niri-handoff.tar.gz` | Private session archive containing instructions, research reports, embedded experimental source, command results, and an embedded reference screenshot. |
| Archive member `waylandshader-niri-handoff/import-session.py` | Session-copy re-rooting helper, not an installer or source-code importer. |
| Archive members `session/NiriRenderHooks.md`, `session/NiriCosmicRoutes.md`, `session/ShaderInterop.md` | Renderer/lifecycle and alternative-route research. Their conclusions remain research, not completed integration. |
| Archive member `session.jsonl` | Original command arguments, probe code and revisions, stdout, nested-session diagnostics, screenshot record, and final status qualifications. |
| `main`: `performance-enhancer-evaluation.md` | Later review correcting native niri status and defining conservative damage/temporal requirements. Its six-fixture experiment is a separate historical experiment, not a niri integration test. |

The archive has 243 members. Its payload is an importer, session JSONL, retained tool logs and research reports, a `session/local/kwin-contract.txt` note, and cached upstream material under `session/url-search/`. Rust and Cargo-shaped files in that cache are upstream research captures, **not a recovered native implementation**. There is no standalone WaylandShader niri Cargo workspace, niri renderer patch, CachyOS package recipe, or shader-enabled niri binary in the archive. Temporary C++ probe sources, configuration, and build instructions are embedded in recorded tool calls; they are not a maintained backend tree. The reference PNG is embedded in a session message, not supplied as a separate image file.

## 2. Architecture: what can be inherited, and what cannot

### The baseline that actually exists

The inherited rendering path is:

```text
KWin output composition
  -> per-output intermediate
  -> optional librashader preset
  -> optional gamma/saturation adjustment
  -> KWin presentation target
```

The separate Qt application and CLI send asynchronous D-Bus requests to the KWin effect. The effect owns live output processing, and settings use KWin output UUIDs and KConfig. See [KDE.md](KDE.md) for its completed construction and verification story.

Useful portable contracts include one selected preset and parameter set, independent output selection and color controls, separate per-output histories, last-known-good replacement, clear loading/error status, and master bypass without discarding settings. These are behavior to preserve, not justification for copying KWin types into niri.

### The pinned niri reference

**[SOURCE]** This chapter's upstream comparison is niri v26.04, revision `8ed0da44d974c32c6877d2f4630c314da0717ecb`. Its [Cargo manifest][niri-cargo] declares Rust 2021, minimum Rust 1.85, workspace version `26.4.0`, and Smithay revision `ff5fa7df392cecfba049ffed55cdaa4e98a8e7ef`. Its [renderer traits][niri-renderer] expose Smithay's `GlesRenderer` beneath both the nested renderer and native tty renderer. These facts are pinned; they are not claims about today's upstream `main` or the laptop's installed package.

Niri's existing short-animation shader support is not a persistent RetroArch `.slangp` runtime. The handoff examined [upstream issue 913][niri-913] and identified damage tracking as a central obstacle to long-running shaders. At the pinned reference there is useful internal framebuffer-effect machinery, not an established WaylandShader plugin installation interface.

### Proposed data path

**[PROPOSED]** A narrow compositor source patch is the best-developed route in the recovered research:

```text
normal niri scene, surface states and damage
  -> final output-sized framebuffer effect at presentation boundary
  -> effect-owned GPU input storage
  -> private desktop-OpenGL librashader execution
  -> synchronized EGLImage-backed GLES presentation texture
  -> normal Smithay/niri submission and presentation feedback
```

The [existing framebuffer-effect implementation][niri-fb] demonstrates capture, reusable GPU texture storage, damage commits, and implementations for both `GlesRenderer` and `TtyRenderer`. It is a useful precedent, not the missing WaylandShader code. The handoff proposes adding an output-sized effect at the final nested and DRM presentation boundaries while leaving normal scene construction intact. Preserve underlying surface IDs/states for frame callbacks, presentation feedback, dma-buf feedback and visibility bookkeeping. Flattening everything into an anonymous texture without preserving that information would be a larger and riskier change.

Do not add filtering indiscriminately to every generic `RenderTarget::Output` render: transition snapshots and other auxiliary renders can use that target too. A presentation-boundary design must explicitly decide whether screenshots and screencasts are unfiltered, filtered, or separately processed; it must never accidentally process snapshots twice or advance physical-output history for an auxiliary capture.

The GPU-copy/texture-interchange route avoids a capture viewer and same-output overlay feedback. It is not a claim of zero copies: the framebuffer-effect precedent captures the source with a GPU blit. It is also not proof of zero CPU fallback on every existing multi-GPU configuration. The handoff identifies CPU-copy fallback in pinned Smithay's multi-GPU path; native validation must bound supported configurations rather than hide that fallback.

### Proposed control path and language boundary

**[PROPOSED]** Keep the Qt/C++ settings application if its UI remains useful; implement niri's compositor-side ownership and event-loop integration in Rust, matching niri/Smithay. The shader engine is itself Rust, currently consumed by the C++ host through librashader's C API. A Rust adapter may use a reviewed C ABI wrapper around the private engine or a pinned Rust crate integration; choose after the runtime prototype establishes ownership and compatibility. Do not assume Rust crate internals constitute a stable ABI.

Rust is appropriate here because renderer types, output lifetimes and scheduling are already Rust code. It avoids forcing Qt/KWin event-loop and private-header dependencies into an unrelated compositor. C++ remains a sensible existing UI/runtime-host choice; it is not a requirement that every future backend be C++. The [handbook](../PROJECT-HANDBOOK.md) explains the shared language rationale and extraction rules.

Choose and document a niri transport before adapting the client: either a deliberately versioned extension to niri's IPC, or a distinct same-user D-Bus endpoint. Niri's optional D-Bus dependency in Cargo does not mean that the WaylandShader methods exist. Do not make a niri client load a KWin effect, address `org.kde.KWin`, or silently target another compositor when both run nested. Transport must expose backend/session identity and capabilities, separate accepted requests from successful runtime publication, and invalidate cached status when the server changes. Read-only status must not enable an effect. Serialize mutations onto the compositor's appropriate owner thread; never execute GPU calls in an arbitrary IPC handler thread.

## 3. Piece-by-piece reconstruction of the available development

This is dependency-order reconstruction backed by source and records, not an exact engineering diary inferred from coarse commits.

1. **Establish native composition as the goal.** The historical discussion separated a portal viewer from changing the actual compositor output. Layer-shell input transparency does not exclude the overlay from capture; niri's capture blocking can produce black coverage rather than reveal an underlying scene. A generic capture-overlay approach therefore did not satisfy seamless whole-output processing.
2. **Build and extend the KWin implementation first.** Commit `7a5c019` is already titled “Nearly fully featured Wayland shader in KDE.” It includes the substantial implementation rather than incremental commits for every component. `ff42c5f` adds the README. The separate license history is merged at `6579b80`. These commits establish the inherited state, not a niri rendering milestone.
3. **Preserve the working KDE line.** The historical session created `/home/knd/AI/WaylandShader-niri` on `niri-backend`. The purpose was isolation before developing a different compositor integration, not a declaration that branch creation completed the port.
4. **Map native niri insertion points.** `NiriRenderHooks.md` studies nested winit and DRM presentation, framebuffer effects, output identity, renderer ownership, scheduling, lock safety, screenshots, scanout and multi-GPU behavior. `NiriCosmicRoutes.md` establishes why portal and overlay capabilities are not an in-place `.slangp` API.
5. **Probe the GLES/desktop-GL boundary before committing to a runtime.** The session embeds a throwaway `probe.cpp` and CMake build. It exercised direct GLES and desktop OpenGL bridged with EGLImage, using the private patched librashader and inherited fixtures. It checked rendered pixels, not just successful shader compilation. Initial temporal/multipass checks passed for both paths; a subsequently added clamp-to-border fixture separated them.
6. **Record the compatibility failure rather than mask it.** Direct GLES produced an out-of-bounds sample of `0.75,0.25,0.75` where the border test expected black, then exited with `unexpected rendered pixel`. The desktop-GL/EGLImage path produced `0,0,0` and reported a pass. The historical explanation was GLES substituting edge clamping for border clamping. This is evidence to prefer the tested bridge as a starting point, not a universal claim that every GLES runtime or driver is incapable of correct support.
7. **Exercise stock niri nesting separately.** A Nixpkgs niri 26.04 executable was launched alongside KDE. The first X11 nesting attempt failed because `libXcursor.so.1` could not be opened. Native Wayland nesting subsequently reached a running compositor and output IPC; the archive records a screenshot of a native reference window. A readiness-marker timeout and session-service warnings are also retained. This established development-environment feasibility, not a shader-enabled compositor.
8. **Stop and remove temporary experiments.** The session records stopping the isolated session and removing its temporary probe directory. Its source writes, output and embedded image survive in the archive, but temporary binaries and paths should not be treated as current build artifacts.
9. **Publish the development branch honestly.** The final handoff message says local and remote `niri-backend` are `6579b80`, explicitly “not yet the niri rendering implementation.” The final architecture recommendation similarly says the GPU probes were not a shader-enabled niri build.
10. **Carry the later scheduling review forward.** `main`'s `performance-enhancer-evaluation.md` correctly reports the missing native integration and explains why queueing a redraw alone may still yield DRM `NoDamage`. Its source-scanner counterexamples strengthen the future requirement for compiled-runtime temporal metadata. They do not supply the absent backend.

## 4. Numbered guidance: what is usable now

There is no truthful `install this plugin in niri` sequence yet. These steps identify the available material and prepare a reproducible development handoff; they do not activate native WaylandShader support.

### 4.1 Inspect the inherited branch without mistaking it for a release

1. **Inspect the existing worktree and exact source revision:**

   ```sh
   git -C /home/knd/AI/WaylandShader-niri rev-parse HEAD
   git -C /home/knd/AI/WaylandShader-niri log -4 --oneline
   git -C /home/knd/AI/WaylandShader-niri show HEAD:CMakeLists.txt
   git -C /home/knd/AI/WaylandShader-niri show HEAD:default.nix
   ```

   These commands produce revision and source evidence. At the investigated baseline, observe the KWin target/dependencies and no niri target. Documentation-only commits may later change `HEAD`; the implementation baseline remains the pinned revision above.

2. **If obtaining the published baseline on another machine, clone into a new directory outside any locked local flake checkout:**

   ```sh
   git clone --branch niri-backend https://github.com/man33li/Wayland-Shader-KDE.git WaylandShader-niri
   git -C WaylandShader-niri rev-parse HEAD
   ```

   This creates a source checkout, not an installed niri effect. Compare its actual revision and build files with the published status; do not assume the branch will stay at this snapshot forever.

3. **Understand the inherited Nix command before using it.** At `6579b80`, `nix-build` evaluates the KWin derivation. If deliberately building that baseline for KDE investigation, an external output link avoids writing `result` into the checkout:

   ```sh
   nix-build /home/knd/AI/WaylandShader-niri/default.nix \
     --out-link /tmp/waylandshader-kwin-baseline-result
   ```

   A successful derivation produces a KWin plugin and existing controls. It neither patches niri nor demonstrates GLES compatibility, native DRM presentation, or CachyOS packaging. This is **not** a niri build/install recommendation or a command executed for this chapter. The expression uses `<nixpkgs>` unless matching `pkgs` are explicitly supplied. See the handbook for package-set matching and local-path input rules.

### 4.2 Inspect or resume the private handoff

4. **List the archive and read the importer before executing anything:**

   ```sh
   tar -tzf /home/knd/waylandshader-niri-handoff.tar.gz
   tar -xOzf /home/knd/waylandshader-niri-handoff.tar.gz \
     waylandshader-niri-handoff/import-session.py
   ```

   This reveals session material and the re-rooting logic. The importer requires the destination checkout to be on `niri-backend`, verifies the expected source session ID, gives the copied session a fresh identity when needed, updates its working directory, and prints an `omp --cwd ... --resume ...` command. It does not compile, install, or recover a native backend from a hidden source bundle.

5. **Optional, manual session recovery:** inspect the private content first; it contains conversation/tool records and must not be published wholesale. Extract a reviewed copy into a new private directory outside the checkout, then run the helper on that copy:

   ```sh
   umask 077
   mkdir -p "$HOME/waylandshader-handoff-review"
   tar -xzf /home/knd/waylandshader-niri-handoff.tar.gz \
     -C "$HOME/waylandshader-handoff-review"
   python3 "$HOME/waylandshader-handoff-review/waylandshader-niri-handoff/import-session.py" \
     /home/knd/AI/WaylandShader-niri
   ```

   The produced resume command is optional and starts a development conversation, not a compositor. Inspect the destination and use a fresh directory to avoid overwriting an earlier review. Historical `/tmp/...` and `/nix/store/...` references are evidence labels, not working installation paths. Preserve the original archive unchanged.

### 4.3 Establish the actual CachyOS baseline before future work

6. **On the laptop, collect read-only version and output facts:**

   ```sh
   niri --version
   pacman -Qo /usr/bin/niri
   pacman -Q mesa libdrm wayland
   niri msg --json outputs
   ```

   The ownership query identifies the actual package, which may not be named simply `niri`; inspect that package's metadata and build recipe next. `outputs` addresses the session selected by `NIRI_SOCKET`; do not confuse a nested compositor with the real desktop. These are evidence-collection instructions, not results obtained from the unavailable laptop.

7. **Pin the package source, not just a release name.** Retain the CachyOS/Arch PKGBUILD revision, upstream niri commit, downstream patches, Cargo.lock, pinned Smithay revision, enabled Cargo features, Rust toolchain, and relevant driver/library versions. Reproduce the installed distribution configuration before introducing the WaylandShader patch. The v26.04 reference is a research baseline, not an instruction to downgrade a rolling-release laptop.

8. **Stop before installation until the readiness gates below are satisfied.** No niri WaylandShader configuration keys or mutation commands are shipped in the inspected branch. Running `waylandshader-controller` today still exercises the KWin client. No amount of adding KWin plugin directories to niri's environment changes that.

For future builds, keep target directories, staging roots and output links outside these local `path:` flake inputs where practical. Documentation changes can change their NAR hashes too. Refresh only the changed named input before a later **manual** system rebuild; this documentation task does not update a lock file, host configuration or desktop. See [PROJECT-HANDBOOK.md](../PROJECT-HANDBOOK.md).

## 5. Successes, failures and limits

| Item | Evidence and outcome | What it does not prove |
| --- | --- | --- |
| Separate niri development line | **[SOURCE / HISTORICAL RECORD]** Worktree and branch published at the KWin baseline; closing session message `19f63597`. | Native niri source or installable support. |
| Renderer/lifecycle design | **[HISTORICAL RECORD]** `NiriRenderHooks.md` maps concrete pinned niri/Smithay seams; selected upstream files are also directly inspected for this chapter. | A compiled compositor patch. |
| Direct GLES runtime probe | **[HISTORICAL RECORD]** Initial temporal, multipass, LUT, mipmap, parameter/reset and ownership checks passed; later border test failed, session result `2ae3d1de`. | Full preset compatibility. A compilation success alone missed an observable semantic difference. |
| Desktop GL/EGLImage bridge | **[HISTORICAL RECORD]** Result `02481e59` reports the same checks plus clamp-to-border passing on Mesa 26.1.8, GLES 3.2 and desktop GL 4.6. Readback was explicitly used for verification. | Integration with niri's live contexts, synchronization under load, any other driver, physical output correctness, or measured performance. |
| First stock nested niri launch | **[HISTORICAL RECORD]** Result `deeb02b1`: X11 backend panicked during startup because `libXcursor` could not be loaded. | A shader bug; this was a development-environment failure before readiness. |
| Subsequent Wayland nesting | **[HISTORICAL RECORD]** Running niri/output IPC, screenshot request and embedded reference PNG; summary `502a6127` reports visual verification. The first marker wait timed out, so process creation alone was not treated as readiness. | Native DRM/CachyOS operation or shading inside niri; the running binary was stock Nixpkgs niri 26.04. |
| Inherited package build or `result` | **[SOURCE]** Derivation builds a KWin effect. Historical KWin verification belongs in the KDE chapter. | A niri package, port, or native shader test. |
| Damage-driven scheduling design | **[HISTORICAL RECORD / PROPOSED]** Later evaluation identifies false-static scanner cases and niri `NoDamage`/generation obligations. | An applied optimization or niri power savings. |
| Physical laptop support | **[UNVERIFIED]** A CachyOS/niri laptop was reported available, but no reachable host, package manifest, native shader build, or laptop result is supplied. | Any physical cursor, scanout, VRR, latency, power, rotation or multi-output claim. |

The later evaluation's missing-native-source conclusion remains correct. Its mention of a retained launch of a missing binary should be read alongside the recovered handoff: the archive establishes more than an unexplained process launch, including real compatibility probes and nested-session evidence, but expressly not an implemented port. Do not erase the successful groundwork, and do not promote it to completed support.

## 6. Proposed native implementation sequence and readiness gates

**Everything in this section is proposed.** Each stage names a deliverable and an observable gate. Commands for a nonexistent WaylandShader niri target would be fiction, so the native build/install commands must be added only with the actual source and package recipe.

### 1. Freeze a reproducible development baseline

Create a separate pinned niri source/package line using the matching CachyOS recipe and lockfile, or a deliberately isolated reference environment. Keep stock niri available. Record the WaylandShader patch revision separately from upstream and downstream niri revisions. Build output should identify all three. Use a package/staging name that does not overwrite the normal desktop binary or its session entry during development.

**Gate:** an unmodified reference niri can be built and launched in a disposable nested session with a known binary, isolated configuration, its own IPC socket, a reference client, and logs. This gate establishes the environment only.

### 2. Establish the runtime boundary with reproducible probe source

Recover/reconstruct the experimental probe from the archive into a maintained development artifact only when implementing the backend. Audit the private librashader lifetime patch and pin its exact base. Start from the desktop-GL/EGLImage route that preserved border semantics in the recorded experiment. Direct GLES remains a possible alternative if its full required behavior is fixed and verified, not silently weakened.

Specify texture format, orientation, color encoding, alpha/replacement semantics, mip-level requirements, EGL display/context compatibility, and both producer-to-consumer and consumer-to-producer fence ownership. An EGLImage is shared storage, not a readiness signal. Never delete a borrowed Smithay/client texture or mutate its sampler/mipmap state as if owned. Preserve the compositor's current context, framebuffer and GL state on every success/error path.

**Gate:** observable shader outputs match expected border sampling, mipmaps, LUTs, multipass, feedback/history, parameter changes and resets; resource ownership remains correct across repeated create/free cycles. Pass on the target driver as well as the reference environment. Reject unsupported capabilities explicitly instead of adding a silent CPU readback fallback.

### 3. Add one native final-output seam, then cover both presentation backends

Implement an output-sized framebuffer effect using pinned niri/Smithay conventions. Preserve the complete source under the effect and the original surface render states. Integrate before nested submission and before DRM frame queueing, not after presentation has already been scheduled. Define whether the cursor/drag icon is included. Prevent primary/overlay scanout from bypassing active whole-output processing; if the cursor is included, its plane must not bypass the filter either.

Use a simple visible deterministic transform to establish real composition before loading complex presets. Separate screenshot/screencast and transition snapshot policy from physical-output presentation; respect capture privacy rules rather than exporting an unrestricted output texture.

**Gate:** the compositor's actual presented output visibly changes, input still reaches original clients, normal frame callbacks/presentation continue, bypass restores ordinary presentation, and the nested and DRM paths exercise the same documented behavior. Geometry-changing shaders do not remap input; document that limitation.

### 4. Give each live output an explicit resource lifecycle

Own each chain, frame counter, feedback/history, input/output storage and effect damage identity per live output. Key GPU objects additionally by renderer/context generation. Keep reusable storage across frames; avoid reallocating stable-sized targets or rediscovering requirements in the frame loop.

Persist selection using niri's [output naming conventions][niri-output]: connector or case-insensitive `make model serial`, with explicit handling of unavailable or duplicate identity fields. A live IPC output ID is not a durable physical-monitor identity. KWin UUIDs and KConfig profiles cannot simply become niri identities. Preserve profiles over disconnection without allowing a stale asynchronous task to attach GPU objects to a different reconnected output.

Rebuild size-dependent resources and invalidate history on mode/size, scale, transform, renderer recreation and relevant resume changes. Stop output scheduling during inactivity/removal. Dispose GPU resources with the correct context before renderer teardown, after outstanding GPU use is complete; do not make an ownership cycle by storing a context owner in data shared by that context. Define a recoverable context-loss path instead of issuing destruction calls through an invalid context.

**Gate:** independent histories never cross monitors; hotplug during compilation, resize/scale/rotation, disable/re-enable, DPMS/session suspension, resume and renderer recreation leave neither stale pixels nor dangling resources.

### 5. Publish shader replacements atomically

Separate requested state from active state. Parse/classify/compile from the same immutable input snapshot or from compiled requirements, and publish a successful candidate only when its request generation and live output generations are still current. Use worker work only where context/thread ownership allows it. GL context currentness cannot be assumed transferable merely because a Rust value is movable.

Retain the old working runtime and its scheduling metadata when compilation fails, the request becomes stale, or an output disappears. Parameter changes during compilation must have defined ordering. Avoid replacing only half the selected outputs without an explicit supported policy. No-selected-output validation must not strand chains or trigger unnecessary output clocks.

**Gate:** valid replacement becomes visible; malformed/incompatible/stale replacement reports failure without removing the working effect; fast repeated requests and topology changes never publish old work or change an unrelated output.

### 6. Integrate damage and temporal scheduling together

Keep separate predicates for **processing is active** and **this runtime demands another frame without scene damage**. A static shader still processes client/compositor damage and can have nonlocal spatial dependencies; preserve full-output source/damage coverage until narrower behavior is proven.

Follow the evaluation's conservative rule: only a runtime proven non-temporal may stop its own continuous scheduling. Active frame/time uniforms, history and feedback dependencies need reliable compiled metadata. Unknown means continuous, not static. Source-file regex scans are not adequate: preprocessing, token-pasting and file replacement can invalidate them. Store requirements at load time with the published runtime rather than rescanning per frame.

Integrate temporal demand with each output's real/estimated VBlank scheduling. Do not replace niri's client, animation, cursor, lock, or frame-callback demand with a shader-only gate or a free-running global timer. A temporal tick must also advance the effect's commit/damage/cache generation, otherwise [DRM can return `NoDamage`][niri-tty] even after redraw is queued. Disabling the effect, parameter/color changes, errors, topology changes and lock transitions must explicitly invalidate presentation without waiting for incidental client activity. An optional force-continuous override must use the new backend's actual settings/control contract.

**Gate:** static output remains visibly filtered after local damage but stops only its own idle frame demand; temporal output animates on a static desktop; static → temporal → static and failed replacements preserve correct scheduling. Mixed refresh rates and another-output-only activity do not incorrectly share frame counters or scheduling ownership.

### 7. Make lock, failure and control behavior safe before daily use

Bypass processing while locking/locked and clear unlocked cached pixels/history across transitions, or establish equivalent isolation with explicit evidence. Lock completion must correspond to a newly safe presented frame, not successful reuse of an unlocked cache. Shader errors during lock must fall back to the compositor's safe lock scene, never the old filtered desktop.

Treat presets as user-supplied GPU programs with filesystem includes, allocations and compiler/driver exposure. Validate finite/ranged controls, enforce same-user/session transport access and bounded requests, and keep an accessible bypass path. Compilation off the main thread reduces stalls but does not contain a driver crash. A private graphics context isolates state, **not the compositor process or GPU failure domain**. Do not catch a panic/exception across FFI and pretend rendering is still safe.

Implement the chosen transport and a backend-aware client adapter. Define persistence precedence and atomic writes using niri/project-owned configuration, not writes to `kwinrc`. Preserve profiles when an output is absent, reject unsupported protocol versions clearly, and make error/status output useful for a bug report.

**Gate:** lock/unlock with temporal history and injected load/render failures never reveals pre-lock content; a unavailable backend is reported without touching KWin; UI edits affect only the chosen niri session/output and survive the documented restart/hotplug cases.

### 8. Package and enter native validation manually

Only after prior gates pass, produce a CachyOS package from the pinned recipe with a documented rollback path and coherent dependencies. Prefer an explicitly separate test session/package rather than overwriting `/usr/bin/niri` by hand. Do not perform partial rolling-distribution upgrades merely to keep an old compositor patch building.

**Manual, potentially session-ending:** save work before logging out and selecting a test session, or move to a separate TTY according to the package's actual instructions. Never replace or restart the live Wayland compositor. Keep a known-good stock session and package available. Package activation/session selection are user-operated steps, not actions performed by this documentation task.

**Gate:** native logs and visible output identify the patched binary and revision, not merely a stock executable with the same version. Repeat physical-output validation after matching-version package updates. Publish exact install/use commands only with that package and the verified control schema.

## 7. Validation plan and future refinements

The minimum future evidence is a reproducible source revision plus observable output, not just a successful build. Keep the following checks distinct:

| Validation layer | Required observations |
| --- | --- |
| Runtime interoperability | Border sampling, mips/LUTs, temporal history/feedback, output independence, parameter/reset semantics, ownership and synchronization on the target driver. |
| Nested native integration | Actual postprocessed scene and cursor policy, bypass, controls, replacement failure, resize/scale, input delivery and normal callback/presentation behavior. A stock reference window is insufficient. |
| Native DRM | Mode/scale/transform, hotplug, DPMS/resume, lock isolation, direct-scanout/plane policy, multiple outputs and renderer teardown. |
| Scheduling | Static/temporal transitions, no-selected-output changes, stale loads, damage propagation for nonlocal shaders, real/estimated VBlank, frame-callback-only activity and mixed monitor refresh. |
| Capture/privacy | Documented filtered/unfiltered capture behavior, no double processing of transitions, no unlocked history entering a lock frame or capture. |
| Hardware performance | Separately measure latency, power, VRR behavior, GPU/CPU load, allocations and multi-GPU transfers with method, versions, preset and baseline recorded. No numbers are supplied by this chapter. |
| Distribution lifecycle | Clean package installation in a test environment, configuration migration/precedence, rollback to stock, and rebuild/retest against the next pinned package set. |

Refinements should follow evidence: trustworthy compiled temporal metadata before idle scheduling optimization; direct GLES only after semantic parity is established; smaller damage regions only for proven spatially local processing; mipmap changes only from actual pass requirements; filtered capture only with privacy and temporal-state isolation. HDR/wide-gamut handling must be designed explicitly rather than inherited from a KWin-specific SDR conversion. Per-window filtering, separate presets per output and input-coordinate remapping are separate features, not implied by a whole-output backend.

The handoff's GPU bridge result is promising but driver-specific. It gives a concrete starting point for these gates, not a performance claim or a guarantee that a compositor-integrated bridge will work unchanged.

## 8. Maintenance, patch and ABI rules

- **Maintain a small pinned source patch, if that route is chosen.** Track upstream niri, downstream CachyOS changes, Smithay, Cargo.lock, librashader and the WaylandShader patch as separate revisions. There is no KWin-like binary effect ABI to rely on here. A clean textual rebase does not prove renderer contracts survived.
- **Re-review the dangerous seams on every rebase:** framebuffer capture/draw order, effect commit/cache behavior, original surface states, DRM `NoDamage`, actual/estimated VBlank continuation, scanout/cursor policy, screenshots/transitions, lock completion and renderer/output teardown. Review both nested and native paths.
- **Do not carry KWin ABI assumptions forward.** KWin plugin metadata, private headers, `org.kde.KWin`, output UUIDs, `kwinrc` and KConfig are backend-specific. Reuse tested shader/control semantics and fixtures; extract implementation only after two real backends demonstrate what is common.
- **Keep GPU ownership explicit across Rust/FFI.** Document who creates, borrows, waits on and destroys every texture, fence, EGLImage and context. No unwinding across C ABI; errors must preserve safe state. Avoid unnecessary per-frame allocation/copies and context-state guessing.
- **Keep the private runtime genuinely private.** The existing `default.nix` requires librashader 0.10.1 and gives its patched library a private name. Do not replace a distribution's shared librashader in order to make the experiment run. A Rust-crate route needs equally explicit version/patch pinning and review of which fixes are still required.
- **Treat configuration and IPC as maintained contracts.** State backend/version/capabilities, document profile identity and ambiguous-monitor behavior, reject incompatible schemas, and migrate deliberately. Never silently reinterpret a KWin UUID as a niri connector or persistent IPC ID.
- **Preserve a recoverable package strategy.** Rolling CachyOS updates can move niri and Smithay together. Keep a coherent tested package set, rebase against its actual source, and provide stock rollback instead of freezing one package indefinitely or overwriting the live compositor.
- **Record licensing at the correct boundary.** WaylandShader's inherited license is MIT, Copyright (c) 2026 man33li. The inspected niri manifest declares GPL-3.0-or-later. Distribution of a modified compositor must respect upstream and dependency licenses; do not relabel the combined niri fork as MIT-only.

The [project handbook](../PROJECT-HANDBOOK.md) owns the general patch workflow, documentation maintenance, local flake-input handling and shared language rationale. These niri-specific rules supplement it.

## 9. Evidence needed to advance the status

To change “native backend not implemented in available source” to an implemented status, provide:

1. A reachable commit or reviewed source archive containing actual native niri integration, not cached upstream files, a branch name, or a launch log.
2. The upstream niri commit, downstream package recipe/patches, Smithay revision, Cargo.lock/features/toolchain and private shader-runtime revision/patches used to build it.
3. Reproducible build/package instructions and binary provenance linking the executable to that patched source.
4. The niri-specific control/configuration implementation and its documented session/output identity contract.
5. Logs and visible evidence of a shader applied inside that compositor, including a working preset, bypass and failed replacement; separate results for nested and native DRM.
6. CachyOS machine/package/driver facts and results for lifecycle, lock safety, output resources, damage/temporal scheduling and scanout/cursor policy.
7. Original measurements and methodology before any niri latency, power, VRR or performance claim.

The available handoff can advance **groundwork documented** immediately; it cannot satisfy items 1–6. Its embedded experimental sources and screenshot make parts of the historical work auditable, but the removed temporary environment and unavailable laptop still require fresh, separately authorized implementation and validation.

## 10. Evidence and further reading

### Repository evidence

- [Pinned inherited implementation at `6579b80`](https://github.com/man33li/Wayland-Shader-KDE/tree/6579b80e3cc7dd04ae3fd04f74c27fca5c8136bd), and [published `niri-backend`](https://github.com/man33li/Wayland-Shader-KDE/tree/niri-backend). Prefer the commit link for historical claims.
- `main`: `performance-enhancer-evaluation.md`, especially “niri assessment and source-state correction,” compiled-runtime classification requirements, and the verification boundary. This is a source path rather than a cross-branch relative link.
- [KDE chapter](KDE.md), [GNOME chapter](GNOME.md), and [shared handbook](../PROJECT-HANDBOOK.md). GNOME's local implementation is a separate development line; its existence does not change niri's source state.

### Private archive citations

The local archive `/home/knd/waylandshader-niri-handoff.tar.gz` is not a public download link. After private extraction, paths below are relative to `waylandshader-niri-handoff/`. Do not upload the session unredacted.

| Artifact or `session.jsonl` entry ID | Evidence |
| --- | --- |
| `import-session.py` | Branch guard, session identity/cwd rewrite, printed resume command. |
| `session/NiriRenderHooks.md` | Pinned renderer seam, lifecycle, presentation, scanout and multi-GPU research. |
| `session/NiriCosmicRoutes.md` | Capture/overlay limitations and native integration rationale. |
| `session/ShaderInterop.md` | Runtime interoperability source research; distinguish it from Main's later probe results. |
| `54a01fba`, `d6da2a37` | Embedded probe source and temporary native reference/configuration writes. |
| `d6001c1b` | Probe invocation and stock niri nested launch specification. |
| `deeb02b1` | X11 nesting failure with missing `libXcursor`. |
| `2ae3d1de` | Direct GLES clamp-to-border failure, 2026-09-10. |
| `02481e59` | Desktop GL/EGLImage bridge output and explicit passing checks, 2026-09-10. |
| `5e88b44f` and its following tool-result message | Reference PNG read request and embedded image. |
| `502a6127` | Final architecture recommendation explicitly qualifying the work as probes, not a shader-enabled niri build. |
| `19f63597` | Published branch confirmed as KWin baseline, not native niri implementation. |

### Primary upstream references

All implementation references below are pinned to the inspected niri/Smithay comparison unless marked as an evolving discussion.

- [niri README at the reference commit][niri-readme] and [Cargo.toml][niri-cargo].
- [Framebuffer-effect precedent][niri-fb], [renderer traits][niri-renderer], [output-name matching][niri-output], [nested backend][niri-winit], [DRM backend][niri-tty], and [central scene/lifecycle code][niri-core].
- [Smithay element contract][smithay-element], [damage tracker][smithay-damage], [EGL context implementation][smithay-egl], and [multi-GPU renderer][smithay-multi].
- [Long-running custom shaders, niri issue 913][niri-913] is an evolving upstream discussion, not a versioned feature guarantee.

[niri-readme]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/README.md
[niri-cargo]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/Cargo.toml
[niri-renderer]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/render_helpers/renderer.rs
[niri-fb]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/render_helpers/framebuffer_effect.rs
[niri-output]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/niri-config/src/output.rs
[niri-winit]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/backend/winit.rs
[niri-tty]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/backend/tty.rs
[niri-core]: https://github.com/niri-wm/niri/blob/8ed0da44d974c32c6877d2f4630c314da0717ecb/src/niri.rs
[niri-913]: https://github.com/niri-wm/niri/issues/913
[smithay-element]: https://github.com/Smithay/smithay/blob/ff5fa7df392cecfba049ffed55cdaa4e98a8e7ef/src/backend/renderer/element/mod.rs
[smithay-damage]: https://github.com/Smithay/smithay/blob/ff5fa7df392cecfba049ffed55cdaa4e98a8e7ef/src/backend/renderer/damage/mod.rs
[smithay-egl]: https://github.com/Smithay/smithay/blob/ff5fa7df392cecfba049ffed55cdaa4e98a8e7ef/src/backend/egl/context.rs
[smithay-multi]: https://github.com/Smithay/smithay/blob/ff5fa7df392cecfba049ffed55cdaa4e98a8e7ef/src/backend/renderer/multigpu/mod.rs
