# WaylandShader TODO

Current implementation: native KWin 6.6, with one shared Slang preset, independent monitor selection, and per-monitor gamma/saturation controls.

## Install and verify the KWin update

- [ ] Let the current `glf-switch` finish successfully.
- [ ] Log out of Plasma and back in to load the updated KWin plugin.
- [ ] Check the monitor selector, per-monitor shader and color toggles, and gamma/saturation controls on physical monitors.

## Native backends

- [ ] **niri — recommended next:** implement and test native compositor integration on the CachyOS/niri laptop. This requires a backend, not just different packaging.
- [ ] **GNOME:** implement native GNOME Shell/Mutter integration.
- [ ] **COSMIC:** implement native COSMIC compositor integration.

## Further validation

- [ ] Validate HDR and wide-gamut behavior. The current shader pipeline is SDR sRGB, not HDR-preserving.
- [ ] Validate physical monitor rotation.
- [ ] Measure latency on physical displays with representative shader presets.
- [ ] Measure idle and active power consumption with representative shader presets.
