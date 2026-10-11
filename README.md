# Deadlock Model Updater

A Windows MVP for checking Deadlock hero-model mods against the installed game, repairing supported skeleton changes, and explaining changes that need author review.

## Use the app

1. Extract the entire portable ZIP and run `DeadlockModelUpdater.exe`.
2. Check the detected **Citadel folder**, or choose `Deadlock/game/citadel` manually.
3. Choose one or more mod VPKs, or drag them onto the window.
4. Click **Analyze mods**. Select a result to read its full explanation.
5. Click **Repair supported mods** to repair eligible VPKs. Each original gets a uniquely named `.bak-...` backup beside it. A VPK with any blocked model is left untouched; other eligible VPKs in the batch can still be repaired.
6. Test repaired mods in-game. Use **Save report** to export the results, reasons, and backup paths as JSON.

Close Deadlock and wait for Steam updates to finish before running repairs. The updater holds read locks while it works; a concurrent update may cause either operation to report a file-access error. A repair always re-analyzes the current files before writing.

To undo a repair, close the game, move the repaired VPK out of the addons folder, then copy its original `.bak-...` file back to the original VPK filename. For a chunked mod, leave its original chunk files in place: the backup directory VPK still refers to them. Repaired VPKs contain their data in a single archive.

## What the results mean

| Result | Meaning |
| --- | --- |
| Repairable | A supported skeleton rewrite passed the structural checks. |
| Updated | The complete repaired VPK was written, reopened, and validated. |
| Current | No actionable supported rig issue was found. Custom render bones, poses and cloth can differ from the stock model. This does **not** certify in-game compatibility. |
| Needs review | An incompatible animation override, unresolved declared rig dependency, ambiguous legacy mapping, or unsupported structure needs a mod author's judgment. |
| Failed | The input could not be read or validated, or the operation failed. |
| Skipped | No supported hero models were found, or the model has no bones. The tool cannot assess that content. |

## MVP scope and limits

The tool supports VPK version 2, hero `.vmdl_c` resources and AG2 `.vnmskel_c` animation-skeleton overrides. A custom render skeleton is not a copy of the stock mesh skeleton: different proportions, helper bones, cloth topology, LOD flags and bone order can be intentional. Models declaring external AG2 skeleton bindings keep their render skeletons unchanged. These differences appear as non-blocking notes in the UI and JSON report.

A bundled stock-named `.vnmskel_c` can override the installed game and break animation when its binding order or low-LOD sampling layout is outdated. The updater checks that layout separately and migrates compatible overrides onto the current game binding order. It preserves existing parent-space reference poses and authored masks, adds new stock bones, recomputes world-space poses, and updates the low-LOD sampling count. A changed parent hierarchy, custom/removed animation bone, or unsupported secondary skeleton blocks that migration rather than silently replacing custom content. Model geometry and other archive resources remain unchanged.

Unique case-only bone renames are supported when they are one-to-one and their mapped parent relationships agree. The updater canonicalizes bone IDs and same-skeleton mask bone references while preserving reference poses, mask weights, mask labels and references to other skeletons. It reports the mappings as `BoneRenames` in JSON. Ambiguous casing, collapsed names, semantic renames and a render model still using incompatible spelling remain review cases; the rule does not remove underscores or infer fuzzy matches.

Haze exposed this case: its render model already uses `weapon_hand_L`, `magazine_L`, `weapon_hand_R` and `magazine_R`, but its animation override used lowercase side suffixes. Its override also had 97 bones and sampled all 97 at low LOD; the installed skeleton has 98 bones and samples 32. The generic repair updates the four names, 22 mask references, binding order and sampling layout. It leaves Haze's render model and other assets byte-identical. This fixes the identified data mismatch; in-game verification is still needed to confirm the visible distortion is resolved.

For embedded/legacy rigs without external AG2 bindings, the existing supported skeleton normalization can reorder compatible bones and add unambiguous missing bones. It updates the mesh remapping table while preserving authored transforms and flags.

It checks archive bounds and per-file CRCs, skeleton array sizes, duplicate names, parent cycles, transform validity, mesh remapping indices, and declared rig dependencies in the current game and selected VPK. Rest-pose and LOD-usage differences alone are not blockers. Missing legacy AG1 graphs are notes for models declaring AG2 bindings; missing declared animation includes or active AG2 dependencies remain review alerts. Models with embedded physics or animation blocks, unknown skeleton metadata, or missing remap tables are blocked if a render-skeleton rewrite would be required.

The updater does not infer arbitrary bone renames, migrate resource paths, rewrite animation clips/physics/cloth systems, or recompile source assets. It does not inspect other installed addons when resolving dependencies. A mod that depends on another addon may therefore need review.

Two-way comparison cannot always distinguish intentional customization from an obsolete binding, so unresolved cases remain review alerts. The known-working local Necro, Hornet and McGinnis samples now produce `Current` and are left byte-identical. Unicorn's outdated bundled animation skeleton is repairable separately from its custom model. Geist reports its missing declared `geist_backup.vmdl` animation include; this is a specific dependency alert, not proof that the whole mod is unusable.

Reopening a compiled model proves structural readability, not correct animation, skinning, attachments, or appearance in Deadlock. Automatic gameplay validation is outside this MVP. Future work should add versioned original-game baselines and explicit migration mappings to make more custom mods repairable.

## Command line

```powershell
./DeadlockModelUpdater.Cli.exe --analyze "C:\Games\Deadlock\game\citadel" "mod.vpk" --report "report.json"
./DeadlockModelUpdater.Cli.exe --update "C:\Games\Deadlock\game\citadel" "mod.vpk" "mod-repaired.vpk"
./DeadlockModelUpdater.Cli.exe --repair "C:\Games\Deadlock\game\citadel" "mod.vpk" --report "repair.json"
```

`--update` writes to a new output filename and refuses to overwrite an existing file. `--repair` replaces the original and always creates a backup. Any review/failed/unsupported result blocks writing the entire VPK. JSON reports can also be saved when analysis fails.

Exit codes: **0** means supported analysis or completed repair, **1** means review or unsupported content, **2** means failure, invalid arguments, or cancellation. Exit code 0 from analysis does not guarantee gameplay compatibility. Use `--help` for syntax; Ctrl+C requests cancellation.

## Build and test

Requires Windows and the .NET 10 SDK. The portable release includes the runtime; a normal framework-dependent build requires the .NET 10 Desktop Runtime.

```powershell
dotnet restore DeadlockModelUpdater.slnx --locked-mode
dotnet build DeadlockModelUpdater.slnx --no-restore -c Release -m:1
dotnet run --project DeadlockModelUpdater.Tests -c Release --no-build
dotnet run --project DeadlockModelUpdater.Tests -c Release --no-build -- --ui-repair-smoke artifacts/ui-repair-smoke.png
./scripts/Publish.ps1
```

The dependency-free regression runner exercises skeleton classification, real synthetic compiled-model migrations, exact backups, unchanged assets, independent ValvePak checksum verification, corrupt input, blocked mixed packages, cancellation, and CLI reports. It returns a nonzero exit code on failure.

The UI smoke check drives Analyze and Repair using synthetic fixtures, verifies the backup and button states, checks that late progress cannot replace the completion summary, and saves a rendered screenshot.

To verify the five local samples and create a separate repaired Unicorn copy, choose a fresh output directory:

```powershell
dotnet run --project DeadlockModelUpdater.Tests -c Release --no-build -- --verify-samples "C:\Games\Deadlock\game\citadel" "input" "input/repaired-check"
```

This check verifies that the three known-working mods are not rewritten, only Unicorn's animation skeleton changes, its authored poses/masks and every other asset are preserved, and the original inputs remain untouched. It saves `input/deadlock-mod-report-fixed.json`.

Optional Haze verification with a fresh output directory:

```powershell
dotnet run --project DeadlockModelUpdater.Tests -c Release --no-build -- --verify-haze "C:\Games\Deadlock\game\citadel" "input" "input/repaired-haze-check"
```

This creates a separate repaired `pak02_dir.vpk`, checks all other assets by SHA-256, checks all original reference poses, verifies that masks differ only by the four aliases, validates the result with ValvePak, and writes `haze-repair-report.json` beside the output. The original VPK is unchanged.

Optional local sample checks (the game and samples are not committed):

```powershell
dotnet run --project DeadlockModelUpdater.Tests -c Release --no-build -- "C:\Games\Deadlock\game\citadel" "input"
```

GitHub Actions builds and tests on Windows and produces a self-contained Windows x64 portable ZIP. No game assets are distributed.

## Credits

Powered by [Source 2 Viewer](https://s2v.app) ([ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat)). Source 2 format support comes from that project's reverse-engineering work. See [third-party notices](THIRD_PARTY_NOTICES.md) for attribution and licenses.
