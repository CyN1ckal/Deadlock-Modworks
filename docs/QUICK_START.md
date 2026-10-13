# Quick start

This is the short path from the zip to one exported pack, then into Deadlock by hand, and back out. Labels below are the ones in this version of the window. There is no in-app **Open quick start** page.

The zip is `DeadlockModworks-0.1.0-win-x64.zip`. It is the release artifact. `v0.1.0` is not tagged, and that zip has not been published as a GitHub release. Extract a zip you were given, or one a maintainer produced with [RELEASING.md](RELEASING.md).

Screenshots in `docs/screenshots/` are captures of the real window with an empty library. They show the shell, not a finished pack. Two-model comparison was not captured.

## 1. Extract and launch

Extract the zip anywhere you can write. `Publish.ps1` puts `DeadlockModworks.exe`, `README.md`, `THIRD_PARTY_NOTICES.md`, `PACKAGE_MANIFEST.txt`, and `licenses\` in it, plus `LICENSE` when that file existed at package time. It does not put this `docs` folder in the zip. The README in the zip is the short first-run page.

Double-click `DeadlockModworks.exe`. You do not need a terminal or an administrator account. The window title is **Deadlock Modworks 0.1.0**. The menus are **File**, **Edit**, **View**, **Tools**, and **Help**.

The left **Explorer** has **SOURCES**, **GAME**, and **LIBRARY**. The workspace tabs are **Mod library**, **VPK builder**, **Version history**, **Skin repairs**, **Model inspector**, and **Multi view**.

If Windows reports that `vulkan-1.dll` is missing, install a driver with Vulkan 1.3 and start again. The zip does not include that DLL. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## 2. Connect Deadlock

On first launch the app looks for Deadlock through Steam. The status bar at the bottom, and **GAME** in the explorer, say **Deadlock connected** or **Deadlock not connected**. While it is opening the archive they say **Connecting to Deadlock...**.

If it is not connected:

1. Choose **File > Choose game archive...** (**Ctrl+G**), or the **...** button next to the game status.
2. Select this file, not a mod and not a chunk:

```text
<Steam library>\steamapps\common\Deadlock\game\citadel\pak01_dir.vpk
```

A right-click on the game status also has **Choose game archive...** and **Reconnect**.

You can browse mods that are already stored without a connection. Textures, **Skin repairs**, comparison with the game, and dependencies that exist only in the game need the connection. The builder shows **Connect Deadlock to resolve base-game assets.** when skins are selected and the game is not connected.

## 3. Import mods

Use any of these:

- **File > Add mods...** (**Ctrl+O**)
- **Add** in the explorer
- Drag `.vpk` files onto the window or onto `DeadlockModworks.exe`

The file dialog title is **Add reference mods (Ctrl / Shift to select multiple)**. **Ctrl-click** and **Shift-click** select more than one file.

**File > Add folder...** (**Ctrl+Shift+O**), or **Folder** in the explorer, adds every mod VPK directly in a folder, such as Deadlock's `addons` folder. Chunk files and the game archive are skipped. If the folder has no mod VPKs, the status line says `No mod VPKs found in <folder>.`

The empty library says **Your library has no mods yet.** and **Add or drop mod VPKs (Ctrl+O). Your source files stay available for future builds.** Import copies the files into the library. It does not modify the VPK you selected. Adding the same content again reuses what is already stored.

An empty source list says **No sources yet. Click Add, or drop mod VPKs on the window.**

## 4. Inspect a skin

On **Mod library**, double-click a skin, or right-click it and choose **Inspect** (**Enter**). That opens **Model inspector**. The empty inspector says to do this, and has **Browse skins** back to the library.

**Preview** shows the model. **Compare versions** is the other tab on that page. A compiled `.vmdl_c` opened by itself can be viewed. Its compare tab says **Comparison needs a model opened from a mod VPK.**

Camera: left-drag orbits, right-drag or middle-drag pans, the wheel zooms. **Frame** or **F** fits the model. The coloring list is **Textured**, **Clay**, **Material colors**, and **Bone colors**. **Bones** and **Grid** toggle those overlays. In **Textured** coloring, a mesh with no color map is drawn in clay.

## 5. Select what goes in the pack

On a skin card, the checkbox appears when the card is hovered or already included. **Space** does the same thing. The tooltip is **Include in pack** or **In the pack. Click to remove.**

Right-click a skin and use **Pack** to include or remove the shown skins, or every skin of that source. Right-click a source in the explorer for **Include every skin in the pack** and **Remove every skin from the pack**.

**In pack** on the library toolbar shows only the current selection. The **VPK builder** tab label includes the count, for example **VPK builder (0)**. **Edit > Clear pack** clears the selection. **Build VPK...** stays unavailable until something is selected.

## 6. Build the VPK

1. Connect the game if the pack contains skins whose dependencies come from Deadlock.
2. Open **VPK builder** (**Ctrl+2**). An empty pack says **The pack is empty.** and **Tick skins in the library, include a mod's other files, or right-click them and choose Include in pack.**
3. Choose **Build VPK...** (**Ctrl+B**) and pick a new `.vpk` name. The button tooltip says skin dependencies are included automatically, a package copies its other files as stored, shared files are deduplicated, and conflicting files block the export.
4. The status line says **Collecting files and validating your pack...** and then `Exported N files to <path>. Test the pack in Deadlock.` `N` is the file count and `<path>` is the path you chose.

**Cancel** on that page, **Tools > Cancel VPK build**, or **Cancel** in the status bar stops the export. A cancelled export reports **Cancelled.** Existing files are never overwritten. If the name is taken, the message is **Output already exists. Choose a new filename; existing files will not be overwritten.**

This build does not copy the VPK into Deadlock and does not show a **Reveal in Explorer** action for the exported file. The path is in the status line. Right-click that line and choose **Copy message**. **File > Open library folder** opens the library `store` folder, not the export.

## 7. Install the pack by hand

Close Deadlock first. Copy only the exported `.vpk` into:

```text
<Steam library>\steamapps\common\Deadlock\game\citadel\addons
```

That is the folder **Add folder...** describes as Deadlock's addons folder. Do not replace `pak01_dir.vpk` or the `pak01_###.vpk` chunk files beside it. The exporter also refuses to write its output next to those official archives.

Start Deadlock and see whether the pack behaves. Export does not repair models and does not guarantee that the game will accept the pack.

## 8. Remove the pack from the game

Close Deadlock. Move or delete that exported file from `addons`. Leave `pak01_dir.vpk` and the game's chunk files in place.

Removing the file from `addons` does not remove it from Modworks. **Remove from library** does not delete the exported file. See [USER_GUIDE.md](USER_GUIDE.md).

## When a step fails

| What you see | What to do |
| --- | --- |
| Windows says `vulkan-1.dll` is missing, or a Modworks dialog says the driver is not Vulkan 1.3 | Update the graphics driver. Do not copy a DLL from the Vulkan SDK into the zip. |
| `--data-root needs a directory.` | Pass a directory after `--data-root`, or omit the flag. |
| **Deadlock not connected** | **File > Choose game archive...** and select `pak01_dir.vpk` in `game\citadel`. |
| A mod is rejected, or the status line reports an error | Right-click the status line and choose **Copy message**. Earlier imports stay in the library. |
| **Conflicting file** and **Deselect one of the models using this file.** | The two sources named in the message disagree about one path. Remove one of them from the pack and build again. Nothing is written over an existing VPK. |
| **Missing dependency** and **Connect the installed game or use a complete source VPK.** | Connect Deadlock, or use a source VPK that already contains that file. |
| **Output already exists** | Choose a filename that is not already there. |
| **Cancelled.** | The export stopped. Pick the build again if you still want the file. |
| **The model library is damaged and was not replaced. Restore catalog.sqlite from a backup.** | Close the app and restore `store\catalog.sqlite` from a copy. The app will not replace a damaged catalog with an empty one. |

More cases are in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
