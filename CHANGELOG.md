# Changelog

All notable changes to PipeDuck are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed

- The shared PulseAudio graph cache reconnects after a failed or lost libpulse connection instead
  of staying unavailable until the tray restarts. This is what made ducking sit in "waiting" while
  source menus kept working through the `pactl` fallback (#36).
- Repeated `Activate` calls from a double click are coalesced, `Scroll` no longer toggles ducking,
  and `SecondaryActivate` no longer acts as a toggle (#39).
- Launching `pipeduck` (or the desktop icon) while the tray is running now forwards activation to
  the running instance instead of exiting silently (#40).
- Trigger, source, microphone and profile menus stay editable while ducking waits for its
  configured source, so a lost stream can be replaced without stopping ducking (#38).
- An incomplete restore no longer disables the Ducking toggle permanently: activating it moves
  streams off abandoned pipeduck sinks and starts a fresh session (#37).

## [1.2.0] - 2026-10-21

### Added

- Named ducking profiles stored in `[profiles.<name>]` tables, with `profiles`, `profile-save`,
  `profile-select` and `profile-delete` CLI commands and a tray Profile menu.
- The tray locks trigger, source, microphone and profile editing while ducking is running and
  explains how to unlock it.

### Fixed

- The ducking control tick never spawns a `pactl` fallback or blocks on an unavailable audio
  backend. Monitor-volume updates use a bounded exponential backoff and recreate the libpulse
  context when the backend returns.
- The control loop detects a missing virtual sink or output within the refresh interval, shows a
  recovering/degraded state instead of `ON`, and rebuilds the route with backoff.

## [1.1.0] - 2026-10-19

### Added

- Group playback streams by application and microphones by device in the tray menus.
- Refresh the source and microphone menus automatically when streams, microphones or the
  configuration change.
- Separate `mic_vad_threshold` setting, CLI override and tuner row for microphone-triggered
  ducking.
- DBusMenu/StatusNotifierItem integration test that runs under `dbus-run-session`.

### Changed

- PulseAudio/PipeWire state is read from a single libpulse event subscription instead of polling
  `pactl`, so the ducking loop and tray menus no longer spawn processes in steady state.
- Volume fades are non-blocking and driven by the control tick. Speech during a return fade now
  cancels it and fades down from the current volume.
- Ducking follows default-output changes while active.

### Fixed

- Stopping ducking no longer overwrites a default sink change made by the user while it was
  running.
- Microphone sensitivity can be tuned independently from call sensitivity.

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
- Original PipeDuck logo, tray mark, active-state icon and full hicolor/symbolic icon set.

### Fixed

- Tray right-click menu implemented as a single DBusMenu root item with the full `(iias)`
  `GetLayout` signature. This matches libdbusmenu/ksni and GNOME's AppIndicator extension, so the
  context menu opens reliably and exposes source, microphone, trigger, tuner and quit controls.
- `AboutToShow` returns `false` like ksni/Qt. GNOME otherwise rebuilds the whole menu while a
  submenu is opening, which immediately closed the Trigger and Source submenus.

### Credits

- Based on [pw-duck](https://github.com/geri1701/pw-duck) by Gerhard Schwanzer (MIT).
