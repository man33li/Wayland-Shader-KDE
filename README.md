# WaylandShader

Native RetroArch Slang shader effects for the Wayland desktop, with per-monitor shader selection, gamma, and saturation controls.

WaylandShader processes the desktop **inside the compositor**. It is not a Wine application, a screen-capture viewer, or an overlay window. The current implementation is a **KDE Plasma / KWin effect**.

[Detailed guide](WaylandShader-Guide.txt) · [Project roadmap](TODO.md)

## Current status

**Version 0.1.0: working KWin backend; other compositor backends are not implemented.**

| Desktop / compositor | Status |
| --- | --- |
| KDE Plasma / KWin 6.6 | Implemented, with native GUI, CLI, per-monitor controls, and persistent settings |
| niri | Planned; recommended next backend, with a CachyOS/niri laptop available for testing |
| GNOME Shell / Mutter | Planned; requires separate native integration |
| COSMIC | Planned; requires separate native integration |

The KWin implementation has passed real OpenGL regressions and isolated, two-output native compositor checks. Broader physical-monitor validation, HDR/wide-gamut behavior, physical rotation, and latency/power measurements remain on the [roadmap](TODO.md).

## Features

- Load real `.slangp` presets through librashader, including multipass shaders, lookup textures, mipmaps, feedback, and previous-frame history.
- Choose which monitors receive the preset. Each selected output has its own filter chain, frame counter, and history.
- Adjust **gamma and saturation independently per monitor**, with a separate color-adjustment toggle. Color adjustments work without a preset or with that monitor's shader processing disabled.
- Use a native Qt settings window or a JSON-printing CLI backed by asynchronous D-Bus calls.
- Edit declared shader parameters at runtime. The preset and its parameters are shared across selected monitors; monitor selection and color settings are independent.
- Persist the preset, parameter overrides, master enablement, and monitor profiles keyed by KWin output UUID.
- Compile replacement presets on a worker thread while retaining the working preset. A failed load does not replace it.
- Handle output resizing, fractional scaling, and output removal/reconnection.

## Tech stack

| Layer | Technology |
| --- | --- |
| Application and effect code | C++20 |
| Desktop integration | KWin 6.6 native effect API; KDE Frameworks 6 CoreAddons and Config |
| Settings UI and control | Qt 6.10+ Core, Gui, Widgets, DBus, and Concurrent; Qt private development headers |
| GPU rendering | Desktop OpenGL/EGL, libepoxy, and KWin's color-managed render targets |
| Shader runtime | [librashader](https://github.com/SnowflakePowered/librashader) 0.10.1, a Rust runtime consumed through its C API |
| Build system | CMake 3.25+, Extra CMake Modules 6.22+, Ninja, and pkg-config |
| Packaging and development environment | Nix / NixOS through [default.nix](default.nix) and [shell.nix](shell.nix) |

**Compatibility matters:** KWin's effect API is private and not binary-stable. Build against the same KWin package set as the installed compositor. The build currently rejects KWin 6.7 and newer.

The Nix package applies a [librashader resource-lifetime and temporal-buffer patch](patches/librashader-gl-lifetime.patch). It installs a private `libwaylandshader-rashader.so.2` rather than replacing the system's librashader. The patch is restricted to **librashader 0.10.1**; an unpatched system library is not a drop-in build dependency.

## How it works

```text
KWin desktop composition
    -> per-output sRGB intermediate
    -> optional Slang preset through librashader
    -> optional per-monitor gamma and saturation
    -> color-managed presentation into KWin's render target
```

There is no PipeWire capture stream, portal permission, capture feedback loop, or CPU readback in the rendering path. Private shared EGL contexts isolate librashader's GL state from KWin, and GPU fences synchronize shared textures.

Application input remains with the original windows. Shader geometry changes affect pixels, not mouse hit-testing.

## Build and install

### Build the Nix package

From the repository root:

```bash
nix-build
```

The `result` symlink contains the KWin plugin, `waylandshader-controller`, `waylandshaderctl`, and the detailed guide. **Building alone does not install or activate the plugin in your desktop session.**

`default.nix` and `shell.nix` default to `<nixpkgs>`. Supply the matching `pkgs` when your system uses a different package set. Nix is the provided packaging path; standalone packaging for other distributions is not included.

### Install on NixOS

Import the package using your system configuration's existing `pkgs`. For a non-flake configuration, add:

```nix
environment.systemPackages = [
  (import /absolute/path/to/WaylandShader/default.nix { inherit pkgs; })
];
```

For a flake-based configuration, declare the checkout as a **non-flake input** in your system flake:

```nix
inputs.waylandshader = {
  url = "path:/absolute/path/to/WaylandShader";
  flake = false;
};
```

Add `waylandshader` to your flake's `outputs` arguments and pass it to the NixOS module, for example through `specialArgs = { inherit waylandshader; };`. The module can then install it with:

```nix
{ pkgs, waylandshader, ... }:
{
  environment.systemPackages = [
    (import "${waylandshader}/default.nix" { inherit pkgs; })
  ];
}
```

Rebuild and switch using your normal NixOS system command, then **save your work, log out of Plasma, and log back in**. This lets KWin discover and load the installed plugin. Do not replace or restart the live Wayland compositor to test it.

After changing a local checkout used as a flake input, refresh that input before rebuilding. For a system flake at `/etc/nixos`:

```bash
sudo nix flake update waylandshader --flake /etc/nixos
```

This refreshes only the named input; follow it with your normal system rebuild/switch command.

## Usage

### Settings application

Launch **WaylandShader** from Plasma's application menu or run:

```bash
waylandshader-controller
```

1. Choose a `.slangp` preset and load it, or use color adjustments without a preset.
2. Enable **desktop effects** with the master checkbox.
3. Choose a monitor in the **Monitor** selector.
4. Toggle **Apply preset on this monitor** and **Enable color adjustments** independently.
5. Adjust the monitor's gamma/saturation or the preset's shared parameters.

The master toggle bypasses both shader and color processing without discarding settings.

| Control | Range | Meaning |
| --- | --- | --- |
| Gamma | `0.1`–`5.0` | `1.0` is neutral; higher values brighten, lower values darken |
| Saturation | `0.0`–`2.0` | `0.0` is grayscale; `1.0` is neutral |

Color adjustments run after the preset and before KWin's output color conversion. They are software SDR adjustments, not ICC calibration or hardware gamma-ramp controls.

Presets are **not bundled**. The [libretro Slang shader collection](https://github.com/libretro/slang-shaders) is one source. Keep each preset's shader files, includes, and lookup textures in their original relative directory layout.

### Command line

```bash
waylandshaderctl load /absolute/path/to/preset.slangp
waylandshaderctl enable
waylandshaderctl status
waylandshaderctl outputs

waylandshaderctl output DP-1 shader off
waylandshaderctl output DP-1 color on
waylandshaderctl output DP-1 gamma 1.1
waylandshaderctl output DP-1 saturation 0.8

waylandshaderctl parameters
waylandshaderctl param PARAMETER_NAME 0.5
waylandshaderctl disable
```

Replace `DP-1` with an ID or unique exact name returned by `outputs`. Replace `PARAMETER_NAME` and its value with a parameter and bounds returned by `parameters`.

`status`, `outputs`, and `parameters` are read-only and do not load a missing effect. Mutation commands may load the installed WaylandShader effect. Loading a preset preserves master enablement; use `enable` when needed. See `waylandshaderctl --help` and the [guide](WaylandShader-Guide.txt) for timeout, exit-code, and status-schema details.

Settings are stored under `$XDG_CONFIG_HOME`, normally `~/.config`: `waylandshaderrc` holds the project settings, and `kwinrc` holds the plugin enablement preference.

## Development and verification

```bash
nix-shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" \
  -DWAYLANDSHADER_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The optional [runtime tests](tests/shader_runtime_test.cpp) execute real OpenGL rendering. They cover multipass/LUT/mipmap results, independent output histories, feedback/history resets, live parameter changes, shared-context state restoration, and an OpenGL 3.3 fallback. Mesa surfaceless EGL is required; its absence is reported as a skip. These tests do not load a plugin into your desktop.

Recorded verification includes a successful Nix package build and **3/3 passing CTest checks**. Separate native checks used an isolated two-output KWin 6.6.6 session on Mesa with an AMD Radeon RX 6800 XT. They exercised real GUI input, per-monitor RGB transformations, restart/hotplug persistence, monitor rearrangement, and rejection of broken replacement presets. See the [detailed verification record](WaylandShader-Guide.txt).

C++ sources use clang-format's **WebKit** style. When reporting a rendering problem, include the compositor version, GPU/driver, preset and relevant settings, reproduction steps, and `waylandshaderctl status` output.

### Source layout

| Path | Contents |
| --- | --- |
| [src/effect.cpp](src/effect.cpp) | KWin integration, per-output processing, persistence, and D-Bus service |
| [src/shader_runtime.cpp](src/shader_runtime.cpp) | librashader runtime and shared EGL context handling |
| [src/controller_main.cpp](src/controller_main.cpp) | Native Qt settings application |
| [src/controller_client.cpp](src/controller_client.cpp) / [src/ctl_main.cpp](src/ctl_main.cpp) | Asynchronous control client and CLI |
| [tests/](tests/) | Real-GL regressions and shader fixtures |
| [patches/](patches/) | Private librashader dependency patch |

## Limitations and safety

- Only **KWin 6.6 on Wayland** is implemented. Installing this package does not add support to niri, GNOME, or COSMIC.
- Processing applies to whole selected outputs, not individual windows or selected regions. Different presets per monitor are not supported.
- The shader input is **SDR sRGB**, not an HDR-preserving pipeline. HDR/wide-gamut correctness and physical monitor rotation remain unvalidated.
- Curvature and other geometry effects do not remap application input coordinates. Prefer non-warping presets for precise desktop interaction.
- A cursor drawn separately from the scene is not necessarily shaded.
- Slang processing continuously repaints selected outputs for temporal effects, including otherwise idle desktops. Color-only processing does not request continuous animation. Processing adds GPU work and can prevent direct scanout; latency, power, VRR behavior, and universal preset compatibility are not guaranteed.
- Rendering runs inside KWin. Native plugin or driver faults can affect the session, and expensive presets can make it slow. Test unfamiliar presets or backend changes in an isolated nested session before daily use.

## Roadmap

See [TODO.md](TODO.md) for the remaining installation checks, native niri/GNOME/COSMIC backends, and hardware validation. The next recommended backend is niri, using the available CachyOS/niri test system; each compositor requires its own native integration.

## License

A project license has not yet been declared, and no `LICENSE` file is currently included. Third-party dependencies and external shader presets retain their respective upstream licenses.
