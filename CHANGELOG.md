# Changelog

## Unreleased

### Added

- **Get mods** (**Ctrl+8**): browse, sort, filter and search Deadlock mods on GameBanana, or open a pasted mod link, then **Download** or **Download & enable** one of its files. Downloads are HTTPS-only from gamebanana.com, checked against GameBanana's MD5, and zip, 7z and rar archives are opened with libarchive. Only the `.vpk` files inside are written, and archive paths cannot leave the temporary folder.
- **Mods** (**Ctrl+7**): enable library mods per profile, set the load order by dragging, see which enabled mods override each other's files, and **Apply to game**, which places the enabled mods in `game\citadel\addons` as `pakNN_dir.vpk`. Modworks records its files in `addons\.modworks.json` and never touches other files there. It checks `gameinfo.gi` for the addons search path and, when asked, adds it with a backup.
- **Build as mod...** in the VPK builder builds the pack into the library as a named mod that the **Mods** page can enable and apply. Building again under the same name replaces it in place, keeping its load-order position and enabled state. A pack identical to a mod already in the library is refused rather than renaming that mod.
- **Add VPKs...** on the **Mods** page, and **Built pack** as a mod source.
- `mods.json` in the data root keeps enabled mods, load order, profiles and download sources.
- zlib 1.3.1, XZ Utils (liblzma) 5.8.1 and libarchive 3.8.1 are built from pinned, hash-checked sources. Their licenses are in `THIRD_PARTY_NOTICES.md` and the package's `licenses` folder.

### Fixed

- **Apply to game** puts moved and removed mods back when a later copy fails or you cancel, instead of deleting them.
- A gzip download is kept. The compressed `Content-Length` is no longer compared to the decoded file.
- Archive extraction stops on a skipped file that expands past the size limit, and it does not open DOS device names such as `NUL.vpk`.
- **Add the line and apply** does not change `gameinfo.gi` while Deadlock is running.

### Changed

- The quick start's Import and Install or remove steps describe **Get mods** and **Apply to game**. Building a pack still does not install it.
- Repairs wait while mods are applied, because both use the stored mod files.

## 0.1.0 — not tagged, not published

The commits below are `8470d845c656fba0c60bcf02644fbb7537a6070a` through `68b8ebf2362556d66e0019107d66bbba659e0134`, plus the documentation commit that describes the empty-library actions. Nothing in this list has been pushed, and `v0.1.0` has not been tagged. A GitHub release does not exist yet. The draft workflow in `.github/workflows/release.yml` has not been run. `scripts/Publish.ps1` was not run on this documentation commit.

### Added

- The product is Deadlock Modworks 0.1.0. The executable is `DeadlockModworks.exe`. About, `--version`, and the Windows version resource use that name. The package file name is `DeadlockModworks-0.1.0-win-x64.zip`.
- An embedded M icon (`assets/branding/modworks.ico`), including the window icon for the model-tools build.
- A steel-blue workspace theme, saved docking in the data directory, and **View > Reset layout**. Reset layout does not clear the mod collection.
- `--data-root`, then the environment variable `DEADLOCK_MODWORKS_DATA_ROOT`, then `%LOCALAPPDATA%\DeadlockModManager`. The default folder name stays `DeadlockModManager` so an existing library still opens.
- Static MSVC runtime (`/MT`) for the portable build. `vulkan-1.dll` stays a driver DLL, is delay-loaded, and is not shipped. A missing loader is supposed to show a message instead of failing in the static imports. That dialog was not observed.
- An empty library with **Connect game**, **Add mods**, and **Open quick start**. The page says browsing and adding mods work without the game, and that building a pack does not install it.
- **Help > Quick start**, a modal titled **Quick start**, with the eight steps Extract, Launch, Connect, Import, Inspect, Select, Build, and Install or remove. The popup was not clicked.
- **Help > Open logs** and **Help > Copy diagnostic details**. The log file is `<data-root>\logs\modworks.log`. The default path is `%LOCALAPPDATA%\DeadlockModManager\logs\modworks.log`. A quiet start does not create it. Those menu items were not clicked.
- A **Pack assembled** modal after a build or **Assemble...**. It shows the summary, says assembly does not install the pack, and has **Reveal in Explorer**, **Open quick start**, and **Close**. No pack was built, so the modal was not shown.
- Steam fills the game archive when none is saved. A typed path, **Connect**, and **Browse...** remain.
- Startup captures `docs/screenshots/startup-empty-library.png`, `startup-corrupt-settings.png`, `startup-readonly-data-root.png`, and `startup-missing-data-root.png`.
- `scripts/Publish.ps1` stages under `build/package-staging/<id>`, writes the zip and a `.sha256` file, and rejects destinations it must not delete. It does not recursively delete an arbitrary caller directory or `build/release`.
- A tag workflow that would build, test, upload the zip, and open a draft GitHub release. It is push-only for tags `v*` and has not been executed.
- This user guide, quick start, troubleshooting, build and release notes, a bug-report template, and the MIT `LICENSE`.

### Fixed

- A damaged or truncated `catalog.sqlite` is refused with **The model library is damaged and was not replaced.** The file is not replaced by a new catalog.
- A corrupt `store\settings.json` is left in place, the catalog is not replaced, and the error is shown. A read-only catalog is not replaced.
- A failed export, a cancelled export, a failed shared-library write, and a failed repair do not replace the previous output or the input snapshot.

### Changed

- The public version for this release is 0.1.0. The CMake project had been `DeadlockSkeletonRepair` 0.1.1. No earlier `DeadlockSkeletonRepair.exe` package had been published, so there is no second executable under the old name.

### Known limits

- Local collection, inspection, comparison, supported skeleton repair, version history, library sharing, and model-pack assembly. Not every mod type. Assembly does not install the pack.
- No installer, auto-update, code signing, or marketplace. `v0.1.0` is not tagged and nothing is published.
- Clean-machine launch, code signing, and publication were not run. See `docs/RELEASE_CHECKLIST.md`.
- Still not run: a clicked **Quick start** popup, the missing `vulkan-1.dll` dialog, drag-and-drop onto the window, a finished **Pack assembled** modal, and `scripts/Publish.ps1` on this documentation commit.
- Side-by-side comparison of two loaded models was not captured. `startup-empty-library.png` shows the empty library and the three buttons. It does not show two models.
