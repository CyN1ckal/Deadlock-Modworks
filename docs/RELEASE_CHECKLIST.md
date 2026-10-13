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

## Remaining blockers

These are reproducible and still open. None of them failed the headless baseline, and none were fixed in this stage. No data-loss bug, startup crash, or broken primary headless workflow showed up in the runs above.

1. Public names disagree. `CMakeLists.txt` names the project `DeadlockSkeletonRepair` and the executable target `DeadlockSkeletonRepair`. `src/preview/main.cpp` creates the window and the Vulkan application as `Deadlock Mod Manager`, and the failure dialog title is `Deadlock Model Tools`. `CommandLine::Execute` usage text starts with `Deadlock Model Updater`. `scripts/Publish.ps1` and `.github/workflows/build.yml` package `DeadlockSkeletonRepair-win-x64.zip`.

2. The version number disagrees with the release name. CMake `PROJECT_VERSION` is `0.1.1`. Remote and local tag lists are empty, so `v0.1.0` is free. This stage did not change the version or create a tag.

3. Saved docking cannot restore. `src/preview/main.cpp` sets `io.IniFilename = nullptr` and, on the first frame, calls `DockBuilderRemoveNode` and rebuilds the 17% Explorer split. Relaunching always discards a rearranged layout. Reset of that layout is the same first-frame path. There is no separate data-root switch, so this was not exercised against a copy of the live library.

4. The window class sets no icon, and the source tree has no application resource script. Explorer, the title bar, and Alt-Tab cannot show a product icon for `DeadlockSkeletonRepair.exe`.

5. Interactive GUI acceptance has no safe profile. `LocalModStore::DefaultRoot` is `%LOCALAPPDATA%\DeadlockModManager` with no command-line or environment override. On this machine that directory already contains `library.db` (1,221,091,328 bytes), `store\catalog.sqlite` (7,467,008 bytes), `mods`, `cache`, and `thumbnails`. `gallery_settings` already has `legacy_store_merged=1`, `legacy_collection_merged=1`, and `library_db_merged=1`. A launch still materializes sources into `cache` and persists the gallery tables on exit. Viewport, button-level build, and a screenshot stay NOT RUN until a later change can aim the GUI at a disposable directory.

6. `scripts/Publish.ps1` deletes the whole output directory it was given (`Remove-Item -Recurse -Force` on `$OutputDirectory`) before copying the executable. It does not run tests. It was not executed. The CI workflow builds, runs the two test executables, and uploads a ZIP. It does not invoke `architecture_diagram.py` itself. Local CTest registered that test only because Python 3 with `networkx` was found at configure time.

7. The repository root has no `LICENSE`. The owner decision for publication is MIT. Third-party notices do not replace it. The file was intentionally not added in this stage.
