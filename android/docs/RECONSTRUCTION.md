# Silent-Storm-Reconstruction integration

The complete `develop` tree from
[met-nikita/Silent-Storm-Reconstruction](https://github.com/met-nikita/Silent-Storm-Reconstruction)
is merged under `reconstruction/`, with its Git history retained as a merge
parent. The imported revision is
[`61b033f25a299389a648c0f84dc0f8140a6c7ef7`](https://github.com/met-nikita/Silent-Storm-Reconstruction/commit/61b033f25a299389a648c0f84dc0f8140a6c7ef7)
(267 commits, 1,568 tracked files). The imported subtree is unchanged from that
revision. Its README, build scripts, CMake files and third-party notices remain
inside that directory. The original `Soft/` and game data are preserved.

## Which engine is built

- **Android:** `android/scripts/build_apk.sh arm64-v8a` still stages the January
  2003 engine through `android/tools/prepare_sources.py`, preserving the ARM64,
  GLES, resource-import, input and performance work. Rule set 26 enables the
  reviewed fixes below in the actual staged engine.
- **Windows/x86 reconstruction:** use `reconstruction/build.bat`, or follow
  [its build instructions](../../reconstruction/README.en.md) from that directory.
  This contains all upstream gameplay, AI, save, UI and tool changes. It was
  imported intact; the Windows build has not been validated on this Mac.

Importing the entire repository does **not** switch Android to its Windows
engine. Its runtime has hundreds of dependent changes, different database/save
layouts, extra source files, MSVC headers and x86 dependencies. Those require
separate adaptation and testing before they can replace the Android runtime.

## Fixes enabled in Android

| Upstream commit | Enabled change |
| --- | --- |
| `8b4d7a8bd` | `GRenderCore.cpp`: compact retained render operations in the source list, preserving the lower-pass list. |
| `b14bbb4f2` (part) | `MapBuild.cpp`: initialize building-object pivot offsets to zero before rotation. |
| `dbb74c6e6` (part) | `Script/lobject.h`: initialize new Lua stack slots to nil for safe stack traversal/serialization. |
| `07e6ad72e` | `GRenderLight.cpp`: skip inactive animated point lights and radii below 0.1. |
| `5d97df475` (part) | `GAnimation.cpp`: return the target pose at the exact endpoint; `GLightmapCalc.cpp`: skip shadow tracing when the visibility scene is null. |
| `789470fe4` | `aiInventory.cpp`: skip stale RPG weapon items before querying cover or firing modes. |
| `f74265771` | `scFlowChartItems.cpp`: initialize new clue template IDs to zero. |

The rule names carry their upstream commit IDs. Partial commits are explicitly
marked: their other changes remain available in the imported tree but are not
silently treated as active in Android. Every rule must match during staging;
an unmatched rule fails preparation.

## Validation and future updates

The device harness tests the real `SplitOps` implementation against every
partition of up to seven draw operations, including empty lists, the threshold
boundary, existing destination entries and stable ordering. The shared host and
device harness poisons storage before constructing a Lua value and checks its
initial nil tag. Existing shader, buffer, frame-scheduling and BSP regressions
continue to run.

Validated on 2026-10-07: ARM64 full-game and ARMv7 harness builds succeed;
53 device checks and 39 host checks pass, with two existing data/UI warnings.
CTest's frame schedule and BSP-cache tests pass. The merged Android build was
installed on the Galaxy Z Fold7 and the mission loaded using the default
renderer; no GL errors were logged during the tested building views. The wide
view still measured 28.8–29.5 FPS, so universal locked 30 FPS is not claimed.

For future upstream revisions, fetch the `reconstruction` remote, review the
diff against the pinned commit, and merge into the same subtree. Keep the
imported tree intact and adapt Android changes in named staging rules. Update
this record when the imported revision or active Android subset changes.
