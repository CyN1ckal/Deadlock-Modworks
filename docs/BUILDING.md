# Building from source

Users who have `DeadlockModworks-0.1.0-win-x64.zip` do not need this page. Extract it and follow [QUICK_START.md](QUICK_START.md).

The project builds on 64-bit Windows. CMake stops on any other host.

## Tools

- Visual Studio 2026 or 2022, with the **Desktop development with C++** workload and a Windows SDK.
- CMake 3.24 or newer on `PATH`.
- The Vulkan SDK, including `glslc`, when you build the model tools. Restart the terminal after installing it so `VULKAN_SDK` is set.

The first configure downloads C++ dependencies and needs a network connection. The packaged app does not need the SDK to run. It needs a Vulkan 1.3 driver.

## Configure and build

Open PowerShell in the repository folder, where `CMakeLists.txt` is. Use a different build folder if you change generators. Do not reuse one CMake cache with another generator.

Visual Studio 2026, preset `windows-release`:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
```

Visual Studio 2022:

```powershell
cmake -S . -B build/release-vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/release-vs2022 --config Release
```

The preset output is:

```text
build\release\bin\Release\DeadlockModworks.exe
```

Close the app before rebuilding, so Windows can replace the executable. The Release build links the static MSVC runtime (`/MT`).

`ModelPreview.exe` is a development entry point to the same workspace. It is not the packaged product. It accepts `--game <citadel-folder>` and `--clip <resource-path>`.

## CLion

Open `CMakeLists.txt`. In **Settings > Build, Execution, Deployment > Toolchains**, use a Visual Studio toolchain named **Visual Studio**, architecture **amd64**, with the bundled CMake and Ninja. The Vulkan SDK must be visible to CLion. Restart the IDE after installing it.

Enable a profile under **Settings > Build, Execution, Deployment > CMake**:

- **clion-debug** and **clion-release** are normal builds.
- **clion-profile** is optimized, keeps debug symbols, and defines Tracy. While that build is open it announces the window to tracy-profiler.

Select the profile and the **DeadlockModworks** target. The profiles use `build/clion-debug`, `build/clion-release`, and `build/clion-profile`.

From an **x64 Native Tools Command Prompt** with Ninja on `PATH`:

```powershell
cmake --preset clion-debug
cmake --build --preset clion-debug
```

## Tests

From the Visual Studio 2026 release tree:

```powershell
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
ctest --test-dir build/release -C Release --output-on-failure
py -3 scripts/architecture_diagram.py --check
```

`architecture_diagram.py` needs Python 3 and `networkx`. CTest registers it as `dependency_levels` when that module was found at configure time. `render_tests` can skip GPU cases when no Vulkan 1.3 device exists. A skip is not a failed assertion. Regenerate the diagram with `py -3 scripts/architecture_diagram.py` only when the include graph changes. See [ARCHITECTURE.md](ARCHITECTURE.md).

Recorded results for earlier commits are in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md). Those rows are not a result for a later commit until the commands are run again.

## Repair-only build

To compile without the Vulkan SDK, use a separate directory and turn the model tools off:

```powershell
cmake -S . -B build/repair-only -G "Visual Studio 17 2022" -A x64 -DSR_BUILD_MODEL_TOOLS=OFF
cmake --build build/repair-only --config Release
```

For Visual Studio 2026, use `-G "Visual Studio 18 2026"`. That build opens the repair-only window and still accepts the command-line analyze, update, and repair commands. The packaged 0.1 app is the full model-tools build.

## Package

Maintainer packaging is [RELEASING.md](RELEASING.md). `scripts/Publish.ps1` builds Release, stages under `build\package-staging\<id>`, and writes `dist\DeadlockModworks-0.1.0-win-x64.zip`. Do not point it at an arbitrary directory.

## Layout

- `src/app` — process entry, command-line dispatch, and the repair-only window.
- `src/core` — VPK and resource reading, the content store, export, and skeleton repair.
- `src/render` — Vulkan rendering and comparison data.
- `src/preview` — the model-tools window. Menus and shortcuts are in `src/preview/main.cpp`.
- `tests` — core and rendering tests.
- `assets/branding` — the icon sources embedded by the resource script.

Binary KV3 and Source 2 resource layouts follow [ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat) release 20.0.6980. That project is not linked. Attribution is in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
