# Running the Android build

## 1. Build

```bash
cd android
./scripts/build_apk.sh                     # all ABIs
./scripts/build_apk.sh arm64-v8a           # or just one
```

The script stages the engine sources, builds `libsilentstorm.so` with the NDK,
and packages a signed APK at `build/apk/silentstorm.apk` using the SDK
build-tools directly. A debug keystore is generated on first run.

Set `ANDROID_SDK_ROOT` / `ANDROID_NDK_HOME` if they are not in the usual place.
With several devices attached, set `ANDROID_SERIAL` to pick one.
Native libraries are rebuilt before packaging, including when a `.so` already
exists. Use `A5_SKIP_BUILD=1` only immediately after a successful native build
if you want to package those exact libraries without staging/building again.

## 2. Game data

The port does not ship game data. Point it at your own copy of the game files —
the same `Complete/` layout the desktop game uses.

Data goes in the app's own external files directory:

```
/sdcard/Android/data/org.silentstorm.port/files/SilentStorm/
    Globals.res  Chapters.res  Terrain.res  Buildings.res  Waypoints.res
    Textures/  Geometries/  Animations/  Sounds/  Scripts/  Fonts/  ...
    game.db
```

That location needs no runtime permission and is removed when the app is
uninstalled. The port also looks in `/sdcard/SilentStorm` and
`/data/local/tmp/SilentStorm`, and reports every location it tried when it finds
nothing.

**Copying with adb:**

```bash
./scripts/push_data.sh          # a ~5 MB subset: packages, scripts, small assets
./scripts/push_data.sh --full   # everything in Complete/ (~3.7 GB, slow)
./scripts/push_data.sh --from /path/to/your/install
```

`adb push` creates directories owned by `shell` with mode 0770, which the app
cannot enter, so the script relaxes permissions afterwards. Copying the files
with a file manager or over MTP produces correctly-owned files and needs none of
that.

Path handling is case-insensitive: the game data was authored on Windows and the
engine asks for paths like `Textures\1234`, so the compat layer translates
separators and falls back to a case-insensitive lookup per path component.

## 3. Run

```bash
./scripts/run.sh          # install, launch, print the engine's report
./scripts/run.sh --build  # build first
```

or open the app from the launcher. Drag to scroll the report.

Everything on screen is also in logcat:

```bash
adb logcat -s SilentStorm
```

The game targets 30 FPS using display-vsync callbacks; it stops stepping while
paused by Android or without window focus. Every five seconds, `perf:` reports
observed FPS, frame-interval p95/max, game-thread wall time (excluding swap), swap time,
draws, missing-program draws, GL errors, and buffer uploads in MiB/frame. A load is included in these timings;
measure steady play separately from startup. The first window also contains the
self-test's two deliberately rejected missing-shader draws.

For repeatable mission checks, put `template 4414` in `SilentStorm/mission.cfg`
and `A5_START_CFG=mission.cfg` in the sibling `files/env.txt`. Restore the previous
environment after testing. Unlock the device and keep the app in the foreground;
FPS while a lock screen or screensaver owns focus is not a gameplay measurement.
Check camera movement, unpaused play, and background/return as well as a still
frame. A passing shader test does not establish correctness of the whole scene.

Camera controls: drag with two fingers to pan, pinch to zoom, and twist the
same two fingers to rotate; the motions can be combined. Three-finger drag
rotates and tilts. A two-finger tap is right click. Camera motion is smoothed,
with lower sensitivity and proportional zoom. Character heads currently use
their neutral pose; facial animation and lip sync are not implemented.

Compact, cached draw buffers are enabled by default. For driver diagnosis,
`A5_D3D_BUFFER_STREAM=0` selects the original buffer-upload path, while
`A5_D3D_BUFFER_MAP=1` selects experimental mapped uploads when STREAM is unset.
An explicit `A5_D3D_BUFFER_STREAM=1` takes precedence. Mapped uploads failed
sustained scene checks; use the default for play. See [PORTING.md](PORTING.md)
for measured performance and remaining limits.

`A5_SCENE_TRACE=1` logs the actual pause state and camera placement.
`A5_CAMERA_POS=x,y,z,rod,pitch,yaw` sets one initial mission view for repeatable
measurements. Remove these test overrides afterwards. For render diagnosis,
`A5_D3D_CAPTURE_FRAME=N` with `A5_D3D_CAPTURE_DIR=<app-writable directory>` dumps
one frame's draw results as PPM thumbnails; exclude that readback frame from
performance measurements.

## Running without a device

The same checks run headlessly on macOS/Linux:

```bash
cmake -S . -B build/host -G Ninja
cmake --build build/host
./build/host/silentstorm_hosttest ../Complete
ctest --test-dir build/host --output-on-failure
```

Exit code is 0 when every check passes, so it works as a CI gate.

## Android Studio

`android/` is also a Gradle project — open it and it will build the same
`CMakeLists.txt` through the NDK. The shell scripts and Gradle produce the same
native library; the scripts exist so the build needs nothing but the SDK.

## Troubleshooting

**"no game data found"** — the report lists every directory it looked in and what
it saw there. The usual causes are data one level too deep (a `Complete/` folder
inside `SilentStorm/`) and the adb permission issue above.

**App starts and closes immediately** — check `adb logcat -b crash`. Report the
native stack trace along with the ABI (`adb shell getprop ro.product.cpu.abi`).

**A check fails on device but not on the host** — that is worth reporting as a
port bug; the two run identical code over identical data.
