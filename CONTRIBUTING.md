# Contributing to PipeDuck

Thanks for helping improve PipeDuck. Bug reports, feature ideas, documentation fixes and pull
requests are all welcome.

## Reporting bugs

Please include:

- Your distribution and PipeWire/WirePlumber versions (`pipewire --version`, `pactl info`).
- Whether the bug affects call ducking, microphone ducking, the tray, a tuner or CLI routing.
- The contents of `~/.config/pipeduck/config.toml` with any private stream names redacted.
- Output from `pipeduck status` and, for routing problems, `pipeduck sources`.
- Relevant log output. Running `pipeduck route --yes-really-route` in a terminal prints VAD and
  routing events that are useful for diagnosis.

If the bug is a security concern, do not open a public issue; see [SECURITY.md](SECURITY.md).

## Development setup

```bash
git clone https://github.com/ErRusso/pipeduck.git
cd pipeduck
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

To skip the GTK4 tuner:

```bash
cmake -S . -B build -DPIPEDUCK_GUI=OFF
```

## Coding style

- C++20, four-space indentation, 100-column lines.
- Format with `clang-format -i` before committing. The repository contains a `.clang-format`.
- Prefer RAII and standard containers. Avoid raw `new`/`delete`.
- Keep audio-graph changes in `routing.cpp` and keep teardown safe: never destroy the virtual sink
  while streams are still attached to it.
- Add or update unit tests for pure logic changes (VAD, config, identity matching, port pairing,
  ramps).

## Regenerating icons

The master logo is `assets/icons/source-pipeduck.png` (1024x1024). After changing it, regenerate
the hicolor application icons, symbolic icons and StatusNotifierItem ARGB pixmaps:

```bash
python3 -m pip install pillow
scripts/generate-icons.py
```

The script writes:

- `assets/icons/hicolor/<size>/apps/pipeduck.png`
- `assets/icons/hicolor/<size>/apps/pipeduck-symbolic.png`
- `assets/icons/pixmap/pipeduck-<size>.argb`
- `assets/icons/pixmap/pipeduck-on-<size>.argb`
- `assets/icons/source-pipeduck-mark.png` and `source-pipeduck-on.png`

Commit the generated assets together with the master. CMake embeds the ARGB pixmaps at build time.

## Integration testing

The unit suite does not require an audio server. To test against a live PipeWire session safely,
create an isolated null sink instead of using your real output:

```bash
pactl load-module module-null-sink sink_name=pipeduck-itest \
  sink_properties=device.description=pipeduck-itest
# play a test tone into it and use a temporary XDG_CONFIG_HOME:
XDG_CONFIG_HOME=/tmp/pipeduck-test build/pipeduck sources
```

Test the following before a routing change:

1. The selected call stream is excluded and stays on its original sink.
2. Other streams move to the `pipeduck-*` sink.
3. Speech activity ducks and idle speech restores volume smoothly.
4. Stopping the process restores every stream to its original sink.
5. Killing a playback client does not crash the ducking worker.

## Pull requests

- Keep changes focused and explain the user-visible effect.
- Update README/config documentation when adding options.
- Make sure the CI build (GCC and Clang), tests and formatting pass.
- Commits should have clear messages. Squashing is fine.

## License

By contributing you agree that your contribution is licensed under the MIT License, the same as
this project.
