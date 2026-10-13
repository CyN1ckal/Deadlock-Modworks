# Quick start

**Help > Quick start**, and **Open quick start** on an empty library, open a modal titled **Quick start**. The modal says these steps are the whole first run, and that assembly does not install the pack. The eight steps are the sentences from `QuickStartSteps()`. There is no ninth step. The popup itself was not clicked; the sentences below are the ones in source.

The zip is `DeadlockModworks-0.1.0-win-x64.zip`. It is the release artifact. `v0.1.0` is not tagged, and that zip has not been published as a GitHub release. `scripts/Publish.ps1` was not run for this documentation update. Extract a zip you were given, or one a maintainer produced with [RELEASING.md](RELEASING.md). `LICENSE` is MIT, `Copyright (c) 2026 CyN1ckal`.

`docs/screenshots/startup-empty-library.png` is a capture of the real window with an empty library. It shows **Connect game**, **Add mods**, and **Open quick start**. It does not show the **Quick start** popup. Two-model comparison was not captured. Other startup captures are `startup-corrupt-settings.png`, `startup-readonly-data-root.png`, and `startup-missing-data-root.png`.

## Extract

Extract the zip. No installer or administrator prompt is required.

`Publish.ps1` puts `DeadlockModworks.exe`, `README.md`, `THIRD_PARTY_NOTICES.md`, `PACKAGE_MANIFEST.txt`, and `licenses\` in the zip, plus `LICENSE` when that file existed at package time. It does not put this `docs` folder in the zip. The README in the zip is the short first-run page.

## Launch

Start Deadlock Modworks. A normal start does not need a terminal or a build tool.

Double-click `DeadlockModworks.exe`. The window title is **Deadlock Modworks 0.1.0**. The menus are **File**, **Edit**, **View**, **Tools**, and **Help**.

The left **Explorer** has **SOURCES**, **GAME**, and **LIBRARY**. The workspace tabs are **Mod library**, **VPK builder**, **Version history**, **Skin repairs**, **Model inspector**, and **Multi view**.

`vulkan-1.dll` is delay-loaded from the graphics driver. The zip does not include it. A missing loader is supposed to show **vulkan-1.dll was not found. Install or update the graphics driver that provides the Vulkan loader, then start Deadlock Modworks again. The model library was not opened.** That dialog was not observed. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Connect

Connect the game. A Steam install is used when none is saved. You can type a path or browse to `game\citadel`.

If no game path was saved and `--game` was not passed, launch looks for Deadlock through Steam. A `gameDirectory` already in `store\settings.json`, a game archive stored with the library, or `--game` is used instead of that search. The status bar, and **GAME** in the explorer, say **Deadlock connected** or **Deadlock not connected**. While the archive is opening they say **Connecting to Deadlock...**.

The explorer keeps a manual path. The field hint is **game\citadel or pak01_dir.vpk**. **Connect** uses a typed path. **Browse...** and **File > Choose game archive...** (**Ctrl+G**) open the file dialog. **Connect game** on an empty library uses a typed path when the field is filled, and otherwise opens that dialog.

Select this file, not a mod and not a chunk:

```text
<Steam library>\steamapps\common\Deadlock\game\citadel\pak01_dir.vpk
```

A right-click on the game status also has **Choose game archive...** and **Reconnect**.

You can browse the library and add mods without a connected game. Textures stored in the game, comparison, repair, and skin packs that use base-game files need the game archive, because those bytes are not in the mod. A pack made only of other files is copied as stored and does not need the game. The builder shows **Connect Deadlock to resolve base-game assets.** when skins are selected and the game is not connected.

## Import

Get mods from GameBanana on the Get mods page, add mod VPKs, or drop them on the window. The library can be browsed before a game is connected.

To get a mod from GameBanana, open **Get mods** (**Ctrl+8**), paste the mod's link or search for it, open it, and choose **Download & enable** on the file you want. The [user guide](USER_GUIDE.md#get-mods) has the details.

To add VPKs you already have, use any of these:

- **Add mods** on an empty library
- **File > Add mods...** (**Ctrl+O**)
- **Add** in the explorer

The empty page says you can drop mod VPKs on the window. A drop onto the window was not performed.

The file dialog title is **Add reference mods (Ctrl / Shift to select multiple)**. **Ctrl-click** and **Shift-click** select more than one file.

**File > Add folder...** (**Ctrl+Shift+O**), or **Folder** in the explorer, adds every mod VPK directly in a folder, such as Deadlock's `addons` folder. Chunk files and the game archive are skipped. If the folder has no mod VPKs, the status line says `No mod VPKs found in <folder>.`

An empty library says **Your library has no mods yet.** Then:

> You can browse the library and add mods without a connected game. Textures stored in the game, comparison, repair, and skin packs that use base-game files need the game archive, because those bytes are not in the mod. A pack made only of other files is copied as stored and does not need the game.

Then **Building a pack does not install it. Open quick start for how to install or remove one.** The buttons are **Connect game**, **Add mods**, and **Open quick start**. Under them: **You can also drop mod VPKs on the window. Your source files stay available for later builds.**

An empty source list says **No sources yet. Connect the game, add mods, or open the quick start from the library.**

Import copies the files into the library. It does not modify the VPK you selected. Adding the same content again reuses what is already stored.

## Inspect

Open a skin to view its model. Textures and the original skeleton that live in the game archive need the connected game.

On **Mod library**, double-click a skin, or right-click it and choose **Inspect** (**Enter**). That opens **Model inspector**. The empty inspector says to do this, and has **Browse skins** back to the library.

**Preview** shows the model. **Compare versions** is the other tab on that page. A compiled `.vmdl_c` opened by itself can be viewed. Its compare tab says **Comparison needs a model opened from a mod VPK.** A side-by-side capture of two loaded models was not made.

Camera: left-drag orbits, right-drag or middle-drag pans, the wheel zooms. **Frame** or **F** fits the model. The coloring list is **Textured**, **Clay**, **Material colors**, and **Bone colors**. **Bones** and **Grid** toggle those overlays. In **Textured** coloring, a mesh with no color map is drawn in clay.

## Select

Tick the skins and other files that belong in the pack.

On a skin card, the checkbox appears when the card is hovered or already included. **Space** does the same thing. The tooltip is **Include in pack** or **In the pack. Click to remove.**

Right-click a skin and use **Pack** to include or remove the shown skins, or every skin of that source. Right-click a source in the explorer for **Include every skin in the pack** and **Remove every skin from the pack**.

**In pack** on the library toolbar shows only the current selection. The **VPK builder** tab label includes the count, for example **VPK builder (0)**. **Edit > Clear pack** clears the selection. **Build VPK...** stays unavailable until something is selected.

## Build

Build the VPK. Assembly writes a file. It does not install the pack and does not guarantee compatibility.

1. Connect the game if the pack contains skins whose dependencies come from Deadlock.
2. Open **VPK builder** (**Ctrl+2**). An empty pack says **The pack is empty.** and **Tick skins in the library, include a mod's other files, or right-click them and choose Include in pack.**
3. Choose **Build VPK...** (**Ctrl+B**) and pick a new `.vpk` name. The button tooltip says skin dependencies are included automatically, a package copies its other files as stored, shared files are deduplicated, and conflicting files block the export.
4. The status line says **Collecting files and validating your pack...** and then `Exported N file to <path>.` when one file is written, or `Exported N files to <path>.` otherwise. `N` is the file count and `<path>` is the path you chose.

**Assemble...** on **Version history** uses `Assembled a pack at <path>.` for its result.

Either way, a modal titled **Pack assembled** shows that summary, then **Assembly writes a VPK. It does not install the pack and does not guarantee compatibility with Deadlock.** Then **Install or remove the pack from Deadlock yourself. The quick start has those steps.** The buttons are **Reveal in Explorer**, **Open quick start**, and **Close**. No pack was built while these notes were written, so that modal was not shown.

**Cancel** on that page, **Tools > Cancel VPK build**, or **Cancel** in the status bar stops the export. A cancelled export reports **Cancelled.** Existing files are never overwritten. If the name is taken, the message is **Output already exists. Choose a new filename; existing files will not be overwritten.**

The status line still accepts a right-click **Copy message**. **File > Open library folder** opens the library `store` folder, not the export. **Reveal in Explorer** on **Pack assembled** is the control that selects the exported file.

## Install or remove

Turn mods on or off on the Mods page and choose Apply to game while Deadlock is closed. A pack you build is not installed; copy it into the addons folder yourself.

### Mods from your library

1. Open **Mods** (**Ctrl+7**). Tick the mods you want. Drag them to set the load order; the higher mod wins when two change the same file.
2. Close Deadlock and choose **Apply to game**. If `gameinfo.gi` does not load the addons folder, the prompt offers to add that line and keeps a backup.
3. Start Deadlock. To take a mod out, untick it and choose **Apply to game** again.

Modworks only touches the `pakNN_dir.vpk` files it placed, which it records in `addons\.modworks.json`. Mods you or another manager put in `addons` stay where they are. See [USER_GUIDE.md](USER_GUIDE.md#mods).

### A pack you built

Close Deadlock first. Copy only the exported `.vpk` into:

```text
<Steam library>\steamapps\common\Deadlock\game\citadel\addons
```

That is the folder **Add folder...** describes as Deadlock's addons folder. Do not replace `pak01_dir.vpk` or the `pak01_###.vpk` chunk files beside it. The exporter also refuses to write its output next to those official archives.

Start Deadlock and see whether the pack behaves. Export does not repair models and does not guarantee that the game will accept the pack.

To remove the pack, close Deadlock and move or delete that exported file from `addons`. Leave `pak01_dir.vpk` and the game's chunk files in place. Removing the file from `addons` does not remove it from Modworks. **Remove from library** does not delete the exported file. See [USER_GUIDE.md](USER_GUIDE.md). Installing and removing a pack was not performed for this note.

## When a step fails

| What you see | What to do |
| --- | --- |
| **vulkan-1.dll was not found. Install or update the graphics driver that provides the Vulkan loader, then start Deadlock Modworks again. The model library was not opened.** The loader is delay-loaded. This dialog was not observed. | Update the graphics driver that provides the Vulkan loader. Do not copy a DLL from the Vulkan SDK into the zip. |
| A Modworks dialog says no Vulkan device was found, or the driver is not Vulkan 1.3 | Update the graphics driver. There is no GPU chooser. |
| `--data-root needs a directory.` | Pass a directory after `--data-root`, or omit the flag. `docs/screenshots/startup-missing-data-root.png` shows that box. |
| **Deadlock not connected** | Type a path and choose **Connect**, or use **Browse...** / **File > Choose game archive...** and select `pak01_dir.vpk` in `game\citadel`. |
| **settings.json could not be read. Saved preferences were left unchanged, and the model library was not replaced.** | The file and `store\catalog.sqlite` stay in place. Fix `store\settings.json` yourself. Do not delete the catalog to get past this. `docs/screenshots/startup-corrupt-settings.png` shows the box. |
| **Deadlock Modworks cannot write its data folder at ... The model library was not replaced.** | The data root or `catalog.sqlite` is not writable. A read-only catalog is not replaced. Use `--data-root` on a folder you can write, or clear the read-only attribute. `docs/screenshots/startup-readonly-data-root.png` shows the box. |
| A mod is rejected, or the status line reports an error | Right-click the status line and choose **Copy message**, or use **Help > Copy diagnostic details**. Earlier imports stay in the library. |
| **Conflicting file** and **Deselect one of the models using this file.** | The two sources named in the message disagree about one path. Remove one of them from the pack and build again. Nothing is written over an existing VPK. |
| **Missing dependency** and **Connect the installed game or use a complete source VPK.** | Connect Deadlock, or use a source VPK that already contains that file. |
| **Output already exists** | Choose a filename that is not already there. |
| **Cancelled.** | The export stopped. Pick the build again if you still want the file. |
| **The model library is damaged and was not replaced. Restore catalog.sqlite from a backup.** | Close the app and restore `store\catalog.sqlite` from a copy. The app will not replace a damaged catalog with an empty one. |

**Help > Open logs** selects `<data-root>\logs\modworks.log` in Explorer when that file exists. The default data root is `%LOCALAPPDATA%\DeadlockModManager`, so the default file is `%LOCALAPPDATA%\DeadlockModManager\logs\modworks.log`. A start with no error does not create the log. **Help > Copy diagnostic details** copies the product title, build commit, data root, and the waiting error. Those two menu items were not clicked.

More cases are in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
