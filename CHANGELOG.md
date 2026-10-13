# Changelog

## 0.1.0 — not tagged, not published

The commits below are `8470d845c656fba0c60bcf02644fbb7537a6070a` through `9d9262df2e5a57e455bf88d81c5399c17b0fbe67`, plus the documentation commit that adds this file. Nothing in this list has been pushed, and `v0.1.0` has not been tagged. A GitHub release does not exist yet. The draft workflow in `.github/workflows/release.yml` has not been run.

### Added

- The product is Deadlock Modworks 0.1.0. The executable is `DeadlockModworks.exe`. About, `--version`, and the Windows version resource use that name. The package file name is `DeadlockModworks-0.1.0-win-x64.zip`.
- An embedded M icon (`assets/branding/modworks.ico`), including the window icon for the model-tools build.
- A steel-blue workspace theme, saved docking in the data directory, and **View > Reset layout**. Reset layout does not clear the mod collection.
- `--data-root`, then the environment variable `DEADLOCK_MODWORKS_DATA_ROOT`, then `%LOCALAPPDATA%\DeadlockModManager`. The default folder name stays `DeadlockModManager` so an existing library still opens.
- Static MSVC runtime (`/MT`) for the portable build. `vulkan-1.dll` stays a driver import and is not shipped.
- `scripts/Publish.ps1` stages under `build/package-staging/<id>`, writes the zip and a `.sha256` file, and rejects destinations it must not delete. It does not recursively delete an arbitrary caller directory or `build/release`.
- A tag workflow that would build, test, upload the zip, and open a draft GitHub release. It is push-only for tags `v*` and has not been executed.
- This user guide, quick start, troubleshooting, build and release notes, a bug-report template, and the MIT `LICENSE`.

### Fixed

- A damaged or truncated `catalog.sqlite` is refused with **The model library is damaged and was not replaced.** The file is not replaced by a new catalog.
- A failed export, a cancelled export, a failed shared-library write, and a failed repair do not replace the previous output or the input snapshot.

### Changed

- The public version for this release is 0.1.0. The CMake project had been `DeadlockSkeletonRepair` 0.1.1. No earlier `DeadlockSkeletonRepair.exe` package had been published, so there is no second executable under the old name.

### Known limits

- Local collection, inspection, comparison, supported skeleton repair, version history, library sharing, and model-pack assembly. Not every mod type. Assembly does not install the pack.
- No installer, auto-update, code signing, or marketplace.
- No in-app quick start, log folder, or **Open logs** command.
- Clean-machine launch, code signing, and publication were not run. See `docs/RELEASE_CHECKLIST.md`.
- Side-by-side comparison of two loaded models was not captured. The screenshots in `docs/screenshots/` are an empty library.
