# Mod manager checkpoint

Work paused at the user's request on October 11, 2026. The local mod store is work in progress; finish verification before treating it as complete.

## Implemented

- The C++ GUI opens to a responsive thumbnail gallery, with sidebar navigation for the collection, VPK builder, model inspector, and skin repairs.
- Gallery cards support inspection and inclusion in a pack, with search, source filtering, selection highlighting, and a bounded thumbnail cache.
- Combined VPK export follows resource dependencies, deduplicates identical files, blocks conflicts and missing dependencies, and validates the output before publishing it. Base-game dependencies remain in the game. Unreferenced mod extras are not included automatically.
- JSON library files preserve source paths, selected models, and an optional game archive.
- CLion Debug and Release CMake presets use Ninja and a Visual Studio toolchain.

## Local store work in progress

The intended behavior is to retain imported mods independently of their originals and restore the same collection and pack selections on every launch.

- `src/core/mod_store.*` stores verified, embedded VPK copies under `%LOCALAPPDATA%\DeadlockModManager\mods\<random-id>\<original-filename>`.
- Chunked input archives are flattened into a self-contained VPK. Their original chunk files should no longer be required after import.
- Each import has an `origin.json` record. Repeat imports use the original path, size, modification timestamp, and chunk metadata to reuse a cached copy. Changed versions and different sources with the same filename get independent copies.
- `library.json` stores the active collection, selections, and game connection. Saving uses temporary files and an atomic move; a readable previous manifest becomes `library.backup.json`. Loading falls back to that backup.
- The GUI restores saved state on startup, imports in the background, checkpoints successful imports, and autosaves collection and selection changes. Imports can be cancelled; completed copies are retained.
- Removing a source from the active library does not delete its cached VPK. Repairs operate on the stored copy once it is imported.

## Validation so far

- Before local-store work, all 120 core tests and 45 rendering tests passed.
- The gallery refactor built successfully. A running-app screenshot showed real model thumbnails, but full visual verification was stopped by the user with Escape. Do not resume computer-use automation without fresh authorization.
- The initial local-store implementation compiled successfully with `skeleton_repair_tests` and `DeadlockSkeletonRepair`.
- The last edits after that build added chunk metadata for repeat-import detection, per-import manifest checkpoints, and six local-store tests. These latest edits have **not been rebuilt or tested**.

## Remaining work

1. Rebuild the final source and run the core and rendering suites. Fix any failures in the new local-store tests, especially the synthetic chunked-VPK fixture. Expected core count is currently 126 tests.
2. Verify restart behavior end to end: import several real VPKs, choose pack contents, close and reopen the manager, and confirm sources, selections, and the configured game archive are restored.
3. Verify that moving or removing originals and their chunks does not break previews, repairs, or pack export from the stored copies.
4. Exercise duplicate imports, changed originals/chunks, same-name files, cancellation, closing during import, and partially failed batches. Check that completed imports survive restart and invalid imports leave no published partial archive.
5. Exercise manifest recovery and saving failures. Confirm that corrupted state is surfaced, a valid backup is preserved, and permission/disk failures do not silently discard the collection. Review the autosave retry and shutdown paths.
6. Review startup and manual-library-load integration. Saved libraries should remap selected model paths to cached VPKs, and an explicit game choice should not be replaced by automatic detection. Check mixed standalone-model/VPK imports too.
7. Update the README for automatic local storage. It currently still tells users to keep original source files at their saved paths; that guidance needs to distinguish imported local copies from external library files. Document store location, removal semantics, game persistence, backup recovery, and imports of changed versions.
8. Complete visual checks for gallery empty/loading/error states, narrow windows, high DPI, selection changes, and navigation between gallery, builder, inspector, and repairs. The local import/save status text in the sidebar needs review with long errors and paths.

## Useful commands

```powershell
cmake --build build/release --config Release

# The sandbox's default temp folder caused Windows archive publish/replace failures.
# Use a workspace temp directory for test runs in this environment.
New-Item -ItemType Directory -Force -Path build/test-temp | Out-Null
$env:TEMP = (Resolve-Path build/test-temp).Path
$env:TMP = $env:TEMP
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
```

CLion's Ninja compiler probe hung on its directory lock inside the execution sandbox; configuring and building outside the sandbox succeeded. Its validation used existing dependencies from `build/release/_deps` to avoid new downloads. Use an x64 MSVC environment for command-line Ninja builds.

## Scope

All feature changes are in the C++ project. The legacy C# removal was committed separately by another agent. Local `.idea` files are IDE state and are not part of this checkpoint. No changes have been pushed remotely.
