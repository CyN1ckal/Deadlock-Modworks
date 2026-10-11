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
| Current | No supported skeleton differences were found. This does **not** certify in-game compatibility. |
| Needs review | A possible rename, changed hierarchy/rest pose/flags, missing rig dependency, or unsupported structure needs a mod author's judgment. |
| Failed | The input could not be read or validated, or the operation failed. |
| Skipped | No supported hero models were found, or the model has no bones. The tool cannot assess that content. |

## MVP scope and limits

The tool supports VPK version 2 and embedded KV3 skeletons in hero `.vmdl_c` resources. It can reorder compatible bones and add missing official bones when there are no ambiguous mod-only bones. It updates the model's mesh remapping table and preserves the mod's existing transforms and other archive contents.

It checks archive bounds and per-file CRCs, skeleton array sizes, duplicate names, parent cycles, transform validity, mesh remapping indices, shared bone parents/rest poses/flags, and availability of referenced rig resources in the current game and selected VPK. Models with embedded physics or animation blocks, unknown skeleton metadata, or missing remap tables are blocked when a rewrite would be required.

The updater does not infer bone renames, migrate resource paths, rewrite animation/physics/cloth systems, or recompile source assets. It does not inspect the other installed addons when resolving dependencies. A mod that depends on another addon may therefore need review.

Two-way comparison cannot always distinguish an intentional custom rig from an obsolete official rig. In particular, custom proportions can produce rest-pose review alerts even for working mods. Both game-only and mod-only bones trigger review rather than a guessed rename. The local samples used during development currently require review under these rules; they are not declared definitely broken.

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

Optional local sample checks (the game and samples are not committed):

```powershell
dotnet run --project DeadlockModelUpdater.Tests -c Release --no-build -- "C:\Games\Deadlock\game\citadel" "input"
```

GitHub Actions builds and tests on Windows and produces a self-contained Windows x64 portable ZIP. No game assets are distributed.

## Credits

Powered by [Source 2 Viewer](https://s2v.app) ([ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat)). Source 2 format support comes from that project's reverse-engineering work. See [third-party notices](THIRD_PARTY_NOTICES.md) for attribution and licenses.
