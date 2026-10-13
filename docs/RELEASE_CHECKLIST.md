# Release checklist

Stage 1 baseline for Deadlock Modworks 0.1. This file records commands that were run in this worktree and the results of those runs. Historical test counts are not used as evidence. This is not a release candidate.

Code under test: `8470d845c656fba0c60bcf02644fbb7537a6070a` on branch `codex/modworks-v0.1`. The plan is commit `8794bcb` (`docs: add Modworks 0.1 release plan`). No version was tagged. No files were pushed.

## Starting point

| Item | Recorded value |
| --- | --- |
| Worktree | `C:\Users\warri\.ao\data\worktrees\deadlock-model-updater\deadlock-model-updater-8` |
| Branch | `codex/modworks-v0.1`, already checked out, tracking `origin/main` |
| `main` and `origin/main` | `8470d845c656fba0c60bcf02644fbb7537a6070a` |
| Working tree at start | Clean. `git status --short` printed nothing |
| Local tags | `git tag --list` printed nothing |
| Remote tags | `git ls-remote --tags origin` printed nothing |
| `v0.1.0` | Free. Not assigned and not tagged |
| CMake project version | `0.1.1` (`project(DeadlockSkeletonRepair VERSION 0.1.1 ...)` in `CMakeLists.txt`) |
| Owner license decision | MIT. `LICENSE` was not added in this stage |

The main checkout still holds the untracked plan source that was copied here, plus local junk (`.idea/`, `diag.obj`, `vc140.pdb`). Those files were not added, edited, or deleted. `build/`, `dist/`, `input/`, and game or mod assets were not committed. Disposable VPK copies used for the workflow runs lived under `%TEMP%\modworks-baseline-8`.

`codex/modworks-v0.1-icon` exists in another worktree at the same base commit. It was not modified.

## Toolchain

| Tool | Value |
| --- | --- |
| OS | Windows 11 Pro, version 10.0.26200 |
| CMake | 4.3.3 |
| Visual Studio | Community 2026 Insiders 18.11.12224.323 at `C:\Program Files\Microsoft Visual Studio\18\Insiders` |
| `vswhere` without `-prerelease` | No display name. Exit code 0 |
| MSVC | 19.51.36260.0, toolset directory `14.51.36231` |
| Windows SDK selected by CMake | 10.0.26100.0 |
| Generator | Visual Studio 18 2026, x64, preset `windows-release`, binary dir `build/release` |
| Vulkan SDK | 1.4.357.0. `glslc.exe` is on `PATH`. `VULKAN_SDK=C:\VulkanSDK\1.4.357.0` |
| Python | `py -3` is Python 3.14.5. CMake found `C:\Users\warri\AppData\Local\Python\pythoncore-3.14-64\python.exe`. `networkx` 3.6.1. `python.exe` and `sqlite3` are not on `PATH` |
| GPU | AMD Radeon Graphics driver 32.0.13018.6, and NVIDIA GeForce RTX 4070 SUPER driver 32.0.16.1088 |
| Installed game | `C:\Program Files (x86)\Steam\steamapps\common\Deadlock\game\citadel` exists, with `pak01_dir.vpk` and chunk files |

The worktree had no `build/release/CMakeCache.txt` before configure, so there was no stale Visual Studio 2022 cache to delete. Configure recorded `SR_BUILD_MODEL_TOOLS=ON` and `SR_TRACY=OFF`. `ModelPreview.exe` built. That is not an environment skip.

## Commands and automated results

All commands were run from the worktree root. Exit code 0 unless noted.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
ctest --test-dir build/release -C Release --output-on-failure
```

| Command | Result |
| --- | --- |
| Configure | Exit 0. Generator Visual Studio 18 2026. Tracy profiler disabled. Vulkan 1.4.357 found, including `glslc` |
| Full Release build | Exit 0. About 130 seconds. Outputs include `DeadlockSkeletonRepair.exe`, `ModelPreview.exe`, `skeleton_repair_tests.exe`, and `render_tests.exe` under `build\release\bin\Release` |
| `skeleton_repair_tests.exe --gtest_brief=1` | 214 tests from 21 suites ran. 214 passed. 5708 ms. Exit 0 |
| `render_tests.exe --gtest_brief=1` | 50 tests from 11 suites ran. 50 passed. 289 ms. Exit 0. The summary had no skipped line |
| `architecture_diagram.py --check` | Exit 0. `58 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |
| CTest Release | 265 tests passed, 0 failed, 11.56 seconds. The last test was `dependency_levels` |

CTest's 265 tests are the 214 core tests, the 50 render tests, and `dependency_levels`. GPU render tests executed (about 0.18 seconds each). No Vulkan skip occurred.

## Disposable workflow runs

Executable: `build\release\bin\Release\DeadlockSkeletonRepair.exe`.

Store and copies: `%TEMP%\modworks-baseline-8`. Sources were copies of `input\*.vpk` from the main checkout. The live library at `%LOCALAPPDATA%\DeadlockModManager` was not opened by these commands.

| Flow | What ran | Result |
| --- | --- | --- |
| Import | `--store <dir> import <copy>` for `pak07_dir.vpk`, `pak12_dir.vpk`, and `pak13_dir.vpk` | Exit 0. `pak07` imported 0 models and 16 other files. `pak12` imported 1 model and 8 other files (9 new blobs). `pak13` imported 1 model and 4 other files |
| Import again | Reimport of the moved `pak12` copy into the same store | Exit 0. Same snapshot id. 0 new blobs |
| Second archive | Import of the `pak07` copy into that store | Exit 0. 0 models, 16 other files, 15 new blobs |
| Bad archive | Import of a text file renamed to `.vpk` | Exit 2. Message: `VPK directory or embedded data is outside the file.` The earlier snapshots stayed listable |
| Restart | New process `--store <dir> models` before moving the imported file, then again after the move | Both exit 0. Both printed 1 model line with the same 12-hex prefix |
| Inspect (stored list) | The same `--store models` output | Exit 0. One stored `.vmdl_c`. This is the headless list, not the Model inspector viewport |
| History | `--store <dir> history <64-hex-blob>` using the `vmdl_c` membership from `catalog.sqlite` | Exit 0. One version line, relation `imported` |
| Build (library API) | Temporary program linked to `skeleton_repair_core.lib`, not committed. It called `ExportModelLibrary` for the first model in the `pak12` copy, with the installed `pak01_dir.vpk` as the game archive, then `ContentStore::Assemble` for one stored model | Export wrote 8 files, reopened, and `Validate` succeeded, and the model path was present (1,909,562 bytes). Assemble wrote 2 entries, `Validate` succeeded (149,193 bytes), and `ImportVpk` of that file returned a 32-character snapshot id. `HistoryOf` on the first listed model returned 1 version and 0 links. Driver exit 0 |
| Repair, current sample | `--analyze`, `--update`, and `--repair` on separate copies of `pak12_dir.vpk`, game folder `citadel`, JSON reports | All exit 0. Outcome Current. No VPK written. Copy length stayed 1,909,595. `--repair` created 0 backups |
| Repair, blocked sample | `--analyze` on a copy of `pak13_dir.vpk` | Exit 1. Outcome NeedsReview. No VPK written |
| Repair, more copies | `--analyze` on copies of `pak08`, `pak04`, `pak10`, `pak11`, `pak03`, `pak05`, `pak01`, and `pak02` directory VPKs | See the table below. Every source copy kept its original length |
| Repair write | `--update` on the `pak02_dir.vpk` copy after analyze reported a Repairable entry | Exit 0. Report `WroteFile` true, `HasFailures` false, `HasBlockers` false, `BackupPath` null. Output 666,286,771 bytes, SHA-256 `623CF63375095065AA17613C691C1AF31C69DBD381C45F82180D351DB8221882`. The copy SHA-256 matched the original input file `0D30910E51212FA0C1BFD50752B2C182D39C0E86FFB00181CC7D052230EA9AF0` (666,271,418 bytes) |

Further `--analyze` outcomes, one row per copied directory VPK:

| Copy | Exit | Outcome counts | Length unchanged |
| --- | --- | --- | --- |
| `pak08_dir.vpk` | 0 | Current 2 | Yes |
| `pak04_dir.vpk` | 0 | Current 1 | Yes |
| `pak10_dir.vpk` | 1 | Skipped 1 | Yes |
| `pak11_dir.vpk` | 0 | Current 1 | Yes |
| `pak03_dir.vpk` | 0 | Current 1 | Yes |
| `pak05_dir.vpk` | 1 | NeedsReview 1, Current 1 | Yes |
| `pak01_dir.vpk` | 0 | Current 2 | Yes |
| `pak02_dir.vpk` | 0 | Current 1, Repairable 1 | Yes |

The VPK builder button, the Model inspector viewport, and the Skin repairs page were not clicked. The build and repair results above are the headless entry points those pages call (`ExportModelLibrary`, `ContentStore::Assemble`, and `HeroModelUpdater::Analyze` / `Update`).

## IDE layout at this commit

Captured from `src/preview/main.cpp`. A screenshot was not taken.

The window is created at 100,100 with size 1440x960, title `Deadlock Mod Manager`, style `WS_OVERLAPPEDWINDOW`, and `WS_EX_ACCEPTFILES`. The window class does not set an icon. The Vulkan application name is also `Deadlock Mod Manager`. `ApplyTheme` starts from `StyleColorsDark` and uses window padding 6, frame padding 6x3, item spacing 6x4, and corner radii from 0 to 3. The window background and the swapchain clear color are RGB 0.118, 0.122, 0.133. The accent in `src/preview/ui.hpp` is RGB 0.34, 0.58, 0.96.

The menu bar is File, Edit, View, Tools, Help. View lists Mod library (Ctrl+1), VPK builder (Ctrl+2), Version history (Ctrl+3), Skin repairs (Ctrl+4), Model inspector (Ctrl+5), Multi view (Ctrl+6), Back, Explorer, Status bar, and Refresh library.

On the first frame the dock builder removes the dock node and splits a left Explorer at 17% from a right Workspace. Both dock windows hide their tab bars. The workspace then draws its own tab strip: Mod library, VPK builder with a selection count, Version history, Skin repairs, Model inspector, and Multi view. The explorer sections are Sources (Add, Folder, filter, source list), Game, and Library (Share, Add shared). A status bar is drawn above the dockspace. `io.IniFilename` is `nullptr`, so ImGui does not save the layout.

## Acceptance evidence

Environment for every row that ran: Windows 11 Pro 10.0.26200, this worktree, commit `8470d84` plus the plan commit, Release preset, MSVC 19.51.36260, Vulkan SDK 1.4.357.0, RTX 4070 SUPER driver 32.0.16.1088.

| Check | Result (PASS/FAIL/BLOCKED/NOT RUN) | Commit/artifact | Environment | Evidence/notes |
| --- | --- | --- | --- | --- |
| Release configure and full build | PASS | `8470d84` | See above | `cmake --preset windows-release` and `cmake --build --preset windows-release`, both exit 0. `SR_BUILD_MODEL_TOOLS=ON`, `SR_TRACY=OFF` |
| Core tests | PASS | `8470d84` | See above | 214/214 passed, exit 0 |
| Rendering tests | PASS | `8470d84` | See above | 50/50 passed, exit 0, no skip line |
| Dependency architecture check | PASS | `8470d84` | See above | Script exit 0, and CTest `dependency_levels` passed. CTest total 265/265 |
| `ModelPreview` build | PASS | `8470d84` | Vulkan SDK present | `build\release\bin\Release\ModelPreview.exe` produced. Not a skip |
| Import | PASS | `8470d84` | Disposable store | Three real copies imported, reimport added 0 blobs, bad file exit 2 |
| Restart | PASS | `8470d84` | Disposable store | Model list survived a new process after the source file was moved |
| Inspect stored models | PASS | `8470d84` | Disposable store | `--store models` listed the imported model after the source move |
| Model inspector viewport | NOT RUN | `8470d84` | Live library not opened | No headless command opens the viewport. The GUI was not launched |
| Build a VPK from a real copy | PASS | `8470d84` | Disposable copies plus installed `pak01_dir.vpk` | `ExportModelLibrary` and `Assemble` validated outputs. The GUI button was not clicked |
| Repair decision | PASS | `8470d84` | Disposable copies plus installed citadel folder | Current, NeedsReview, Skipped, and Repairable outcomes observed. Sources were not modified |
| Repair write | PASS | `8470d84` | Copy of `input\pak02_dir.vpk` | `--update` exit 0 wrote a new VPK. Source SHA-256 matched the original input file |
| History | PASS | `8470d84` | Disposable store | `--store history` exit 0, one imported version |
| IDE screenshot | NOT RUN | `8470d84` | Live library present | Launching the GUI runs `LocalModStore::Load`, which materializes every gallery source, and process exit calls `Persist`, which deletes and rewrites `gallery_sources`, `gallery_selected`, and `gallery_packages`. Layout is described from source instead |
| Identity, icon, theme, packaging, docs, license file | NOT RUN | | | Later stages. MIT is decided and was not written into `LICENSE` |

## Stage 2 product identity

Worktree: `C:\Users\warri\.ao\data\worktrees\deadlock-model-updater\deadlock-model-updater-10`. Branch `codex/modworks-v0.1`. Started from `1b9bb6d` (`docs: record v0.1 verified baseline`). No reset or rebase. The GUI was not launched. `%LOCALAPPDATA%\DeadlockModManager` was not opened. `scripts/Publish.ps1` was not executed. No tag was created and nothing was pushed.

The tested binary was configured while `HEAD` was still `1b9bb6d5325a8e4f5fe17071f72cf7d130ff0a4e`, so its diagnostics embed that revision. The next configure embeds whatever `git rev-parse HEAD` returns then.

Commands, from this worktree, exit code 0:

```powershell
cmake --preset windows-release -DSR_BUILD_MODEL_TOOLS=ON -DSR_TRACY=OFF
cmake --build --preset windows-release
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
ctest --test-dir build/release -C Release --output-on-failure
.\build\release\bin\Release\DeadlockModworks.exe --version
```

| Command | Result |
| --- | --- |
| Configure | Exit 0 in 21.8 seconds. Visual Studio 18 2026, Windows SDK 10.0.26100.0, MSVC 19.51.36260.0. `SR_BUILD_MODEL_TOOLS=ON`, `SR_TRACY=OFF`. Vulkan 1.4.357 found, including `glslc` |
| Full Release build | Exit 0 in 129.78 seconds. `build\release\bin\Release\DeadlockModworks.exe` and `ModelPreview.exe`. `DeadlockSkeletonRepair.exe` was not produced |
| `skeleton_repair_tests.exe --gtest_brief=1` | 215 tests from 22 suites ran. 215 passed. 6427 ms. Exit 0. The added test is `Library.DefaultDataRootIsDeadlockModManager` |
| `render_tests.exe --gtest_brief=1` | 50 tests from 11 suites ran. 50 passed. 326 ms. Exit 0. The summary had no skipped line |
| `architecture_diagram.py --check` | Exit 0. `58 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |
| CTest Release | 266 tests passed, 0 failed, 11.35 seconds. The last test was `dependency_levels`. `Library.DefaultDataRootIsDeadlockModManager` passed |
| `DeadlockModworks.exe --version` | Exit 0. Printed `Deadlock Modworks 0.1.0 (1b9bb6d5325a8e4f5fe17071f72cf7d130ff0a4e)`. No store path was passed |

`--help` still starts `Deadlock Modworks 0.1.0 — hero models and animation skeleton repairs` and returns 0. Existing analyze, update, repair, and store arguments and exit codes were not changed. The version check is inside the existing command-line test.

Windows file properties on that executable, read without launching the GUI:

| Property | Value |
| --- | --- |
| FileDescription | Deadlock Modworks 0.1.0 |
| FileVersion | 0.1.0.0 |
| ProductName | Deadlock Modworks |
| ProductVersion | 0.1.0 |
| OriginalFilename | DeadlockModworks.exe |
| InternalName | DeadlockModworks |
| Comments | build 1b9bb6d5325a8e4f5fe17071f72cf7d130ff0a4e |

CMake `project(DeadlockModworks VERSION 0.1.0)` writes `product_version.hpp`, `app.rc`, and `product_package_name.txt`. About, the window titles, the Vulkan application name, the failure dialog, CLI usage, and `--version` use that header. The resource script has version strings and no icon. The IDE window is still created at 100,100 with size 1440x960, `WS_OVERLAPPEDWINDOW`, and `WS_EX_ACCEPTFILES`. `io.IniFilename`, the dock builder, and theme colors were not changed. Win32 class atoms stay `DeadlockModelPreview` and `DeadlockSkeletonRepairWindow`.

`Library.DefaultDataRootIsDeadlockModManager` calls `LocalModStore::DefaultRoot()` and checks the path string only. The filename is `DeadlockModManager`. The test does not construct a store at that path and does not open the live library. No `DeadlockModworks` data directory was added, and there is no migration.

### Alias decision

`git tag --list` printed nothing. `git ls-remote --tags origin` printed nothing. `gh release list --limit 20` printed nothing. Nothing was published under `DeadlockSkeletonRepair.exe`, so this stage does not ship a second executable or a compatibility copy of the old name.

`scripts/Publish.ps1` still recursively deletes its output directory before copying. That behavior is unchanged and was not run. The script now reads the executable and zip names from the CMake file `build/release/product_package_name.txt` (`DeadlockModworks.exe` and `DeadlockModworks-0.1.0-win-x64.zip`). The default staging directory is `dist/DeadlockModworks`. `.github/workflows/build.yml` uploads artifact `DeadlockModworks-0.1.0-win-x64` from `dist/DeadlockModworks-0.1.0-win-x64.zip`.

## Remaining blockers

Stage 1 items 1 and 2 are closed by the run above. These are still open. `v0.1.0` is still not tagged.

3. Saved docking cannot restore. `src/preview/main.cpp` sets `io.IniFilename = nullptr` and, on the first frame, calls `DockBuilderRemoveNode` and rebuilds the 17% Explorer split. Relaunching always discards a rearranged layout. Reset of that layout is the same first-frame path. There is no separate data-root switch, so this was not exercised against a copy of the live library.

4. The embedded icon resource is closed in stage 3 below. Explorer, the title bar, the taskbar, and Alt-Tab were not observed. The GUI was not launched.

5. Interactive GUI acceptance has no safe profile. `LocalModStore::DefaultRoot` remains `%LOCALAPPDATA%\DeadlockModManager` with no command-line or environment override. That compatibility path is intentional for 0.1. On this machine that directory already contains `library.db` (1,221,091,328 bytes), `store\catalog.sqlite` (7,467,008 bytes), `mods`, `cache`, and `thumbnails`. `gallery_settings` already has `legacy_store_merged=1`, `legacy_collection_merged=1`, and `library_db_merged=1`. A launch still materializes sources into `cache` and persists the gallery tables on exit. Viewport, button-level build, and a screenshot stay NOT RUN until a later change can aim the GUI at a disposable directory.

6. `scripts/Publish.ps1` deletes the whole output directory it was given (`Remove-Item -Recurse -Force` on `$OutputDirectory`) before copying the executable. It does not run tests. It was not executed. The public exe and zip names now follow the CMake identity. The CI workflow builds, runs the two test executables, and uploads that ZIP. It does not invoke `architecture_diagram.py` itself. Local CTest registered that test only because Python 3 with `networkx` was found at configure time.

7. The repository root has no `LICENSE`. The owner decision for publication is MIT. Third-party notices do not replace it. The file was intentionally not added in this stage.

## Stage 3 program icon

Worktree: `C:\Users\warri\.ao\data\worktrees\deadlock-model-updater\deadlock-model-updater-11`. Branch `codex/modworks-v0.1`. Started from `75883af` (`feat: name the product Deadlock Modworks 0.1.0`). No reset or rebase. Cherry-pick of `48e60155330cc8afc639387869856e472c9d42ae` (`feat: add Deadlock Modworks icon assets`, from `codex/modworks-v0.1-icon`) applied cleanly as `22c9426078ef913ca5d30ca5af7b490fe802d93a`. That commit adds only `assets/branding/modworks.svg`, `modworks-256.png`, `modworks.ico`, and `README.md`. The artwork was not redrawn.

The GUI was not launched. `DeadlockModworks.exe` and `ModelPreview.exe` were not started. `%LOCALAPPDATA%\DeadlockModManager` was not opened. `scripts/Publish.ps1` was not executed. No tag was created and nothing was pushed.

`cmake/app.rc.in` keeps the CMake version strings and adds `1 ICON` for `assets/branding/modworks.ico`. `ModelPreview` is still named `ModelPreview`. It links `cmake/app_icon.rc.in`, which is the same icon id and not the product `VERSIONINFO`. `src/preview/main.cpp` loads resource 1 into the model-tools window class (`hIcon` and `hIconSm`) and sends `WM_SETICON` for `ICON_BIG` and `ICON_SMALL`. The class atom is still `DeadlockModelPreview`. The window is still created at 100,100 with size 1440x960, `WS_OVERLAPPEDWINDOW`, and `WS_EX_ACCEPTFILES`. Product name, version, theme colors, `io.IniFilename`, and the dock builder were not changed. The repair-only window in `src/app/main_window.cpp` still uses `IDI_APPLICATION`. The product build does not open that window.

Configure ran while `HEAD` was `22c9426078ef913ca5d30ca5af7b490fe802d93a`, so the version comment embeds that revision. The wiring commit is the next one. The next configure embeds whatever `git rev-parse HEAD` returns then.

Commands, from this worktree, exit code 0:

```powershell
cmake --preset windows-release -DSR_BUILD_MODEL_TOOLS=ON -DSR_TRACY=OFF
cmake --build --preset windows-release --target DeadlockModworks ModelPreview skeleton_repair_tests render_tests
cmake --preset windows-release -DSR_BUILD_MODEL_TOOLS=ON -DSR_TRACY=OFF
cmake --build --preset windows-release --target ModelPreview
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
```

The second configure and `ModelPreview` build moved that target from the full `app.rc` onto the icon-only script. `DeadlockModworks.exe` was not relinked by that second build.

| Command | Result |
| --- | --- |
| First configure | Exit 0. Configuring done in 20.8 seconds. Visual Studio 18 2026, Windows SDK 10.0.26100.0, MSVC 19.51.36260.0. `SR_BUILD_MODEL_TOOLS=ON`, `SR_TRACY=OFF`. Vulkan 1.4.357 found, including `glslc`. Tracy profiler disabled |
| Product and test build | Exit 0 in 131.5 seconds. Targets `DeadlockModworks`, `ModelPreview`, `skeleton_repair_tests`, and `render_tests`. Output `build\release\bin\Release\DeadlockModworks.exe` |
| Second configure | Exit 0. Configuring done in 4.5 seconds. Same preset and cache flags |
| `ModelPreview` rebuild | Exit 0. `build\release\bin\Release\ModelPreview.exe` |
| `skeleton_repair_tests.exe --gtest_brief=1` | 215 tests from 22 suites ran. 215 passed. 5867 ms. Exit 0. The summary had no skipped line |
| `render_tests.exe --gtest_brief=1` | 50 tests from 11 suites ran. 50 passed. 297 ms. Exit 0. The summary had no skipped line |
| `architecture_diagram.py --check` | Exit 0. `58 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |

The icon check loaded each exe with `LoadLibraryExW` and `LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE`. That maps the file. It does not start the process. `ExtractIconExW` then read icon index 0 and returned a large icon and a small icon. Those handles were destroyed. `FileVersionInfo` read the version block the same way.

`assets/branding/modworks.ico` is 13,529 bytes and lists 16, 20, 24, 32, 48, 64, 128, and 256.

| Executable | Group icon id 1 | Version block | Shell extract |
| --- | --- | --- | --- |
| `DeadlockModworks.exe` | 8 frames, 32 bpp: 16, 20, 24, 32, 48, 64, 128, 256. Group resource 118 bytes | Present, 744 bytes | `ExtractIconExW` count 1. Index 0 returned both a large and a small icon |
| `ModelPreview.exe` | Same 8 frames and the same widths | Absent. `FileDescription`, `ProductName`, `OriginalFilename`, and `FileVersion` are empty | Same extract result. The target name is still `ModelPreview` |

Windows file properties of `DeadlockModworks.exe`, read without launching it:

| Property | Value |
| --- | --- |
| FileDescription | Deadlock Modworks 0.1.0 |
| FileVersion | 0.1.0.0 |
| ProductName | Deadlock Modworks |
| ProductVersion | 0.1.0 |
| OriginalFilename | DeadlockModworks.exe |
| InternalName | DeadlockModworks |
| Comments | build 22c9426078ef913ca5d30ca5af7b490fe802d93a |

| Check | Result (PASS/FAIL/BLOCKED/NOT RUN) | Commit/artifact | Environment | Evidence/notes |
| --- | --- | --- | --- | --- |
| Icon resource in `DeadlockModworks.exe` | PASS | `22c9426` plus the wiring in this commit | This worktree, Release, MSVC 19.51.36260.0 | Group icon id 1 has all eight sizes. Version strings still come from CMake. The exe was not started |
| Icon resource in `ModelPreview.exe` | PASS | Same | Same | Same eight sizes. No product `VERSIONINFO`. The exe was not started |
| Runtime window icon | NOT RUN | Same | Live library not opened | The model-tools window class loads resource 1 for the large and small icons. The window was not created |
| Explorer, title bar, taskbar, Alt-Tab | NOT RUN | Same | GUI not launched | Shell extraction shows the resource is readable. A desktop observation was not made |

## Stage 7 portable package

Worktree: `C:\Users\warri\.grok\worktrees\repos-deadlock-model-updater\subagent-01a1335d-3509-7e20-b30e-a9f10342787e`. Branch `codex/modworks-v0.1-package`, created from `4ff8308130a0e482dccd0d1c9afcc34f2562edde` (`feat: embed the Deadlock Modworks icon`). This stage closes the stage 2 packaging blocker: `Publish.ps1` no longer deletes `build/release` or an arbitrary output directory. The GUI was not launched. `%LOCALAPPDATA%\DeadlockModManager` was not opened. Nothing was pushed, tagged, or published. `gh release create` was not run.

The packaged executable embeds `4ff8308130a0e482dccd0d1c9afcc34f2562edde` because configure ran before these commits. `--version` does not open a store.

### Runtime

Static MSVC runtime. Release vcxproj `RuntimeLibrary` is `MultiThreaded` (`/MT`) for `DeadlockModworks`, `skeleton_repair_tests`, `render_tests`, `gtest`, `libzstd_static`, and `meshoptimizer`. Debug is `MultiThreadedDebug` (`/MTd`). `gtest_force_shared_crt` is OFF. `ZSTD_USE_STATIC_RUNTIME` is ON. Older subprojects ignore `MSVC_RUNTIME_LIBRARY`, so the per-config C/CXX flags also pass `/MT` or `/MTd`.

`dumpbin /nologo /dependents` from `C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe` (vswhere `-prerelease`):

```
SHELL32.dll
ole32.dll
bcrypt.dll
ADVAPI32.dll
vulkan-1.dll
COMDLG32.dll
KERNEL32.dll
USER32.dll
GDI32.dll
IMM32.dll
```

No `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `MSVCP140.dll`, or `api-ms-win-crt-*`. The zip ships no runtime DLL. `vulkan-1.dll` is the graphics-driver loader and is not in the zip. If a static build imported a VC runtime DLL, or a non-system DLL was missing beside the exe, `Publish.ps1` would fail. Dynamic CRT was not needed.

### Commands

From this worktree. Exit code 0 unless noted. This PowerShell execution policy rejects `.\scripts\Publish.ps1` unless launched with `-ExecutionPolicy Bypass`.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
ctest --test-dir build/release -C Release --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Publish.ps1
```

| Command | Result |
| --- | --- |
| Configure | Exit 0. Configuring done in 19.9 seconds. Visual Studio 18 2026, Windows SDK 10.0.26100.0, MSVC 19.51.36260.0. Vulkan 1.4.357 found, including `glslc`. Tracy disabled. `product_package_name.txt` is `DeadlockModworks.exe`, `DeadlockModworks-0.1.0-win-x64.zip`, `0.1.0`, `static` |
| Full Release build | Exit 0. `build\release\bin\Release\DeadlockModworks.exe` |
| `skeleton_repair_tests.exe --gtest_brief=1` | 215 tests from 22 suites ran. 215 passed. 5946 ms. Exit 0 |
| `render_tests.exe --gtest_brief=1` | 50 tests from 11 suites ran. 50 passed. 328 ms. Exit 0. No skipped line |
| `architecture_diagram.py --check` | Exit 0. `58 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |
| CTest Release | 266 passed, 0 failed, 11.71 seconds. Last test `dependency_levels` |
| `Publish.ps1` | Exit 0. Reused `build/release`. Did not delete it |

Unsafe destinations were rejected before configure, with no recursive delete. A sentinel file outside the repo and `build\unsafe-out\sentinel.txt` were still present after those runs and after packaging. A later package run left `dist\KEEP.txt` in place. `build\release\CMakeCache.txt` was still present.

### Package

`scripts\Publish.ps1` builds first, then stages under `build\package-staging\<id>` and writes the zip. It rejects missing caller directories, paths that resolve outside the repository, filesystem and repository roots, paths that overlap the build or sources, and caller directories that already contain unrelated files. It deletes only a previous package zip, its `.sha256`, a `.partial` zip, and staging directories it marked under `build\package-staging`.

Final artifact, not committed:

| Item | Value |
| --- | --- |
| Zip | `dist\DeadlockModworks-0.1.0-win-x64.zip` |
| Bytes | 4,213,760 |
| SHA-256 | `f5e572864f685c0a425b90ed61cc7dd4ec52a55e9ab967938bd21714a0872f7b` |
| Sidecar | `dist\DeadlockModworks-0.1.0-win-x64.zip.sha256` |
| Exe bytes | 4,172,800 |
| Exe SHA-256 | `1f17b684f15b0a4e87fa8889d846d315a085b3db69a2126eff23baff496e94a3` |

Extracted layout: `DeadlockModworks.exe`, `README.md`, `THIRD_PARTY_NOTICES.md`, `PACKAGE_MANIFEST.txt`, and `licenses\` for ValveResourceFormat, lz4, zstd, nlohmann/json, meshoptimizer, Dear ImGui, and Tracy. No `LICENSE` (the root file is still absent; that did not fail the package). No loose shaders, game assets, tests, PDBs, or profiler data. Manifest file sizes and SHA-256 values matched the extracted files (0 mismatches).

Extracted `DeadlockModworks.exe --version` exited 0 and printed `Deadlock Modworks 0.1.0 (4ff8308130a0e482dccd0d1c9afcc34f2562edde)`. File description `Deadlock Modworks 0.1.0`, file version `0.1.0.0`, product `Deadlock Modworks` `0.1.0`, original filename `DeadlockModworks.exe`, internal name `DeadlockModworks`. The process was not left running and no store path was passed.

### CI

`.github/workflows/build.yml` installs Python 3.12 and `networkx`, configures, builds, runs `ctest --test-dir build/release -C Release --output-on-failure`, then both test executables, then `py -3 scripts/architecture_diagram.py --check`, then `Publish.ps1`. The artifact includes the zip and the `.sha256` file.

`.github/workflows/release.yml` is push-only for tags `v*`. It repeats those checks, uploads the same artifact, and runs `gh release create --draft`. It was not executed. Hosted CI does not launch the Vulkan window. `render_tests` skips when no Vulkan 1.3 device exists; a skip is not a unit-test failure. There is no separate GPU smoke job.

| Check | Result (PASS/FAIL/BLOCKED/NOT RUN) | Commit/artifact | Environment | Evidence/notes |
| --- | --- | --- | --- | --- |
| Release configure and full build | PASS | `4ff8308` plus this stage | VS 2026, MSVC 19.51.36260.0, Vulkan SDK 1.4.357.0 | Preset `windows-release`. Static `/MT`. Exit 0 |
| Core tests | PASS | Same | Same | 215/215 |
| Rendering tests | PASS | Same | RTX 4070 SUPER present locally | 50/50, no skip. Hosted CI may skip device tests |
| Dependency architecture check | PASS | Same | `py -3`, networkx already installed | Script and CTest `dependency_levels`. CTest 266/266 |
| `dumpbin /dependents` | PASS | `build\release\bin\Release\DeadlockModworks.exe` | VS 2026 dumpbin | Static CRT. Ten imports. No VC runtime DLL |
| Package zip and checksum | PASS | `dist\DeadlockModworks-0.1.0-win-x64.zip` | This worktree | SHA-256 `f5e572864f685c0a425b90ed61cc7dd4ec52a55e9ab967938bd21714a0872f7b` |
| Extracted `--version` | PASS | Same zip | No data-root flag exists | Printed 0.1.0 and `4ff8308`. Store not opened |
| Unsafe destination rejection | PASS | `scripts\Publish.ps1` | Sentinels kept | Outside, root, relative-escape, overlap, missing, and non-empty paths rejected |
| Clean-machine launch | NOT RUN | Zip above | No machine without VS, the Vulkan SDK, and a dev PATH | Offline double-click was not run. `--version` on this machine is not that check |
| Code signing | NOT RUN | | | No certificate. Signing stays deferred |
| Draft GitHub release | NOT RUN | `.github/workflows/release.yml` | Not executed from this machine | Tag `v*` would upload the zip and open a draft. No tag was pushed |

## Milestone 6 storage failures

Worktree: `C:\Users\warri\.grok\worktrees\repos-deadlock-model-updater\subagent-01a1335d-3509-7e20-b30e-a9ed0fb530dc`. Branch `codex/modworks-v0.1-storage`, started from `4ff8308130a0e482dccd0d1c9afcc34f2562edde`. No reset or rebase. `src/preview/`, `scripts/Publish.ps1`, and `.github/` were not edited. `LocalModStore::DefaultRoot` is still `%LOCALAPPDATA%\DeadlockModManager`. The live library was not opened. `DeadlockModworks.exe` and `ModelPreview.exe` were not started. `scripts/Publish.ps1` was not executed. No tag was created and nothing was pushed.

The process `TEMP` and `TMP` directories were left unchanged. The archive tests passed there, so `build/test-temp` was not required. CMake was not changed. Schema version stays 2. No table and no `DropObservation` signature change.

Commands, from this worktree:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --target skeleton_repair_tests
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
```

| Command | Result |
| --- | --- |
| `cmake --preset windows-release` | Exit 0. Configuring done in 21.4 seconds. Visual Studio 18 2026, Windows SDK 10.0.26100.0, MSVC 19.51.36260.0. Vulkan 1.4.357 found. Tracy profiler disabled. `build/release` had no stale cache |
| `skeleton_repair_tests` build | Exit 0. Cold build 81.45 seconds. The tested executable is `build\release\bin\Release\skeleton_repair_tests.exe` after the later incremental rebuild, also exit 0. This target does not link Vulkan |
| `skeleton_repair_tests.exe --gtest_brief=1` | 224 tests from 22 suites ran. 224 passed. 6355 ms. Exit 0. The summary had no skipped line. Floor before this stage was 215 |
| `architecture_diagram.py --check` | Exit 0. `58 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |
| `render_tests` | NOT RUN. This stage edited `.cpp` files and tests only. No rendering source and no shared header included by that target |

Synthetic fixtures only. No Deadlock install and no network.

| Check | Result (PASS/FAIL/BLOCKED/NOT RUN) | Commit/artifact | Environment | Evidence/notes |
| --- | --- | --- | --- | --- |
| Truncated or corrupt catalog is not replaced | PASS | This commit | This worktree, Release, synthetic catalog | `Store.DamagedCatalogIsNotReplaced`. A same-size non-SQLite file, a 4-byte file, a half-truncated catalog, and a catalog with `snapshots` dropped all throw `The model library is damaged and was not replaced.` The main file bytes stay. Restoring the bytes reopens the one stored model. `Store.UnsupportedSchemaIsRejected` still requires the exact unsupported-schema message and leaves schema version `3` in place |
| Gallery-only catalog still initializes | PASS | This commit | Same | `LibraryTest.LocalStoreExportsAndMergesSharedLibraries` saves a thumbnail before the first import. That creates gallery tables without `meta`. Open still creates the content schema |
| Truncated blob is replaced on import | PASS | This commit | Same | `Store.TruncatedBlobIsRestoredOnImport`. A short blob and a same-size flipped blob are rewritten from the imported payload. One blob file remains and reads back |
| Export collision, missing dependency, and write failure | PASS | This commit | Same, process temp directory | `LibraryTest.RefusesOverwritesAndCancellationDoesNotPublish` was already present and passed again. `LibraryTest.FailedExportLeavesEarlierOutputAndSources` keeps the first VPK and both sources when a later export lacks a dependency or its parent path is a file. `LibraryTest.SharedExportRefusesCollisionAndCancelledExportPublishesNothing` refuses an existing file and an existing directory, leaves their bytes, and a pre-cancelled export of an empty or populated library creates no destination and no `library-*.tmp`. A following export to a new name writes user version 2. `LibraryTest.SaveFailureLeavesThePreviousLibrary` holds the destination open without delete sharing; the previous `library.json` bytes stay and `library.json.saving` is removed |
| Bad or cancelled import keeps earlier snapshots | PASS | This commit | Same | `Store.BadArchiveDoesNotDropAnEarlierSnapshot` and `LibraryTest.FailedImportKeepsAnEarlierSnapshot`. A damaged VPK and a pre-cancelled import leave the first snapshot readable. `ImportFolder` records the bad archive and keeps both good snapshots. Removing the source files still reads the stored path |
| Stored mod after the original path is gone | PASS | This commit | Same | Existing coverage ran in this suite: `Store.ReadSnapshotPathReturnsPayloadsAfterTheSourceFileIsGone`, `Store.AssembleWritesOneModelAndItsResolvedTexture`, `LibraryTest.LocalStoreKeepsImportedModsAfterOriginalIsRemoved`, and `LibraryTest.LocalStoreFlattensChunkedVpkIntoSelfContainedCopy`. No separate hole was found |
| Supported repair failure keeps the input snapshot | PASS | This commit | Same | `Store.SupportedRepairFailureKeepsTheInputSnapshot`. The output parent is a file, and a pre-cancelled repair publishes nothing. Snapshot count stays 2, transforms stay 0, and the input bytes remain readable. No `.new` or `.bak-` file is left |
| Assemble failure leaves the snapshot and earlier output | PASS | This commit | Same | `Store.AssembleFailureLeavesSnapshotAndExistingOutput`. Collision, pre-cancel, a parent path that is a file, and a missing blob file leave the snapshot and the existing marker. No partial `.new` file |
| In-flight cancel between two folder imports | NOT RUN | This commit | Cancellation is an atomic flag polled by the caller | A pre-cancelled `ImportFolder` and a bad file after a committed import were tested. There is no hook to cancel exactly after one file commits and before the next starts without a race |
| In-flight cancel after a shared-library commit and before publish | NOT RUN | This commit | Same polling limit | Pre-cancelled export, including an empty library, publishes nothing. The check immediately before `MoveFileExW` was not timed separately |
| Corrupt `settings.json` | NOT RUN | Unchanged preview code | `src/preview/` is owned by another stage | `LoadLibrarySettings` returns empty settings on a parse failure. `HistoryCompare.SettingsFileRoundTripsAndCorruptFilesStayEmpty` locks that. This stage did not change it |
| Full-disk or read-only volume fault injection | NOT RUN | This commit | No volume fault harness | Write failure was a locked `library.json`, a destination that already exists, and a parent path that is a file |
| Live library upgrade and GUI restart | NOT RUN | Not opened | `%LOCALAPPDATA%\DeadlockModManager\library.db` is about 1.2 GB. GUI exit rewrites it | No copy, open, or write. Startup on a damaged catalog was proven only through `ContentStore::Open` in the test process |
| Real-mod manual import, preview, export, and repair | NOT RUN | Synthetic VPKs only | No Deadlock install was used | Representative installed mods were not part of this run |
| `render_tests` | NOT RUN | Not built this stage | No rendering or shared-header edit | A Vulkan SDK miss did not occur. Configure found Vulkan 1.4.357. `skeleton_repair_tests` does not link it |
| `scripts/Publish.ps1`, package, clean-machine launch | NOT RUN | This stage did not run the script | Stage 7 on the other parent replaced the recursive delete | Clean-machine launch is still NOT RUN. The package SHA from stage 7 is not a storage-stage result |
| Marketplace, installer, deployment manager, new brand data directory | NOT RUN | Not implemented | Schema stays version 2. Default root stays `DeadlockModManager` | Those are outside this storage stage |

## Stage 4 theme and saved layout

Worktree: `C:\Users\warri\.grok\worktrees\repos-deadlock-model-updater\subagent-01a1335d-3509-7e20-b30e-a9d4eecaf84b`. Branch `codex/modworks-v0.1-theme`, started from `4ff8308130a0e482dccd0d1c9afcc34f2562edde`. The commit that contains this section is `feat: apply the steel-blue workspace theme and restore saved layout`. Nothing was pushed, tagged, or published. `scripts/Publish.ps1` was not run.

`LocalModStore::DefaultRoot()` is still `%LOCALAPPDATA%\DeadlockModManager`. The GUI resolves `--data-root`, then `DEADLOCK_MODWORKS_DATA_ROOT`, then that default. The live `library.db` stayed 1,221,091,328 bytes with `LastWriteTimeUtc` `2026-10-12T15:24:28.2335301Z` before and after every launch below.

Final tokens, sRGB, also commented in `src/preview/theme.hpp`. Contrast is WCAG relative luminance against the named surface.

| Token | Value | Measured pair |
| --- | --- | --- |
| Workspace / clear | `#141B23` | Primary text `#E6EDF3` is 14.68:1 |
| Panel | `#1D2833` | Primary text 12.66:1. Secondary text `#9BAEBE` is 6.55:1 |
| Border | `#344657` | 1px dividers and control borders |
| Accent mark | `#4682B4` | Borders, docking preview, scrollbar active. Not a text color and not a button fill (`#E6EDF3` on it is 3.48:1) |
| Focus / text accent / selection border | `#659BC3` | 5.01:1 on the panel, 5.81:1 on the workspace |
| Raised control | `#2A3948`, hover `#3A4C5E`, pressed `#243244` | Ordinary buttons |
| Muted selection | `#1E3C56`, hover `#2A4E70`, active `#345E86` | Selected, hovered, and pressed stay apart |
| Primary action | fill `#2A5678`, hover `#346888`, pressed `#1E4460`, text `#E6EDF3` | 6.58:1, 5.10:1, and 8.67:1. Used for `Add mods...` and `Build VPK...` |
| Disabled text | `#8B9DAB` | 5.35:1 on the panel, darker than `#9BAEBE` |
| Success / warning / failure | `#6BCC85` / `#F2B357` / `#F56B66` | 7.55:1, 8.09:1, and 5.11:1 on the panel. Text stays labeled |
| Viewport and swapchain clear | `#141B23` (`20/255, 27/255, 35/255`) | Same as the workspace. The Win32 class brush is `RGB(0x14, 0x1B, 0x23)` |

Density at 100% before the one `ScaleAllSizes` call: window padding 6, control padding 6x3, item spacing 6x4, 1px dividers, radii 0–3. Segoe UI stays 15px times the DPI scale.

Commands, from this worktree, after a fresh `build/release` configure (no previous cache):

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/release -C Release --output-on-failure
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
py -3 scripts/architecture_diagram.py
```

| Command | Result |
| --- | --- |
| Configure | Exit 0. Visual Studio 18 2026, Windows SDK 10.0.26100.0, MSVC 19.51.36260.0. Vulkan 1.4.357 found. Tracy disabled. About 19.4 seconds |
| Release build | Exit 0. `DeadlockModworks.exe` and `ModelPreview.exe` under `build\release\bin\Release` |
| CTest Release | Exit 0. 267 passed, 0 failed, 11.50 seconds. Includes `Library.ExplicitDataRootIsWhatTheStoreOpens` and `dependency_levels` |
| `skeleton_repair_tests.exe --gtest_brief=1` | Exit 0. 216 tests from 22 suites passed. 5884 ms. No skipped line |
| `render_tests.exe --gtest_brief=1` | Exit 0. 50 tests from 11 suites passed. 259 ms. No skipped line |
| `architecture_diagram.py --check` | Exit 0. `59 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |
| Diagram regenerate | Exit 0. `docs/architecture.svg` rewritten. `theme` is in the graph |

The GUI was launched only as `DeadlockModworks.exe --data-root build\theme-gui-data` (an empty directory under this worktree). `ModelPreview.exe` was not launched. The disposable root received `layout.ini` (785 bytes, Explorer docked at about 19% on the first run), `window.ini`, and `store\catalog.sqlite` (155,648 bytes). No live library file was copied there.

A second launch kept an edited Explorer size of `480,739` instead of rebuilding the 17% split. The catalog length stayed 155,648. `DeadlockModworks.exe --data-root` with no directory exited 1 and showed `--data-root needs a directory.` The live library timestamp did not change.

Side-by-side comparison of two models was not captured. The empty library has no `.vmdl_c` to open, and no game asset was copied in.

| Check | Result (PASS/FAIL/BLOCKED/NOT RUN) | Commit/artifact | Environment | Evidence/notes |
| --- | --- | --- | --- | --- |
| Data-root override | PASS | This commit | Release, MSVC 19.51.36260.0, Windows 11 10.0.26200 | `Library.ExplicitDataRootIsWhatTheStoreOpens` passed. The GUI catalog was created only under `build\theme-gui-data` |
| Theme tokens and clear color | PASS | This commit | Same | Tokens in the table above. Captures show the steel-blue shell, panel, and primary `Add mods...` button |
| Saved dock layout | PASS | This commit | Same | `layout.ini` kept Explorer and Workspace. A relaunch preserved Explorer width 480 instead of resetting to the default split |
| Reset layout keeps the library | PASS | This commit | Menu item was not clicked | `ResetLayout` calls `std::filesystem::remove` only on `layout.ini`, then rebuilds the dock. A relaunch left `store\catalog.sqlite` at 155,648 bytes |
| Library page | PASS | `docs/screenshots/library.png` | Disposable root, CopyFromScreen | Empty library, explorer on the left, `Add mods...` |
| VPK builder | PASS | `docs/screenshots/builder.png` | Ctrl+2 | Empty pack, `Build VPK...` |
| Version history | PASS | `docs/screenshots/history.png` | Ctrl+3 | Models and packs tables, no rows |
| Skin repairs | PASS | `docs/screenshots/repairs.png` | Ctrl+4 | Ready to analyze, 0 archives |
| Model inspector | PASS | `docs/screenshots/inspector.png` | Ctrl+5 | Empty inspector. No model was loaded |
| Two-model comparison | NOT RUN | — | No disposable model | The inspector page is empty. No `.vmdl_c` was added |
| Shortcuts dialog | PASS | `docs/screenshots/shortcuts.png` | F1 | Modal lists View > Workspace and View > Reset layout |
| Missing `--data-root` value | PASS | `docs/screenshots/data-root-error.png` | Exit 1 | Message box: `--data-root needs a directory.` Store was not opened |

## Milestone 5 fresh startup and first-pack result

Worktree: `C:\Users\warri\.grok\worktrees\repos-deadlock-model-updater\subagent-01a13381-7f6e-7b51-aa6b-1298a851bc88`. Branch `codex/modworks-v0.1-startup`, started from `9d9262df2e5a57e455bf88d81c5399c17b0fbe67`. The commit that contains this section is `feat: guide an empty library and report startup failures`. Nothing was pushed, tagged, merged, or published. `scripts/Publish.ps1` was not run. Schema stays version 2. The damaged-catalog sentence is unchanged.

`LocalModStore::DefaultRoot()` is still `%LOCALAPPDATA%\DeadlockModManager`. Resolution order is still `--data-root`, then `DEADLOCK_MODWORKS_DATA_ROOT`, then that default. The live `library.db` stayed 1,221,091,328 bytes with `LastWriteTimeUtc` `2026-10-12T15:24:28.2335301Z` before and after every launch below. It was not opened, copied, or written.

The launched exe was configured from that parent SHA, so `--version` prints `Deadlock Modworks 0.1.0 (9d9262df2e5a57e455bf88d81c5399c17b0fbe67)`. `vulkan-1.dll` is a delay-load dependency of `DeadlockModworks.exe` and `ModelPreview.exe` (`dumpbin /dependents`). `ModelPreview.exe` was not launched.

Commands, from this worktree. The first `cmake --build --preset windows-release` failed in `tests/startup_tests.cpp` because `std::filesystem::current_path` returns void. After that fix, one startup test saved a library before treating the catalog as read-only. The full preset build below is the later incremental build, exit 0.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/release -C Release --output-on-failure
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
py -3 scripts/architecture_diagram.py
```

| Command | Result |
| --- | --- |
| Configure | Exit 0. Visual Studio 18 2026, Windows SDK 10.0.26100.0, MSVC 19.51.36260.0. Vulkan 1.4.357 found. Tracy disabled. About 21.8 seconds. No previous `build/release` cache |
| Release build | Exit 0 on the incremental `cmake --build --preset windows-release` after the test fixes. `DeadlockModworks.exe` and `ModelPreview.exe` under `build\release\bin\Release` |
| CTest Release | Exit 0. 283 passed, 0 failed, 11.90 seconds. Includes `Library.ExplicitDataRootIsWhatTheStoreOpens`, `dependency_levels`, and the seven startup tests |
| `skeleton_repair_tests.exe --gtest_brief=1` | Exit 0. 232 tests from 24 suites passed. 7270 ms. No skipped line. 225 of those were already on the parent (224 plus `Library.ExplicitDataRootIsWhatTheStoreOpens`). None of those were removed |
| `render_tests.exe --gtest_brief=1` | Exit 0. 50 tests from 11 suites passed. 278 ms. No skipped line |
| `architecture_diagram.py --check` | Exit 0. `60 modules: no cycles, layering holds, stable dependencies hold within 0.10 and 1 accepted exception(s).` |
| Diagram regenerate | Exit 0. `docs/architecture.svg` rewritten. `startup` is in the graph |

New tests: `StartupTest.DataPathsStayUnderTheRootWhenThePathHasASpace`, `StartupTest.UnicodeDataRootKeepsTheStoreBesideThatRoot`, `StartupTest.GamePathUsesTheCommandLineThenTheSavedPath`, `StartupTest.CorruptSettingsDoNotReplaceTheCatalog`, `StartupTest.ReadOnlyCatalogIsNotReplaced`, `StartupTest.ErrorLogIsUnderTheDataRootAndStartupCanStayQuiet`, `StartupMessages.QuickStartAndGameNeedsStaySpecific`.

GUI launches used only `--data-root` directories under this worktree. The empty-library launch was `DeadlockModworks.exe --data-root build\startup gui путь` with the working directory `build\startup-gui-cwd`. That root has a space and the Cyrillic word путь. A later `WM_CLOSE` exited 0 and left `layout.ini` (577 bytes), `window.ini` (20 bytes), and `store\catalog.sqlite` (155,648 bytes) under that root. The working directory stayed empty. `logs\modworks.log` was not created. `gallery_sources` count was 0. `meta.schema_version` was 2. `gallery_settings.game_archive` was `c:\program files (x86)\steam\steamapps\common\Deadlock\game\citadel\pak01_dir.vpk`.

| Check | Result (PASS/FAIL/BLOCKED/NOT RUN) | Commit/artifact | Environment | Evidence/notes |
| --- | --- | --- | --- | --- |
| Empty library actions | PASS | `docs/screenshots/startup-empty-library.png` | Release, disposable root, CopyFromScreen | The library page shows `Your library has no mods yet.`, the paragraph that names which actions need the game archive, `Building a pack does not install it.`, and the buttons `Connect game`, `Add mods`, and `Open quick start`. Explorer says to use those actions and shows `Connect` and `Browse...` |
| Quick start popup | NOT RUN | Steps are unit-tested | The button was not clicked | `StartupMessages.QuickStartAndGameNeedsStaySpecific` checks the eight step titles and the install wording. The popup itself was not opened |
| Steam auto-detect when nothing is saved | PASS | Same capture | A Steam Deadlock install is present | The status line says Deadlock connected. The saved archive is `pak01_dir.vpk` under `steamapps\common\Deadlock\game\citadel`. A typed path and `Browse...` stay on the explorer |
| Library without a connected game | PASS | Missing-game launch | `--game C:\modworks-missing-game\citadel` | The window stayed open. The log says the folder was not found at `C:\modworks-missing-game\citadel\pak01_dir.vpk` and that the model library was not changed. The copied catalog length stayed 4,096 |
| Data root with a space and a Unicode name | PASS | `build\startup gui путь` | Working directory was a different empty directory | `layout.ini`, `window.ini`, and `store\catalog.sqlite` were created only under that root. The working directory gained no files. Covered again by `StartupTest.DataPathsStayUnderTheRootWhenThePathHasASpace` and `StartupTest.UnicodeDataRootKeepsTheStoreBesideThatRoot` |
| Read-only catalog | PASS | `docs/screenshots/startup-readonly-data-root.png` | `build\startup ro data`, catalog file attribute ReadOnly | Message box says the data folder cannot be written and `The model library was not replaced.` Exit 1. Catalog length stayed 4,096 and the read-only attribute stayed. `StartupTest.ReadOnlyCatalogIsNotReplaced` also covers a data root that is a file |
| Corrupt `settings.json` | PASS | `docs/screenshots/startup-corrupt-settings.png` | `{not json` beside a copied catalog | Message box: `settings.json could not be read. Saved preferences were left unchanged, and the model library was not replaced.` The window was still open after the box. The settings text was unchanged. The log under that data root repeats the version, build, data root, and that sentence. `StartupTest.CorruptSettingsDoNotReplaceTheCatalog` keeps one stored source and does not rewrite the corrupt file |
| Missing `vulkan-1.dll` message | NOT RUN | Delay-load is present | This machine has the loader | `dumpbin` shows `vulkan-1.dll` as a delay-load dependency, not a normal import. The message string is covered by `StartupMessages.QuickStartAndGameNeedsStaySpecific`. The DLL was not hidden, so the message box was not shown |
| Unsupported device | NOT RUN | Not forced | The empty-library window opened on the installed device | `SetupVulkan` still reports a device failure through the existing message box. No unsupported adapter was exercised |
| Quiet startup versus an error log | PASS | No log on the empty launch | Errors from the other launches were logged | The empty launch created no `logs\modworks.log`. Corrupt settings, the missing game, and a bad CLI VPK each appended a log under their own data root. `StartupTest.ErrorLogIsUnderTheDataRootAndStartupCanStayQuiet` checks that an empty error writes nothing |
| Open logs and Copy diagnostic details | NOT RUN | Help menu was not clicked | The log file and `DiagnosticText` are covered | Those two menu items were not used. The written log lines include the product title, the build SHA, the data root, and the error |
| `--version` and `--help` | PASS | Exit 0 from `build\startup-cli-cwd` | GUI subsystem exe launched in PowerShell | `--version` printed `Deadlock Modworks 0.1.0 (9d9262df2e5a57e455bf88d81c5399c17b0fbe67)` and created no file. `--help` printed the repair commands and the exit-code lines. The live library timestamp did not change |
| `--data-root` without a directory | PASS | `docs/screenshots/startup-missing-data-root.png` | Exit 1 | Message box: `--data-root needs a directory.` No store was created in the working directory |
| CLI path still opens the window | PASS | `build\startup cli vpk` | A 9-byte file named `not-a-real-mod.vpk` | The window stayed open. The data-root log says `not-a-real-mod.vpk: VPK directory or embedded data is outside the file.` The source file stayed in the working directory |
| Drag-and-drop | NOT RUN | No drop was sent | `WM_DROPFILES` still calls `AddPaths` | A real drop onto the window was not performed |
| Finished pack result | NOT RUN | No pack was built | Assembly wording is unit-tested | No export finished, so `Reveal in Explorer` and the assembled-pack dialog were not shown. `kAssemblyDoesNotInstall` says assembly writes a VPK, does not install the pack, and does not guarantee compatibility. No installer was added |
| First-pack install and remove | NOT RUN | Not this stage | Install steps are summarized only | The quick start text tells the user to install or remove the pack. That action was not performed |
| 100%, 150%, and 200% UI | NOT RUN | Not resized | The capture is the window at its default 1440x960 | DPI steps were not changed |
| Clean extracted zip and `Publish.ps1` | NOT RUN | Script not run | Packaging stays with the earlier stage | No zip was extracted and the publisher was not started |
