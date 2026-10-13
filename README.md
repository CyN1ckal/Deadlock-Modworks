# Deadlock Mod Manager

A native C++20 mod manager for Windows x64. Browse Deadlock skins in a thumbnail gallery, assemble a custom VPK, inspect models, and repair supported skeleton changes.

The main executable is **`DeadlockSkeletonRepair.exe`**. Launch it without arguments to open the GUI, or use command-line options to analyze and repair a mod from a terminal.

## Build from source

### 1. Install the build tools

- **Visual Studio 2026 or 2022**, with the **Desktop development with C++** workload and a Windows SDK.
- **CMake 3.24 or newer**, available on your `PATH`.
- **Vulkan SDK**, including `glslc`. Restart your terminal after installation so it picks up `VULKAN_SDK`.

The viewer needs a graphics driver supporting **Vulkan 1.3**. The SDK is a build dependency; you do not need it to run a packaged app.

### 2. Configure and build

Open PowerShell in the repository folder, where `CMakeLists.txt` is located. The first configure downloads the C++ dependencies, so it needs an internet connection.

**Visual Studio 2026:**

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
```

**Visual Studio 2022:**

```powershell
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64
cmake --build build/release --config Release
```

Choose the commands for your installed Visual Studio version. If you switch generators later, use a different build folder, such as `build/release-vs2022`.

### 3. Start the app

```powershell
.\build\release\bin\Release\DeadlockSkeletonRepair.exe
```

You can also double-click that executable in File Explorer. Close the app before rebuilding it, so Windows can replace the executable.

## Build and run in CLion

Open the repository's `CMakeLists.txt` in CLion. In **Settings > Build, Execution, Deployment > Toolchains**, create or select a **Visual Studio** toolchain named **Visual Studio**, with the architecture set to **amd64**. Use its bundled CMake and Ninja. The Vulkan SDK must be installed and visible to CLion; restart the IDE after installing it.

Reload the CMake presets and enable a profile in **Settings > Build, Execution, Deployment > CMake**. **clion-debug** and **clion-release** are normal builds. **clion-profile** is the same app, optimized with debug symbols, with Tracy announcing the window to tracy-profiler while it is open. Select the enabled profile and the **DeadlockSkeletonRepair** target to build, run, or debug. The profiles use separate `build/clion-debug`, `build/clion-release`, and `build/clion-profile` directories and do not depend on a specific Visual Studio release.

For command-line use of these presets, run CMake from an **x64 Native Tools Command Prompt** with Ninja on `PATH`:

```powershell
cmake --preset clion-debug
cmake --build --preset clion-debug
```

## Use the GUI

### Connect the installed game

The app tries to find Deadlock in your Steam libraries automatically. The connection shows under **Game** in the explorer and at the left of the status bar. If it is not connected, choose **File > Choose game archive...** (**Ctrl+G**) and select:

```text
<your Steam library>/steamapps/common/Deadlock/game/citadel/pak01_dir.vpk
```

The installed game provides the reference models, textures, and animation resources used by the viewer and comparison tools.

### Add your mods

1. Click **Add** in the explorer, or choose **File > Add mods...** (**Ctrl+O**).
2. Select one or more mod `.vpk` files. Use **Ctrl-click** for individual files or **Shift-click** for a range, then click **Open**.
3. Browse the **Mod library** tab. Search by model or source (**Ctrl+F**), click a source in the explorer to show only its skins, and double-click a skin to inspect its model. **Grid** and **List** switch between thumbnails and a sortable table; **Ctrl + mouse wheel** resizes the thumbnails.

You can also drop multiple VPKs onto the window or executable. Existing library entries are not added twice. Adding a batch keeps you in the gallery. Thumbnails load as cards become visible and stay in a bounded cache. Right-click a source in the explorer and choose **Remove from library** to remove it; this does not delete the files.

Most items have a right-click menu: skins (inspect, multi view, pack selection, copy paths, reveal in Explorer), sources, history versions and copies, repair results, meshes and the 3D viewports. Press **F1** for the keyboard shortcuts.

A standalone compiled `.vmdl_c` file can be opened for viewing. Version comparison requires a model from a mod VPK.

### View a model

Double-click a skin, or right-click it and choose **Inspect**, to open the **Model inspector**. Its **Preview** tab shows textures, levels of detail (LOD), and the skeleton, with model statistics and per-mesh visibility in the side pane. Choose an animation clip and click **Play** when animation resources are available.

### Build a combined model VPK

1. Use **Add mods...** to add your reference mod VPKs to the library, or **Add folder...** to add every mod VPK in a folder such as `addons`. Chunk files and `pak01_dir.vpk` are skipped.
2. Tick the checkbox on the skins you want, or select one and press **Space**. Selected cards are highlighted. Right-click a skin to include every shown skin or a whole source. The **In pack** filter shows only your selections. Missing materials fall back to clay; hover **No preview** for an error. Use **Refresh** (**F5**) after changing source files.
3. Connect the installed game archive to resolve dependencies supplied by Deadlock.
4. Open the **VPK builder** tab to review your selected skins. Click **Build VPK...** (**Ctrl+B**) and choose a new `.vpk` filename. Export runs in the background and can be cancelled. The output is reopened and validated before it is published. Existing output files and source archives are never overwritten.

Export follows external resource references and supported KV3 resource/skeleton references recursively. It includes selected models and reachable overrides from each model's own source archive; base-game dependencies stay in the game. Identical files at the same internal path are deduplicated. Different contents at the same path block export and identify both sources: deselect one of the conflicting models. A missing dependency also blocks export. Dependencies supplied only by another reference VPK are not resolved automatically.

### Your model library

Everything the manager keeps is saved in the model library, `%LOCALAPPDATA%\DeadlockModManager\store`: every imported mod, your collection, pack selections, the connected game, gallery thumbnails, and the repair history of each model. The catalog is `catalog.sqlite`; file contents are kept under `blobs`. Imported mods no longer depend on their original VPKs or chunk files, which can be moved or deleted.

- Each file inside a mod is stored once per SHA-256 checksum. Importing the same mod again, even renamed or from another folder, reuses it, and files that several mods share are stored once.
- A changed original, or a repaired mod, is stored as a new version. Earlier versions stay in the library.
- **Remove from library** only takes a mod out of your collection; it stays stored.
- Mods you open are rebuilt from the library into the `cache` folder next to it. That folder can be deleted at any time and is rebuilt when needed.

Use **Share library...** (in the **File** menu or under **Library** in the explorer) to save a copy of your whole library as one `.db` file. **Add shared library...** adds a shared `.db` library's mods, pack selections and thumbnails to yours; nothing already in your library is replaced. Each shared mod is checked against its checksums first. A game location from another computer is used only if it exists on yours. **Add shared library...** also opens JSON library lists from earlier releases and imports the VPKs they name.

**Version history** lists every model and pack in the library, the versions of each model and how repairs changed them. Click a version to make it **A**; the version clicked before it becomes **B**. From the toolbar or a version's right-click menu you can view or compare versions, compare a version with the game, repair a pack, or assemble one model into a new pack.

On first start, the manager adds the mods, collection and thumbnails from earlier releases (`library.db`, `library.json`, `mods`, `thumbnails`) to the model library. Those files are only read and are left in place.

**Refresh** rescans changed archives; missing checked models are shown so you can remove them explicitly.

Only reachable resource overrides are included. Unreferenced scripts, sounds, addon metadata, or other extras bundled by a mod author are not automatically copied. Test the exported pack in Deadlock; packaging does not repair models or guarantee in-game compatibility.

| Control | Action |
| --- | --- |
| Left mouse drag | Orbit the camera |
| Mouse wheel | Zoom |
| Middle or right mouse drag | Pan |
| **Frame** or **F** | Fit the model in view |
| Right-click | Display options: bones, grid, coloring, LOD |

### Multi view

Right-click a skin and choose **Multi view**, or open the **Multi view** tab (**Ctrl+6**) and pick a character. The page shows up to four models of that character (`models/heroes/<character>/...`), one in each viewport. The clicked model is shown first, then one model from each other mod of that character, with further files from those mods filling any viewports that remain. Each viewport's list can show a different model. **Link** keeps orbit, pan, and zoom on one shared camera, and one animation clip plays on every viewport that has an animation skeleton. The viewport count can be 1, 2, 3, or 4.

### Compare versions

Open **Compare versions** in the **Model inspector** and choose the versions shown on the left and right: **Mod**, **Game**, or **Repaired**, when available. **Link cameras** keeps the two views aligned. The panel also shows skeleton differences, skin-binding checks, and available animation playback.

**Repaired** is a proposed repair held in memory. Viewing it does not change a VPK. The comparison previews model and animation-skeleton migrations; the repair workflow also checks the archive's dependent resources.

### Analyze and repair

Close Deadlock and wait for Steam updates to finish before repairing.

1. Add the VPKs you want to check to the library.
2. Open the **Skin repairs** tab and click **Analyze**. This checks every VPK in the library without changing it.
3. Read the results table. Click an outcome count in the toolbar to hide or show those results, and hover a row for its full detail and warnings. Click **Repair *N* supported** to repair the archives eligible for automatic fixes.
4. Use **Save report...** to export the results as JSON. Test repaired mods in Deadlock.

Repair rechecks the current files before writing, replaces each supported VPK, and saves an original `.bak-...` backup beside it. Archives needing review are left untouched. The selected archive reloads after repair.

**Cancel** stops at the next cancellation checkpoint. Changing the game or mod library clears the analysis. File selection is disabled while analysis or repair is running.

### Understand the results

| Result | Meaning |
| --- | --- |
| Repairable | A supported fix passed the structural checks and can be applied. |
| Updated | The repaired archive was written, reopened, and validated. |
| Current | No supported rig issue needing repair was found. This does not guarantee in-game compatibility. |
| Needs review (`NeedsReview` in JSON/CLI) | A dependency, incompatible rig, or unsupported structure needs a mod author's review. |
| Failed | The input could not be read, validated, or repaired. |
| Skipped | No supported hero models were found in the archive. |

### Undo a repair

Close Deadlock, move the repaired VPK out of the addons folder, and copy its `.bak-...` backup back to the original VPK filename. Keep the backup until you have tested the mod. For a chunked VPK, keep its original chunk files too: the backup directory VPK still refers to them.

## Use the command line

Run these examples from `build/release/bin/Release`, or from an extracted portable package. Replace the game and mod paths with your own.

```powershell
# Check a mod without changing it and save a report.
.\DeadlockSkeletonRepair.exe --analyze "C:\Games\Deadlock\game\citadel" "C:\Mods\mod.vpk" --report "report.json"

# Write a repaired copy to a new file.
.\DeadlockSkeletonRepair.exe --update "C:\Games\Deadlock\game\citadel" "C:\Mods\mod.vpk" "C:\Mods\mod-repaired.vpk"

# Repair the original mod and keep a backup.
.\DeadlockSkeletonRepair.exe --repair "C:\Games\Deadlock\game\citadel" "C:\Mods\mod.vpk" --report "repair.json"

# Show command-line help.
.\DeadlockSkeletonRepair.exe --help
```

Each command accepts one mod VPK. Use the GUI to work with multiple archives. The game argument is the **citadel folder**, not the `pak01_dir.vpk` file. `--update` refuses to overwrite an existing output file. Optional `--report` must come last and use a `.json` filename. **Ctrl+C** cancels the operation.

Exit codes: **0** = supported analysis or completed repair; **1** = needs review or unsupported content; **2** = invalid input, failure, or cancellation. JSON reports use schema version 2.

## Run the tests

After building, run both C++ test executables:

```powershell
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
```

The core suite checks archives, resources, skeleton migration, and command-line behavior. The rendering suite checks model previews, comparisons, skinning, and animation poses; GPU tests may skip when a compatible Vulkan device is unavailable.

## Create a portable package

From the repository folder:

```powershell
.\scripts\Publish.ps1
```

The script builds the release app, chooses Visual Studio 2026 or 2022 when configuring a fresh build folder, and creates **`dist/DeadlockSkeletonRepair-win-x64.zip`**. Extract the ZIP and launch `DeadlockSkeletonRepair.exe`. It includes the README and third-party licenses. Shaders are embedded; game assets are not included.

The executable uses the Visual C++ runtime. If Windows reports a missing `VCRUNTIME` or `MSVCP` DLL, install the Microsoft Visual C++ x64 Redistributable. The GUI also needs a Vulkan 1.3 graphics driver.

## Build a repair-only app

To build without the Vulkan SDK, use a separate build folder:

```powershell
cmake -S . -B build/repair-only -G "Visual Studio 17 2022" -A x64 -DSR_BUILD_MODEL_TOOLS=OFF
cmake --build build/repair-only --config Release
.\build\repair-only\bin\Release\DeadlockSkeletonRepair.exe
```

For Visual Studio 2026, use `-G "Visual Studio 18 2026"`. This build opens the native repair window and supports the same CLI commands. Model viewing and comparison are available in the standard build.

## Scope and limits

Automatic repair supports VPK version 2, hero `.vmdl_c` models, and AG2 `.vnmskel_c` animation-skeleton overrides. It can migrate compatible animation bindings, handle unambiguous case-only bone renames, and normalize supported embedded skeletons while preserving authored poses and skin bindings.

Custom proportions, helper bones, cloth, and reference poses can be intentional. Differences from the game are not automatically treated as errors. The tool does not infer arbitrary bone renames, recompile source assets, or rewrite animation clips, physics, or cloth. Dependencies are resolved from the selected mod VPK and installed game, not from other addons in the library.

Structural validation and preview playback cannot guarantee correct appearance or behavior in Deadlock. Test repaired mods in-game.

## C++ source layout

- `src/app`: main executable, CLI dispatch, and the repair-only Win32 GUI.
- `src/core`: VPK/resource parsing, skeleton repair, game detection, and reports.
- `src/render`: Vulkan rendering, model versions, and comparison logic.
- `src/preview`: the shared model tools GUI and animation controls.
- `tests`: core and rendering tests.

The standard build also produces `ModelPreview.exe`, a development entry point to the same workspace with `--game <citadel-folder>` and `--clip <resource-path>` options.

## Credits

Binary KV3 and Source 2 resource layouts follow [Source 2 Viewer](https://s2v.app) / [ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat), pinned to release 20.0.6980. ValveResourceFormat is not linked into the executable. See [third-party notices](THIRD_PARTY_NOTICES.md) for attribution and the licenses for LZ4, Zstandard, nlohmann/json, meshoptimizer, and Dear ImGui.
