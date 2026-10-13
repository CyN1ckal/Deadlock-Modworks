# Deadlock Modworks 0.1: standalone implementation handoff

This file is the complete handoff for an implementation agent. No prior conversation, generated image, or other design attachment is needed. Read it together with the repository's current instructions and source. The written design brief below is the reference for implementation.

Prepared October 12, 2026. Repository at handoff: `C:\Users\warri\source\repos\deadlock-model-updater`, branch `main`, HEAD `8470d845c656fba0c60bcf02644fbb7537a6070a`. Paths below are repository-relative so this file also works in another checkout. Reinspect status and instructions before editing; these observations are a snapshot, not a guarantee that the checkout has remained unchanged.

Implementation branch: `codex/modworks-v0.1`, based on the current project after recording a baseline. Target release: `v0.1.0`, subject to the version-history check below. At handoff only this document has been added: no implementation, new branch, build verification, tagging, or publication was performed for this release effort.

## Assignment and user's intent

Take the existing project from its current state to a finalized, ready-to-publish 0.1 release. Implement the work below when the owner gives you this handoff as an implementation assignment; do not merely produce another plan. If asked only to review the handoff, review it without assuming permission to implement.

The user's explicit preferences are:

- The project has grown from a model updater into a mod manager and needs a coherent new identity.
- The selected name is **Deadlock Modworks**.
- The user likes **steel blue**. They described the sample colors as a good start.
- The user wants an **IDE-like experience**. They rejected the sample's gallery-first application layout as the overall experience.
- The release must include a basic program icon, a palette overhaul, documentation, and easy startup.

The previous visual sample showed an M-shaped icon, cool silver details, dark blue/graphite surfaces, and a skin-card gallery. Its palette and broad icon direction inform this brief. Its gallery-dominated layout is explicitly not the desired application structure. No pixel matching of that image is required. Character artwork in that sample was invented placeholder art and must not be used as a release asset or represented as an available mod.

The specific release defaults in this file (portable ZIP, retaining the data directory, limited workspace changes, and deferred installer) are proposed engineering decisions that make 0.1 achievable. They are not additional verbatim user requirements. Use them unless evidence requires a change; record material deviations and the reason. Do not reopen settled name/color/IDE preferences.

Work through implementation and verification autonomously within the assignment. Preserve unrelated user changes and existing libraries. Complete all unblocked work before reporting limitations. Do not publish, rename the hosted repository, or merge to the release branch merely because this file describes those eventual steps; the immediate deliverable is a reviewable release candidate. Ask the owner about the unresolved license choice while continuing independent work. Do not invent a license decision.

## Read first and orient yourself

1. Read applicable `AGENTS.md` and other active repository instructions; inspect Git status and the current commit. If the proposed branch already exists, inspect it and continue the appropriate work instead of overwriting it.
2. Read `README.md`, `CMakeLists.txt`, `CMakePresets.json`, `cmake/render.cmake`, `scripts/Publish.ps1`, and `.github/workflows/build.yml`.
3. Inspect the source areas listed below before changing behavior. Read `docs/ARCHITECTURE.md` for boundaries. Treat `docs/MOD_MANAGER_HANDOFF.md` as historical context, not an up-to-date description of storage or verified test results.
4. Create or update `docs/RELEASE_CHECKLIST.md` as the execution log: baseline, tasks, failures, verification evidence, outstanding decisions, and candidate artifact paths/checksums.

| Area | Starting points |
| --- | --- |
| Process startup, GUI/CLI dispatch, manifest | `src/app/main.cpp`, `src/app/app.manifest` |
| Main IDE shell, docking, menus, theme, Vulkan initialization | `src/preview/main.cpp` |
| Shared UI helpers and semantic colors | `src/preview/ui.hpp`, `src/preview/ui.cpp` |
| Gallery, selection, thumbnails | `src/preview/library_panel.*`, `library_model.*`, `thumbnail_cache.*` |
| Inspection, comparison, multi-view, repairs, history | Corresponding panel/view files under `src/preview/` |
| Rendering and viewport backgrounds | `src/render/`, `src/preview/model_viewport.*` |
| Persistent library and content versions | `src/core/content_store.*`, `mod_store.*`, `model_library.*`, `model_versions.*`, `store_jobs.*` |
| VPK export and safe archive updates | `src/core/version_builder.*`, `vpk.*`, `archive_transaction.*`, `updater.*` |
| CLI help and execution | `src/core/command_line.*` |
| Legacy repair-only build | `src/app/main_window.*`, `SR_BUILD_MODEL_TOOLS=OFF` |
| Build/package/test integration | `CMakeLists.txt`, `cmake/render.cmake`, `CMakePresets.json`, `scripts/Publish.ps1`, `.github/workflows/build.yml`, `tests/` |

## Standalone visual specification

### Program icon

Create an original, simple vector mark; a fresh image-generation call is not necessary. Use an editable SVG as the source, then rasterize with a documented tool to PNG/ICO. The mark should be recognizable as an **M** and suggest stacked mod packages.

- Composition: a dark rounded-square backing tile, approximately 12% corner radius; an M centered inside with roughly 15% clear margin. Use two pale-silver upright outer strokes and steel-blue diagonal inner strokes meeting at a low center point.
- Behind the M, offset one or two simple blue rectangular/parallelogram tiles upward to suggest a stack. The M remains the dominant silhouette. Avoid making the result resemble a folder with an unrelated letter pasted on top.
- Use `#141B23` for the backing, `#4682B4` and optionally `#659BC3` for blue faces, and `#E6EDF3` for silver/light faces. Flat fills are sufficient. Transparency outside the rounded square is required.
- The visual character is restrained industrial/Art Deco geometry: a little stepped structure is acceptable, but no tiny decorative rails, bevel texture, metallic gradients, glow, gears, wrench, padlock, official game logo, or full product name inside the icon.
- At 16/20/24px simplify or remove rear tiles if necessary. Prioritize a legible M over preserving all large-size geometry. Inspect at actual pixel size.
- Suggested outputs: `assets/branding/modworks.svg`, `modworks-256.png`, `modworks.ico`, and a short asset-generation note. Add Windows resources under `src/app/` and wire them into CMake.
- Use the ordinary product name next to the icon in About/docs. A custom wordmark is optional and must not delay 0.1.

### IDE shell and layout

Preserve and polish the native desktop working environment. No web-style landing page, oversized navigation rail, card dashboard, hero banner, or large branding area in the normal workspace.

```text
+-----------------------------------------------------------------------+
| [M] Deadlock Modworks                         native window controls   |
| File   View   Tools   Help                                             |
| Compact context actions                                               |
+-------------------+---------------------------------------------------+
| Explorer          | Workspace tab strip                               |
| Game              | Mod library | VPK builder | Model inspector | ...  |
| Libraries/mods    +---------------------------------------------------+
| Sources/versions  | Active tool: table/gallery, model viewport,        |
| Tree navigation   | comparison split, pack assembly, history, repairs |
|                   | Context properties beside the active view when    |
|                   | relevant; keep usable space for the main task     |
+-------------------+---------------------------------------------------+
| Game connection | Background operation / result | Selection/status    |
+-----------------------------------------------------------------------+
```

This is a structural sketch, not a demand to duplicate labels that differ in the current source. The existing explorer-left/workspace-right shell is the starting point. Approximately 17-22% left explorer width is appropriate at a normal desktop size. Keep the main editing/inspection region dominant. Preserve existing preview/compare/multi-view tools and shortcuts.

Keep tool tabs within the workspace for 0.1. Context details can stay within existing panels. If a collapsible bottom output area helps expose existing operation results/logs, add it without replacing those tools or forcing it open when idle. A new full document model, independent tab per open model, arbitrary split editors, and separate floating OS windows are not required for this release. The user requested an IDE feel, not a wholesale rewrite.

### Theme implementation details

- Use the palette in milestone 4 as the shared source of truth. Add derived tokens for raised controls, muted selection fill, selected borders, status colors, viewport backgrounds, and focus treatment; document final values.
- Starting density: window padding about 6px, control padding 6x3px, item spacing 6x4px, thin 1px dividers, and 0-3px corner radii at 100% scaling. These are close to existing values. Apply DPI scaling once from the base style, not cumulatively.
- Use a readable system UI font (the app currently loads Segoe UI) around 14-15px at 100%. Reserve decorative styling for the icon; keep controls and labels conventional.
- Keep inactive panels mostly neutral. Use an accent underline for the active tab, a visible keyboard-focus indicator, muted blue row/card selection, and stronger blue sparingly for primary actions. Selected, hovered, focused, and disabled states must remain distinguishable.
- Blue accent colors are not automatically safe button fills with light text. Measure actual combinations, target at least 4.5:1 for normal text, and darken button fills or alter text colors when needed.
- Keep success, warning, and failure visually distinct and text-labeled. Comparison/skeleton visualization colors encode meaning; adjust those only where readability requires it and keep their legends consistent.
- Audit render clear colors as well as ImGui styles so resizing/minimizing/loading does not flash the previous theme.
- Store layouts in the existing user-data location with a stable lifetime for any `io.IniFilename` string. The current first-frame dock builder unconditionally resets nodes: change that logic together with settings loading so saved docking is actually restored. Reset layout must not reset the mod collection.
- Onboarding belongs in an empty-state workspace or small dismissible panel. Returning users should go directly back to their workspace.

Design acceptance: the icon is clear at taskbar size; the interface looks like a compact engineering tool; the palette is consistent across all screens; library cards do not define the entire application; no prior generated image is needed to judge completion.

## Release goal

A Windows x64 user can download a ZIP, extract it, double-click `DeadlockModworks.exe`, connect Deadlock, import mods, inspect and compare models, assemble a validated VPK, and restart without losing their collection or workspace. The application presents a compact IDE-style workspace with a consistent steel-blue identity, an embedded icon, actionable errors, and accurate user documentation.

0.1 describes the capabilities actually shipped: local mod collection, model inspection/comparison, supported repairs, version history, library sharing, and model-pack assembly. Do not imply that all mod types or one-click game installation are supported. Include a verified manual installation/removal guide for exported packs. An integrated deployment manager is deferred.

## Current repository findings

- CMake names the product `DeadlockSkeletonRepair` and sets version `0.1.1`; the main window says Deadlock Mod Manager and some error paths use other names.
- The existing Win32/ImGui/Vulkan GUI already has docking, an explorer, workspace tabs, compact styling, and keyboard shortcuts. Keep and refine this foundation.
- `src/preview/main.cpp` owns the current theme. Shared colors also live in `src/preview/ui.hpp`. ImGui settings persistence is explicitly disabled with `io.IniFilename = nullptr`.
- The window class currently supplies no application icons. No application resource script appeared in the inspected source inventory.
- `scripts/Publish.ps1` builds and packages the old executable name. It recursively deletes a caller-supplied output directory, does not run tests, and does not establish runtime dependency completeness.
- CI builds, runs two test executables, and uploads a ZIP. CMake's optional architecture check is not guaranteed to run in that workflow.
- The README has extensive feature documentation but starts with developer setup. The handoff document describes an older storage implementation and unverified work; it is not current release evidence.
- The documented store root is `%LOCALAPPDATA%\DeadlockModManager\store`. A branding change must preserve existing libraries.
- No root project LICENSE appeared in the root listing. The owner must choose the project license before public distribution; third-party notices alone do not make that decision.
- Current untracked `.idea/`, `diag.obj`, and `vc140.pdb` are local state; do not include or delete them as part of release work.
- No local Git tags were listed. Check remote release history before assigning `v0.1.0`, since CMake already says `0.1.1`.

## 1. Establish the branch and verified baseline

- Record the starting commit, working-tree state, available toolchain, and published version history; create `codex/modworks-v0.1` from the agreed baseline.
- Configure and build Release with the full GUI enabled and profiling disabled. Run core and rendering tests and the dependency architecture check. Record results and any environment-dependent skips explicitly.
- Exercise the current import, restart, inspect, build, repair, and history flows on disposable copies of representative files. Capture the existing IDE layout for comparison.
- Convert discovered failures into a bounded release checklist. Fix data-loss, startup, packaging, and core-workflow blockers before cosmetic refinement.

Exit: reproducible baseline and an explicit blocker list; historical test counts are not treated as proof.

## 2. Unify product identity and versioning

Primary files: `CMakeLists.txt`, `cmake/render.cmake`, `src/app/`, `src/preview/main.cpp`, CLI help, build scripts, workflows, and documentation.

- Set public identity to **Deadlock Modworks**, executable/target to `DeadlockModworks`, and initial release version to `0.1.0` if release history permits it.
- Generate shared product/version metadata from CMake for About, CLI `--version`, Windows file properties, and packaging. Include a build revision in diagnostics.
- Replace visible legacy names in titles, errors, help, artifact names, and notices. Update CLion/build references. Keep internal namespaces and library target names unless changing them is necessary; a broad internal rename adds little release value.
- Retain `%LOCALAPPDATA%\DeadlockModManager` as the compatibility data root for 0.1 and document it. Do not silently create an empty replacement library under the new brand.
- Keep existing CLI behavior and documented arguments functional. Decide whether a legacy executable alias is needed based on actual prior distribution.

Exit: UI, file properties, CLI, and release ZIP agree on name/version; an existing library loads intact.

## 3. Add a basic production icon

New assets: editable vector source, PNG previews, multi-resolution ICO, and Windows resource definitions under a dedicated assets/resources directory.

- Implement the standalone icon specification above as a flat **M made from stacked mod tiles**, using steel blue and pale silver. Remove texture, small ornamental strokes, and text from the executable icon.
- Supply ICO sizes 16, 20, 24, 32, 48, 64, 128, and 256 pixels. Adjust the smallest sizes for clarity.
- Embed the icon and version resource; set large/small Win32 window icons. Reuse the mark in About and documentation.
- Inspect Explorer, title bar, taskbar, Alt-Tab, and light/dark desktop backgrounds at common scaling levels.

Exit: the packaged executable has a recognizable icon without depending on a loose image file.

## 4. Apply the steel-blue theme to the IDE workspace

Extract theme tokens and application of ImGui styles into a small shared theme module. Audit per-panel hard-coded colors so the overhaul covers tables, trees, dialogs, selection, disabled controls, viewports, and repair results.

| Role | Starting value |
| --- | --- |
| Workspace background | `#141B23` |
| Panel background | `#1D2833` |
| Borders/dividers | `#344657` |
| Steel-blue accent | `#4682B4` |
| Hover/focus accent | `#659BC3` |
| Primary text | `#E6EDF3` |
| Secondary text | `#9BAEBE` |

- Preserve compact menus, toolbars, explorer trees, tab strips, and split model views. The gallery remains one workspace page.
- Use blue for focus, active tabs, selection, and principal actions. Keep readable semantic success/warning/error colors with accompanying labels or symbols.
- Check actual text/button contrast and adjust foregrounds or fills where needed; the proposed palette is a starting point, not a guarantee of legibility.
- Persist docking layout, window size/position, and relevant workspace preferences in the user data directory. Initialize a default layout only when no valid layout exists. Add **View > Reset layout**, validate stored settings, and recover windows moved off-screen.
- Make explorer/workspace panels recoverable through View controls. Preserve current comparison and multi-view splits. A full multi-document editor or arbitrary per-model docking system is deferred beyond 0.1.
- Verify keyboard focus, shortcuts, clipped labels, tooltips, long paths, 100/150/200% scaling, and a 1280x720 window. Verify moving between displays with different scaling.

Exit: every shipped page uses the theme, remains readable and usable, and the workspace survives restart/reset. Capture screenshots of the real application.

## 5. Make startup and the first successful pack straightforward

- Default distribution: `DeadlockModworks-0.1.0-win-x64.zip`. Extract and double-click; no build tools, SDK, terminal, or administrator privileges required for normal operation.
- Show a useful empty workspace with **Connect game**, **Add mods**, and **Open quick start** actions. Auto-detect Steam installs; preserve manual choices and provide a clear browse fallback.
- Allow library browsing without a connected game where supported. Explain which operations need game resources and why.
- Persist state in the user data directory, independent of the launch working directory. Cover paths containing spaces and Unicode, read-only application folders, and missing/moved game installations.
- Diagnose missing Vulkan loader, unsupported device/features, unwritable storage, and corrupt settings in user-facing messages. Ensure launch failures remain visible when no console exists; missing-loader handling may require delay loading or an equivalent bootstrap check.
- Provide a local log and **Open logs / Copy diagnostic details** action with version and actionable error information. Keep normal startup quiet and distinguish errors from background progress.
- Verify double-click, shortcut launch, drag-and-drop, and CLI launch. Preserve CLI exit codes and terminal output.
- Give export a clear completion result with **Reveal in Explorer** and a link to installation instructions. Explain that assembly does not install or guarantee compatibility.

Exit: a fresh user reaches a validated exported pack using only the ZIP and quick-start instructions; failed startup explains the next action.

## 6. Harden the existing workflows and storage

Add focused regression tests for changed behavior and failures with data-integrity implications. Use representative real mods for manual checks without committing or distributing those assets.

- Import: single/chunked VPKs, duplicate inputs, changed inputs, same-name files, bad archives, partial batches, cancellation, and shutdown during work.
- Persistence: restart restores collection, selected skins, game connection, and history; moving originals does not break stored mods. Exercise legacy store migration on copied fixtures and library sharing/merging.
- Export: identical-resource deduplication, conflicts, missing dependencies, output validation, cancellation, destination collisions, and write failures. Sources and previously published outputs remain intact.
- Repair/history: supported and unsupported outcomes, backup/version recovery, before/after comparison, and no partial publication on failure. Make recovery instructions match the current store-backed implementation.
- Storage failures: unwritable paths, truncated/corrupt state, interrupted saves, and controlled disk-write failures. Surface errors rather than silently resetting the library.
- Responsiveness: large collections, bounded thumbnail loading, repeated inspect/close operations, resize/minimize/restore, and exit with background jobs running. Record dataset size and hardware with observations.
- Audit any remaining unsafe assumptions in external archive parsing and user-supplied paths encountered during these checks.

Exit: no known data-loss, startup-crash, or broken primary-workflow defects. Every remaining limitation has a reproducible description and a release-note entry.

## 7. Produce a complete, repeatable package

- Refactor `Publish.ps1` to use a fresh staging directory under a validated packaging root. Reject dangerous/overlapping destinations; never recursively delete arbitrary caller-provided paths. Build before replacing a prior output.
- Establish one dependency/runtime policy: preferably a consistent static MSVC runtime across targets for the portable build, subject to validation; otherwise include the supported runtime deployment prerequisites. Inspect actual executable dependencies.
- Ship the executable, quick start, applicable license/notices, and required runtime files. Verify shaders/resources are embedded or included. Exclude game/mod assets, developer paths, test fixtures, caches, and profiler data.
- Include versioned ZIP naming, SHA-256 checksums, and a manifest of package contents. Fail on missing required files or stale/mismatched version metadata.
- Update CI to run CTest with failure output and explicitly install the architecture-check dependencies so that check cannot silently disappear. Distinguish GPU-dependent smoke checks from tests runnable on hosted CI.
- Build and smoke-test the extracted artifact, not only the build-tree executable. Provide a tag-triggered release workflow that prepares a draft release and matching artifacts after checks pass.
- Verify on a clean Windows machine with a suitable graphics driver but no Visual Studio, Vulkan SDK, or development PATH. Verify offline startup after download/extraction.

Exit: the CI-produced ZIP launches on a clean machine and contains everything promised. An installer, auto-update service, and signing infrastructure are deferred; record the signing status honestly.

## 8. Finish user and maintainer documentation

- Rewrite README around what Modworks does, a real screenshot, download, requirements, and a short first-run workflow. Put source-build instructions in `docs/BUILDING.md`.
- Add `docs/QUICK_START.md`: extract, launch, connect, import, inspect, select, build, install/remove the output, and recover from a failed step. Verify game installation instructions against the release-time behavior before publishing.
- Add a user guide covering IDE navigation, shortcuts, comparison, repairs, history, library sharing, data locations, backup/restore, and the difference between removing a library entry and deleting data.
- Add troubleshooting for graphics/loader issues, missing game resources, corrupt mods, export conflicts, storage errors, and collecting useful diagnostics.
- Add `CHANGELOG.md`, `docs/RELEASING.md`, and a completed release checklist. Document supported/unsupported mod content and known limitations.
- Update architecture and third-party notices to the shipped build. Mark the old handoff document as historical or replace it with an accurate status reference.
- Obtain the owner's license choice and add the root LICENSE and package copy. Review attribution/package contents against the actual dependencies.
- Add a focused bug-report template requesting app version, reproduction, expected/actual result, and relevant diagnostics; do not require uploading entire private libraries.

Exit: a reader can use the release without reading developer setup, and a maintainer can reproduce the build and package.

## 9. Release candidate and publication gate

1. Freeze scope and build a candidate from a recorded commit with a clean tracked working tree.
2. Run automated checks and the clean-machine/startup/storage/UI acceptance matrix above against the exact artifact.
3. Complete an end-to-end fresh-install and existing-library-upgrade walkthrough; verify final screenshots and documentation against that candidate.
4. Verify executable metadata, package version, checksum, license, notices, and known issues. Resolve every release blocker; do not replace failures with undocumented skips.
5. Prepare the branch PR, release notes, and draft release assets. Have the final candidate reviewed before merging/tagging/publishing.
6. Tag the approved commit `v0.1.0` and publish matching artifacts. Download the published ZIP and verify its checksum and startup once more.

Ready to publish means all gates through step 4 are satisfied and the exact candidate is available for review. This planning request does not itself publish a release or rename the hosted repository.

## Suggested commit sequence

1. Baseline fixes and release checklist.
2. Product name/version and icon resources.
3. Shared steel-blue theme and IDE layout persistence.
4. First-run experience and startup diagnostics.
5. Storage/workflow fixes with focused regression coverage.
6. Packaging and CI release artifacts.
7. Documentation, license, screenshots, and release notes.
8. Candidate verification and final fixes.

## Scope boundaries and decisions

- Required: identity, icon, theme, IDE polish/persistence, easy portable startup, reliable existing workflows, complete packaging, documentation, and release evidence.
- Deferred: mod marketplace/downloader, arbitrary mod deployment/uninstall engine, account/cloud sync, plugin system, multi-platform support, full editor architecture rewrite, automatic updater, and installer.
- Owner decision before publication: project license. Verify published version history before finalizing `0.1.0`. Hosted repository rename to `deadlock-modworks` can follow separately after links and integrations are inventoried.
- Do not claim compatibility with Windows versions or GPU configurations that have not been tested. Record a concrete support matrix from release-candidate verification.

## Build and verification runbook

These commands describe the inspected checkout. Confirm available tools and presets before running them; update names as the rename lands. Use PowerShell from the repository root. Do not reuse a CMake build directory with a different generator. No build or test commands were executed in the preparation of this handoff.

```powershell
git status --short
git rev-parse HEAD
git branch --show-current
git tag --list

# Only after checking for an existing branch and preserving local work:
git switch -c codex/modworks-v0.1

# Existing default preset requires Visual Studio 2026 plus the Vulkan SDK.
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/release -C Release --output-on-failure

# Run explicitly as well if not registered in CTest; Python needs networkx.
python scripts/architecture_diagram.py --check
```

If Visual Studio 2022 is the installed toolchain, configure a separate directory and use it consistently for build, tests, and eventual packaging:

```powershell
cmake -S . -B build/release-vs2022 -G "Visual Studio 17 2022" -A x64 -DSR_BUILD_MODEL_TOOLS=ON -DSR_TRACY=OFF
cmake --build build/release-vs2022 --config Release
ctest --test-dir build/release-vs2022 -C Release --output-on-failure
```

Check every native command's exit code; a later successful command does not erase a failed configure/build/test. CLion Ninja presets also exist; command-line use requires the appropriate x64 MSVC environment. Dependency fetching may need network access. Report genuine toolchain/network limitations instead of implying a successful build.

The historical handoff reported sandbox temp-directory failures during archive tests. If that problem recurs, use a disposable workspace temp directory and record this environment adjustment:

```powershell
New-Item -ItemType Directory -Force -Path build/test-temp | Out-Null
$env:TEMP = (Resolve-Path build/test-temp).Path
$env:TMP = $env:TEMP
```

Do not run the current `Publish.ps1` with a valuable/custom output directory: its existing deletion behavior is a task to fix. After hardening it, document its final arguments and prove it accepts the selected build directory/configuration or provides an equally clear supported build path. Do not rely on an old cached EXE in `dist/`.

For live app checks, inspect the active environment's available UI tools and instructions. The historical handoff records that an earlier UI session was stopped by the user; do not silently resume that session. If fresh interactive authorization or a capable machine is unavailable, complete source/build/package work and leave the corresponding manual checks explicitly pending. A mockup, screenshot generated by AI, or unit test is not evidence that a GUI workflow works.

## Required acceptance evidence

Maintain a table in `docs/RELEASE_CHECKLIST.md` with columns **Check**, **Result (PASS/FAIL/BLOCKED/NOT RUN)**, **Commit/artifact**, **Environment**, and **Evidence/notes**. A blocked check is not a pass. Keep logs and large temporary evidence out of source control; commit useful release documentation and selected final screenshots only.

| Check | Minimum evidence |
| --- | --- |
| Build and automated tests | Exact configure/build/test commands, exit codes, counts from the current run, and architecture check result |
| Identity | EXE name, About, CLI version/help, Windows properties, and package filename agree |
| Icon | Actual 16/32/48/256px previews plus Explorer/taskbar observation on the built executable |
| Theme | Real screenshots of library, inspector/comparison, builder, repair/history, and a dialog/error state |
| Workspace persistence | Resize/rearrange, close/reopen, then Reset layout; collection remains unchanged |
| Fresh startup | Extracted ZIP launched as a normal user on a machine without developer tools; record OS and GPU/driver |
| First successful pack | Connect game, import, select, inspect, export, validate, and locate output using the quick start |
| Existing data | A copied legacy/current library survives upgrade/restart with selection/history intact |
| Original-file independence | Imported single/chunked mods still preview/export after original inputs are moved |
| Failure handling | Invalid import, conflicting export, failed save/write, and unavailable game/graphics produce actionable outcomes without data loss |
| Repair/recovery | Supported repair, unsupported input, and documented restoration/history workflow on disposable data |
| UI scaling/navigation | 100/150/200% scaling, small window, long paths, keyboard navigation, and display-change behavior |
| Package | Manifest, dependency audit, complete notices, ZIP checksum, offline launch, no accidental game assets |
| Docs | A walkthrough using the final artifact matches written labels, requirements, paths, and limitations |

If a representative mod collection, compatible GPU, clean machine, or license decision is unavailable, identify the exact missing input and which checks it blocks. Do not create synthetic evidence or mark the release ready prematurely. Prefer controlled fixtures and disposable library profiles for failure tests; never corrupt the owner's active library to test recovery.

## Final delivery from the implementation agent

Deliver a concise summary linking to the implementation branch/diff or PR (if created within the assigned workflow), plus:

1. The final executable/package paths, version, source commit, and SHA-256 checksum.
2. Editable icon source and generated icon assets, with the regeneration procedure.
3. Final theme tokens and real screenshots showing the IDE-style result.
4. Updated README, quick start, user/build/release guides, notices, changelog, and release checklist.
5. Automated test results and manual acceptance evidence, clearly distinguishing unrun/blocked checks.
6. Remaining defects, known limitations, owner decisions, and exact next steps for publication.

State **ready for publication review** only when the release gates have passed. Otherwise state what is implemented and what still blocks that status. Stop at the reviewable candidate unless the owner separately authorizes merge/tag/publication. Do not spend the final report merely proposing to do work that this implementation assignment already required.
