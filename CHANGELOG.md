# Changelog

All notable changes to PipeDuck are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-10-19

### Added

- Automatic ducking of non-call playback on PipeWire through a temporary virtual sink.
- Voice-call stream selection and exclusion.
- Microphone-triggered ducking with `call`, `mic` and `both` trigger modes.
- Smoothstep fade-down and fade-back volume ramps, including a configurable smooth return.
- Native libpulse volume control with `pactl` fallback.
- StatusNotifierItem tray with live state, source/mic/trigger menus and a toggle.
- Terminal and GTK4 tuners with live config reload.
- CLI: `tray`, `status`, `sources`, `select-source`, `mic-sources`, `select-mic`, `tune`,
  `tune-gui`, `route`, `route-once`, `config-path`, `init-config`.
- TOML configuration compatible with pw-duck's schema, extended with trigger and fade options.
- Safe teardown which leaves the virtual sink alive if streams cannot be moved away.
- Single-instance tray lock.
- CMake install rules, desktop entry, hicolor icons and man page.
- Unit tests and GitHub Actions CI.

### Fixed

- Tray right-click menu implemented as a single DBusMenu root item with the full `(iias)`
  `GetLayout` signature. This matches libdbusmenu/ksni and GNOME's AppIndicator extension, so the
  context menu opens reliably and exposes source, microphone, trigger, tuner and quit controls.
- `AboutToShow` returns `false` like ksni/Qt. GNOME otherwise rebuilds the whole menu while a
  submenu is opening, which immediately closed the Trigger and Source submenus.

### Credits

- Based on [pw-duck](https://github.com/geri1701/pw-duck) by Gerhard Schwanzer (MIT).
