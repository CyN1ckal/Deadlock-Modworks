# Changelog

## Unreleased

### Changed

- Pages are renamed and reordered around getting mods into the game. The window opens on **My mods** (was **Mods**), then **Discover** (was **Get mods**) and **Skins** (was **Mod library**). The tools follow with dimmer labels: **Pack builder** (was **VPK builder**), **Repairs** (was **Skin repairs**), **Inspector** (was **Model inspector**), **Side by side** (was **Multi view**) and **History** (was **Version history**). **Ctrl+1** to **Ctrl+8** follow the new order.
- The explorer is not drawn on **My mods** and **Discover**. Its source list is now **MOD FILES**.
- Plainer wording: **File > Locate Deadlock...** (was **Choose game archive...**), **Deadlock found** / **Deadlock not found**, **Import a shared library...** and **Export library to share...**, **Tools > Check mods for problems** and **Fix supported problems**. My mods says **Up to date** or that changes aren't in the game yet, and explains a missing `gameinfo.gi` line in a tooltip. Empty pages lead with a heading and one sentence instead of a paragraph of caveats.
- Navigation moved from the tab strip to a rail down the left edge, with icons, **PLAY** and **TOOLS** groups, an **Apply** badge on **My mods** while changes wait, and the pack count on **Pack builder**. A card at the bottom says whether Deadlock was found and opens the game menu.
- A **Settings** page (**Ctrl+,**) holds Deadlock's folder and manual path, the library's location with export and import, and the interface switches. The explorer is now the **mod files panel** and lists only mod files; its **GAME** and **LIBRARY** sections moved to Settings.
- **My mods** is a list of cards with on/off switches, where each mod came from, and badges for **Your pack**, **Wins** / **Loses n files**, and **Not in game yet** / **Still in game**. Profile actions moved into the **Profile** menu, and **Enable all** / **Disable all** into the right-click menu as **Turn all on** / **Turn all off**. Positions, drag handles, arrows and a **Which mod wins** list are in a separate **Load order** view.
- A bar under the list replaces the status lines on **My mods**. It names the changes that aren't in the game yet, offers **Undo** and **Apply to game**, reads **Close Deadlock to apply** while the game runs, and offers **Locate Deadlock...** when the game isn't found. Files from other tools are under **Other files** in the bar.
- More spacing and rounder controls, a bold heading font, and a brighter primary button colour (`#2F6FAE` with white text) so the main action on a page stands out.

### Added

- **Get mods** (**Ctrl+8**, now **Discover**, **Ctrl+2**): browse, sort, filter and search Deadlock mods on GameBanana, or open a pasted mod link, then **Download** or **Download & enable** one of its files. Downloads are HTTPS-only from gamebanana.com, checked against GameBanana's MD5, and zip, 7z and rar archives are opened with libarchive. Only the `.vpk` files inside are written, and archive paths cannot leave the temporary folder.
- **Mods** (**Ctrl+7**): enable library mods per profile, set the load order by dragging, see which enabled mods override each other's files, and **Apply to game**, which places the enabled mods in `game\citadel\addons` as `pakNN_dir.vpk`. Modworks records its files in `addons\.modworks.json` and never touches other files there. It checks `gameinfo.gi` for the addons search path and, when asked, adds it with a backup.
- **Build as mod...** in the VPK builder builds the pack into the library as a named mod that the **Mods** page can enable and apply. Building again under the same name replaces it in place, keeping its load-order position and enabled state. A pack identical to a mod already in the library is refused rather than renaming that mod.
- **Add VPKs...** on the **Mods** page, and **Built pack** as a mod source.
- `mods.json` in the data root keeps enabled mods, load order, profiles and download sources.
- **Base game** models: with Deadlock connected, the game's own hero models (`models/heroes*/<character>/`) are listed read-only for reference. Choose **Base game** in the explorer or the library's source list, or tick **Base game** in the library toolbar to show them beside your mods. They can be inspected and opened in **Multi view**, which now puts the game's copy of a model right after the mod you opened. They never go in the pack and are not analyzed.
- zlib 1.3.1, XZ Utils (liblzma) 5.8.1 and libarchive 3.8.1 are built from pinned, hash-checked sources. Their licenses are in `THIRD_PARTY_NOTICES.md` and the package's `licenses` folder.

### Fixed

- The model tools window releases its window, Vulkan device and Dear ImGui context when startup fails part way, and waits for the GPU before freeing viewports when it closes after an error.
- `--game` and `--clip` without a value now stop with an error, like `--data-root`, instead of being opened as a file.
- Version 1 VPKs, which some packing tools still write, are no longer rejected as "not a Source 2 VPK". They are read with their 12-byte header and, like any mod Modworks repairs or rebuilds, written back as version 2.
- Models compiled by newer tools preview again instead of failing with "KV3 value type is not supported". Their KV3 data uses two newer value types for unsigned 0 and 1, which are now read. Modworks writes them back as ordinary unsigned integers. An unknown type is now named in the error.
- **Apply to game** puts moved and removed mods back when a later copy fails or you cancel, instead of deleting them.
- A gzip download is kept. The compressed `Content-Length` is no longer compared to the decoded file.
- Archive extraction stops on a skipped file that expands past the size limit, and it does not open DOS device names such as `NUL.vpk`.
- **Add the line and apply** does not change `gameinfo.gi` while Deadlock is running.
- **Add the line and apply** no longer makes Deadlock crash with a failure to load user keybinds. The current `gameinfo.gi` has no `Mod` or `Write` lines, so putting `citadel/addons` first made it the game's MOD and write path, where user settings are read. Modworks now also adds `Mod citadel` and `Write citadel` after the addons line, matches the file's line endings, and offers to repair a `gameinfo.gi` that an earlier version patched.

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
