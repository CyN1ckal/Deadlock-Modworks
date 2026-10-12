# Deadlock Skeleton Repair

A Windows x64 command-line tool for checking Deadlock hero-model mods against the installed game, repairing supported skeleton changes, and writing a JSON report of anything that needs author review.

The program is `DeadlockSkeletonRepair.exe`. It does not open a window. With no arguments, or with arguments it does not understand, it prints usage and exits 2.

## Use the tool

Close Deadlock and wait for Steam updates to finish before running repairs. The tool holds read locks while it works; a concurrent update may cause either operation to report a file-access error. A repair always re-analyzes the current files before writing.

```powershell
.\DeadlockSkeletonRepair.exe --analyze "C:\Games\Deadlock\game\citadel" "mod.vpk" --report "report.json"
.\DeadlockSkeletonRepair.exe --update "C:\Games\Deadlock\game\citadel" "mod.vpk" "mod-repaired.vpk"
.\DeadlockSkeletonRepair.exe --repair "C:\Games\Deadlock\game\citadel" "mod.vpk" --report "repair.json"
```

`<citadel-folder>` is the installed `Deadlock/game/citadel` directory, the folder that contains `pak01_dir.vpk`.

- `--analyze` reads the mod and prints one line per hero resource. It does not write a VPK.
- `--update` writes a new archive. It refuses to overwrite an existing file.
- `--repair` replaces the mod with a validated archive and keeps a uniquely named `.bak-...` backup beside the original.

`--report` is optional, must be the last two arguments, and the path must end in `.json`. A VPK with any blocked model is left untouched. Test a repaired mod in-game.

To undo a repair, close the game, move the repaired VPK out of the addons folder, then copy its original `.bak-...` file back to the original VPK filename. For a chunked mod, leave its original chunk files in place: the backup directory VPK still refers to them. Repaired VPKs contain their data in a single archive.

## What the results mean

| Result | Meaning |
| --- | --- |
| Repairable | A supported skeleton rewrite passed the structural checks. Analysis uses this before any file is written. |
| Updated | The complete repaired VPK was written, reopened, and validated. |
| Current | No actionable supported rig issue was found. Custom render bones, poses and cloth can differ from the stock model. This does not certify in-game compatibility. |
| NeedsReview | An incompatible animation override, unresolved declared rig dependency, ambiguous legacy mapping, or unsupported structure needs a mod author's judgment. |
| Failed | The input could not be read or validated, or the operation failed. |
| Skipped | No supported hero models were found. The tool cannot assess that content. |

Progress lines spell “Needs review”. The result line and the JSON report use `NeedsReview`.

## Scope and limits

The tool supports VPK version 2, hero `.vmdl_c` resources and AG2 `.vnmskel_c` animation-skeleton overrides. A custom render skeleton is not a copy of the stock mesh skeleton: different proportions, helper bones, cloth topology, LOD flags and bone order can be intentional. Models declaring external AG2 skeleton bindings keep their render skeletons unchanged. These differences appear as non-blocking notes.

A bundled stock-named `.vnmskel_c` can override the installed game and break animation when its binding order or low-LOD sampling layout is outdated. The tool checks that layout separately and migrates compatible overrides onto the current game binding order. It preserves existing parent-space reference poses and authored masks, adds new stock bones, recomputes world-space poses, and updates the low-LOD sampling count. A changed parent hierarchy, custom or removed animation bone, or unsupported secondary skeleton blocks that migration. Model geometry and other archive resources stay as they were.

Unique case-only bone renames are supported when they are one-to-one and their mapped parent relationships agree. The tool canonicalizes bone IDs and same-skeleton mask bone references while preserving reference poses, mask weights, mask labels and references to other skeletons. It reports the mappings as `BoneRenames` in JSON. Ambiguous casing, collapsed names, semantic renames and a render model still using incompatible spelling remain review cases.

Haze is the case this covers: its render model already uses `weapon_hand_L`, `magazine_L`, `weapon_hand_R` and `magazine_R`, while its animation override used lowercase side suffixes. That override also had 97 bones and sampled all 97 at low LOD; the installed skeleton has 98 bones and samples 32. The repair updates the four names, 22 mask references, binding order and sampling layout, and leaves the render model and the other archive entries unchanged. In-game verification is still required.

For embedded rigs without external AG2 bindings, the supported skeleton normalization can reorder compatible bones and add unambiguous missing bones. It updates the mesh remapping table while preserving authored transforms and flags.

The tool checks archive bounds and per-file CRCs, skeleton array sizes, duplicate names, parent cycles, transform validity, mesh remapping indices, and declared rig dependencies in the current game and the selected VPK. Rest-pose and LOD-usage differences alone are not blockers. Missing legacy AG1 graphs are notes for models declaring AG2 bindings. Missing declared animation includes or active AG2 dependencies remain review alerts. Models with embedded physics or animation blocks, unknown skeleton metadata, or missing remap tables are blocked when a render-skeleton rewrite would be required.

The tool does not infer arbitrary bone renames, migrate resource paths, rewrite animation clips, physics or cloth, or recompile source assets. It does not inspect other installed addons when resolving dependencies. A mod that depends on another addon may therefore need review.

Two-way comparison cannot always distinguish intentional customization from an obsolete binding, so unresolved cases remain review alerts. A hero whose animation binding already matches the installed game is `Current` and is not rewritten. A missing declared animation include, such as Geist's `geist_backup.vmdl`, is a dependency alert, not proof that the whole mod is unusable.

Reopening a compiled model proves structural readability, not correct animation, skinning, attachments, or appearance in Deadlock.

## Command line

`--help` and `-h` print usage when they are the only argument.

Exit codes: **0** means supported analysis or a completed repair, **1** means review or unsupported content, **2** means invalid input, failure, or cancellation. Exit code 0 from analysis does not guarantee gameplay compatibility.

JSON reports use schema version 2. `GeneratedUtc` is the time the report was written.

## Build and test

Requires Windows x64, CMake 3.24 or newer, and Visual Studio 2026 or Visual Studio 2022 with the C++ desktop workload. The repair tool does not need a .NET install. The executable uses the Visual C++ redistributable that comes with that Visual Studio install.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
.\build\release\bin\Release\skeleton_repair_tests.exe
.\scripts\Publish.ps1
```

`windows-release` uses the Visual Studio 18 2026 generator. On a machine that only has Visual Studio 2022, `Publish.ps1` configures with that generator instead. The script builds `DeadlockSkeletonRepair.exe`, copies the README, third-party notices and license texts, and writes `dist/DeadlockSkeletonRepair-win-x64.zip`. The zip is a single executable plus those notices.

GitHub Actions builds and tests on Windows and uploads that zip. No game assets are distributed.

## Model preview

When the Vulkan SDK, including `glslc`, is installed, the same build also produces `ModelPreview.exe`. It opens a compiled hero model, or a model inside a VPK, and draws the decoded meshes. The repair command does not use it, and the portable zip does not include it. Without the SDK, that target is skipped and the repair tool still builds.

## Credits

Binary KV3 and Source 2 resource layouts follow [Source 2 Viewer](https://s2v.app) ([ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat)), pinned to release 20.0.6980. That library is not linked into this executable. See [third-party notices](THIRD_PARTY_NOTICES.md) for the format credit and for LZ4, Zstandard, nlohmann/json, and meshoptimizer.
