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

The unit suite does not require an audio server. The tray integration test runs the real tray
binary on a private session bus and checks the StatusNotifierItem/DBusMenu contract (signatures,
root item, `AboutToShow`, `GetGroupProperties` and click handling):

```bash
dbus-run-session -- build/pipeduck_tray_tests build/pipeduck
# ctest runs it automatically when dbus-run-session is installed:
ctest --test-dir build --output-on-failure
```

To test against a live PipeWire session safely, use a temporary prefix, config directory and a
separate null sink. The recipe below never touches your real output or saved configuration:

```bash
export PIPEDUCK_TEST_ROOT=$(mktemp -d)
mkdir -p "$PIPEDUCK_TEST_ROOT/config"
null_module=$(pactl load-module module-null-sink sink_name=pipeduck-itest \
  sink_properties=device.description=pipeduck-itest)

# Make the test sink the default only for this shell.
pactl set-default-sink pipeduck-itest

# Generate a quiet test tone into the test sink for a few seconds.
# ffmpeg is only needed for this diagnostic; it is not a PipeDuck dependency.
ffmpeg -f lavfi -i sine=frequency=440:duration=5 -f pulse default pipeduck-itest-tone &

XDG_CONFIG_HOME="$PIPEDUCK_TEST_ROOT/config" \
  build/pipeduck init-config
XDG_CONFIG_HOME="$PIPEDUCK_TEST_ROOT/config" \
  build/pipeduck sources
XDG_CONFIG_HOME="$PIPEDUCK_TEST_ROOT/config" \
  build/pipeduck route-once --seconds 3 --yes-really-route

# Clean up.
pactl unload-module "$null_module"
rm -rf "$PIPEDUCK_TEST_ROOT"
```

Run the install smoke test after changing CMake install rules:

```bash
prefix=$(mktemp -d)
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build build -j"$(nproc)"
cmake --install build
tests/install_smoke.sh "$prefix"
rm -rf "$prefix"
```

For deterministic sanitizer runs, use separate build directories:

```bash
cmake -S . -B build-asan -DPIPEDUCK_GUI=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build-asan -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure -R '^pipeduck_tests$'
```

Test the following before a routing change:

1. The selected call stream is excluded and stays on its original sink.
2. Other streams move to the `pipeduck-*` sink.
3. Speech activity ducks and idle speech restores volume smoothly.
4. Stopping the process restores every stream to its original sink and leaves a default-sink switch
   made during the session intact.
5. Killing a playback client does not crash the ducking worker.
6. With `release_fade_ms` set to 1500-3000 and the `mic` trigger, speak again during the return
   fade. The volume must start decreasing from its current value instead of continuing to rise.

## Pull requests

- Keep changes focused and explain the user-visible effect.
- Update README/config documentation when adding options.
- Make sure the CI build (GCC and Clang), sanitizer jobs, install smoke tests, tests and formatting
  pass.
- Commits should have clear messages. Squashing is fine.

## Release checklist

Version numbers live in three places:

- `project(... VERSION ...)` in `CMakeLists.txt`;
- the version and date in the `.TH` line of `docs/pipeduck.1`;
- the newest entry in `CHANGELOG.md`.

To publish a release:

1. Bump all three versions in one commit and confirm the changelog describes user-visible changes.
2. Run the local checks: GCC/Clang builds with GUI ON and OFF, `ctest`, the install smoke test and
   at least the ASan+UBSan unit run.
3. Create an annotated tag: `git tag -a vX.Y.Z -m "PipeDuck X.Y.Z"`.
4. Push the commit and tag: `git push origin main --follow-tags` (or push the tag explicitly).
5. The `Release` workflow checks out the tag, builds/tests both GUI configurations, runs the install
   smoke test, creates `pipeduck-X.Y.Z.tar.gz` plus a SHA-256 sidecar, and only then publishes the
   GitHub release.
6. Verify that the release contains the archive and checksum and that `sha256sum -c` passes after
   downloading it.

Distribution-specific packages are optional follow-up work; the release publishes source artifacts
only.

## License

By contributing you agree that your contribution is licensed under the MIT License, the same as
this project.
