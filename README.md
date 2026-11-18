# Deadlock Modworks

Deadlock Modworks 0.1.0 is a Windows app for a local collection of Deadlock mods. It browses skins, inspects and compares models, repairs supported skeleton changes and pre-October 2026 camera attachments, keeps a version history, shares that library, and assembles selected models into one VPK. Assembly does not install the pack, and it does not cover every kind of mod.

It also downloads Deadlock mods from GameBanana (**Discover**, **Ctrl+2**) and turns library mods on or off in the game (**My mods**, **Ctrl+1**), the page it opens on: you tick mods, set their load order and profiles, and **Apply to game** places them in `game\citadel\addons`. Modworks only changes the files it placed there. See [My mods](docs/USER_GUIDE.md#my-mods) and [Discover](docs/USER_GUIDE.md#discover).

The program is `DeadlockModworks.exe`. The release artifact is `DeadlockModworks-0.1.0-win-x64.zip`. `v0.1.0` is not tagged, and a GitHub release has not been published. Publishing that zip is still a draft step. Until a maintainer publishes it, use a zip built with the steps in [Releasing](docs/RELEASING.md), or build from source. `scripts/Publish.ps1` was not run for this documentation update, so this commit did not produce a zip.

![Deadlock Modworks 0.1.0 with an empty library. The page shows Connect game, Add mods, and Open quick start](docs/screenshots/startup-empty-library.png)

That picture is `docs/screenshots/startup-empty-library.png`, a capture of the real window. The library is empty and shows **Connect game**, **Add mods**, and **Open quick start**. The capture includes desktop around the window. It predates the current page names and empty-library wording described below. A side-by-side comparison of two models was not captured. The **Quick start** popup was not opened for this picture.

## What you need

- 64-bit Windows.
- A graphics driver that supports Vulkan 1.3. `vulkan-1.dll` comes from that driver and is delay-loaded. It is not in the zip. A missing loader is supposed to show a message instead of failing in the static imports. That dialog was not observed.
- No Visual Studio, Vulkan SDK, installer, or administrator account for normal use. The executable is linked with the static MSVC runtime, so the zip does not include `VCRUNTIME` or `MSVCP` DLLs.

You can browse the library and add mods without a connected game. Textures stored in the game, comparison, repair, and skin packs that use base-game files need the game archive. A pack made only of other files is copied as stored and does not need the game.

There is no installer, automatic update, code signing, or GameBanana account sign-in. Downloading from GameBanana needs an internet connection. A clean-machine launch and code signing were not run.

## First run

1. Extract `DeadlockModworks-0.1.0-win-x64.zip`. No installer or administrator prompt is required.
2. Double-click `DeadlockModworks.exe`. A normal start does not need a terminal or a build tool.
3. The window opens on **My mods**. Until your first mod is in the game, a checklist above the list walks through **Find Deadlock**, **Get a mod**, **Turn it on** and **Apply to game**. The **Skins** page (**Ctrl+3**) says **No skins yet** and offers **Add mods...**, **Locate Deadlock** while the game is not found, and **Quick start**. **Help > Quick start** opens a modal titled **Quick start**. That popup was not clicked. Its eight steps are in the [quick start](docs/QUICK_START.md).
4. Steam fills the game archive when none is saved. The status bar says **Deadlock found** when that succeeds. You can still use **File > Locate Deadlock...** (**Ctrl+G**), or type `game\citadel` or `pak01_dir.vpk` under **Deadlock** on **Settings** (**Ctrl+,**) and choose **Use this path**.
5. Get a mod. Open **Discover** (**Ctrl+2**), paste a GameBanana link or search, and choose **Download & enable**. Or choose **Add mods...** (**Ctrl+O**) and select mod `.vpk` files you already have. You can also drop mod VPKs on the window. A drop was not performed.
6. On **My mods** (**Ctrl+1**), turn the mods you want on with their switches. When two change the same file, **Load order** sets which one wins.
7. Close Deadlock and choose **Apply to game** in the bar under the list. Start Deadlock. To take a mod out, turn it off and apply again. Modworks only changes the files it placed in `addons`.
8. After Deadlock updates, **My mods** offers **Check my mods**, and then **Fix n mods** for the ones Repairs can fix.

To combine skins from several mods into one, tick them on **Skins** (or press **Space**), open **Pack builder** and choose **Add to My mods...**. Give it a name and it appears on **My mods** like any other mod; building again under that name replaces it. **Build VPK file...** (**Ctrl+B**), on the arrow beside it, writes a `.vpk` instead. A modal titled **Pack assembled** then shows the summary and says assembly writes a VPK, does not install the pack, and does not guarantee compatibility. Close Deadlock and copy that file into `game\citadel\addons` yourself, without replacing `pak01_dir.vpk` or the game's chunk files. No pack was built for this update, so that modal was not shown.

The same steps, with the failure cases, are in the [quick start](docs/QUICK_START.md). The zip from `Publish.ps1` contains this README, not the `docs` folder. The longer guides are in the source repository.

The collection is stored in `%LOCALAPPDATA%\DeadlockModManager`. `--data-root <directory>` overrides that, and otherwise the environment variable `DEADLOCK_MODWORKS_DATA_ROOT` does. The flag wins when both are set.

Errors are appended to `<data-root>\logs\modworks.log`. With the default root that file is `%LOCALAPPDATA%\DeadlockModManager\logs\modworks.log`. A start with no error does not create it. **Help > Open logs** selects that file in Explorer when it exists. If it does not, the status line says `No log file yet. Errors are written to <path>.` **Help > Copy diagnostic details** copies the product title, the build commit, the data root, and the waiting error, or **No error is waiting.** Those two menu items were not clicked.

Saved preferences are `<data-root>\store\settings.json`. A corrupt `settings.json` is left as it is, the catalog is left in place, and an error is shown. A read-only `store\catalog.sqlite` is not replaced.

## Guides

- [Quick start](docs/QUICK_START.md) — extract, launch, connect, import, inspect, select, build, then install or remove.
- [User guide](docs/USER_GUIDE.md) — the workspace, shortcuts, comparison, repairs, history, sharing, and backups.
- [Troubleshooting](docs/TROUBLESHOOTING.md) — graphics, missing game files, bad mods, export conflicts, and storage.
- [Building](docs/BUILDING.md) — compile from source.
- [Releasing](docs/RELEASING.md) — how a maintainer builds the zip. The tag workflow has not been run.
- [Changelog](CHANGELOG.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)
- [License](LICENSE) — MIT, `Copyright (c) 2026 CyN1ckal`.

**Help > About** shows the product title and the build commit. `DeadlockModworks.exe --version` prints the same identity. **Help > Keyboard shortcuts** (**F1**) lists the shortcuts. **Help** also has **Quick start**, **Open logs**, and **Copy diagnostic details**.
