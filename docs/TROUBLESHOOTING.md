# Troubleshooting

There is no log folder and no **Open logs** or **Copy diagnostic details** command in this version. Collect the text the app already shows. [How to report it](#collecting-diagnostics) is at the end.

## The window never opens

`DeadlockModworks.exe` imports `vulkan-1.dll` from the graphics driver. The zip does not contain it, and there is no delay-load dialog of our own.

- If Windows says the program cannot start because `vulkan-1.dll` is missing, install the current driver for the GPU and confirm the driver supports Vulkan 1.3. Do not copy `vulkan-1.dll` out of a Vulkan SDK into the extracted folder. That DLL has to match the driver.
- If the DLL loads and startup still fails, Modworks shows a dialog titled **Deadlock Modworks 0.1.0**. The text is one of:
  - **No Vulkan device was found.**
  - **The Vulkan driver does not support version 1.3.**
  - **The Vulkan device cannot present to this window.**
  - `Vulkan initialization or rendering failed (N). Check that your graphics driver supports Vulkan 1.3.`
  - **Could not create the model tools window.**
- Update the graphics driver. The dialog names the Vulkan failure. The app does not have a control for choosing which GPU to open.
- `--data-root` with no directory shows **--data-root needs a directory.** and exits. The library is not opened. Pass a folder, or leave the flag off.

The static runtime means a missing `VCRUNTIME140.dll` or `MSVCP140.dll` is not a normal failure of this build. Those DLLs are not supposed to be beside the exe.

## Deadlock is not connected

**Deadlock not connected** means no game archive is open. **File > Choose game archive...** (**Ctrl+G**) expects:

```text
<Steam library>\steamapps\common\Deadlock\game\citadel\pak01_dir.vpk
```

The dialog title is **Select game\citadel\pak01_dir.vpk**. The citadel folder has to contain that file. Choosing a mod VPK, a chunk such as `pak01_000.vpk`, or a shortcut does not connect the game.

Automatic detection reads Steam's library list. If Steam is installed per-user in an unusual place, or the game is on a drive Steam is not showing, use **Choose game archive...**. **Reconnect** opens the last chosen archive again.

The red or warning text under **GAME** is the error. A missing or unreadable `pak01_dir.vpk` stays on screen until a later open succeeds.

Without the game you can still browse a library that is already imported. You cannot analyze repairs. Comparison with the game says **Set the game folder to compare this model with the installed game.** Export of a skin can stop with **Missing dependency ... Connect the installed game or use a complete source VPK.** The builder can also show **Connect Deadlock to resolve base-game assets.**

## A mod will not import

The status line and the explorer error text are the message. Right-click the status line and choose **Copy message**.

| Message | What it means |
| --- | --- |
| **That file is the game archive. Choose a mod VPK from the addons folder.** | You selected `pak01_dir.vpk` that has chunk files beside it. Choose a mod from `game\citadel\addons`, or another folder. |
| **Choose a mod outside the game's archive folder, such as a VPK in addons.** | The file sits next to the game's `pak01_dir.vpk`. Move the mod, or select the copy that already lives in `addons`. |
| **Import the directory VPK, not a chunk file.** | `*_###.vpk` is a chunk. Import the matching directory VPK. **Add folder...** skips chunks on its own. |
| **Could not find the mod VPK.** | The path is gone. |
| **VPK directory or embedded data is outside the file.** or another read error | The file is truncated or not a VPK. The library keeps snapshots that were already stored. |
| **No mod VPKs found in ...** | **Add folder...** found nothing it considers a mod VPK. |
| `<file> is damaged: a stored mod does not match its checksum.` | A blob already in the library does not match the catalog. Do not keep importing over it. Restore `store` from a backup. |
| **The model library is in use by another process.** | Another Modworks process has the catalog open. Close it. |

A tree in the gallery, **N source archives could not be read**, lists sources whose rebuilt copy could not be opened. The source stays in the collection. **Refresh** (**F5**) tries again.

**No preview** on a card means the thumbnail was not drawn. Hover the card for the error. Connecting the game supplies most textures. A mesh can still draw in clay when a color map is missing. That is the viewport's **Textured** fallback, not a failed import.

## Export conflicts and other build failures

**Build VPK...** writes nothing until the output has been validated. These messages are the whole failure. Fix the selection and build to a new name.

| Message | What to do |
| --- | --- |
| **Conflicting file** followed by two paths and **Deselect one of the models using this file.** | Two selected sources put different bytes at one internal path. Remove one model or one included package from the pack. |
| **Missing dependency ... Connect the installed game or use a complete source VPK.** | Connect `pak01_dir.vpk`, or select a source that already contains that resource. Another mod in the library is not searched unless you selected it. |
| **Output already exists. Choose a new filename; existing files will not be overwritten.** | Pick a path that is not already a file or a directory. This also applies to **Share library...** and **Assemble...**. |
| **Choose an output distinct from every source archive.** | Do not export onto a VPK the pack is reading. |
| **Output must be a .vpk file.** | Keep the `.vpk` extension. |
| **Select at least one model.** | The pack was empty. |
| **Selected model is missing: ...** | The skin is no longer in that archive. **Refresh**, or remove it. The builder lists **N skins in the pack are missing from your library**, with **Remove** and **Remove all**. |
| **Cancelled.** | **Cancel** or **Tools > Cancel VPK build** stopped the run before a new file was published. |
| **Could not publish the archive.** or **Could not write ... Windows error N** | The folder is read-only, full, or locked. The previous file at a different path is left as it was. |

A successful line is `Exported N files to <path>. Test the pack in Deadlock.` The pack is still not installed. Copy it by hand. Steps are in [QUICK_START.md](QUICK_START.md).

## Repair results that are not a fix

**Needs review** is left untouched. Read the detail and the warnings in the row tooltip. Typical causes are a dependency the game and the mod do not resolve, a rig the migration will not rewrite, or a structure this repair does not support.

**Skipped** means no supported hero model. Other mod types are out of scope for 0.1.

**Failed** is on the row. **Copy detail** is on the right-click menu. If the message is about the game folder, connect `citadel` again. **Could not find pak01_dir.vpk in the game folder.** means the connected path's folder does not contain the archive.

**Current** means this repair had nothing supported to change. It does not mean the mod is compatible with the latest Deadlock update.

If a repair is interrupted, **Cancelled. Completed results are shown below.** A repair that fails validation does not leave a `.new` file in place of the archive. The version from before a successful repair remains in **Version history**.

Command-line `--repair` is harsher than the library button: it replaces the path you gave it. Undo is in [USER_GUIDE.md](USER_GUIDE.md). `--update` writes a second file and does not replace the input.

## Storage errors

| Message | What to do |
| --- | --- |
| **The model library is damaged and was not replaced. Restore catalog.sqlite from a backup.** | Close Modworks. Replace `store\catalog.sqlite` with a copy made while the app was closed. Do not delete the damaged file and launch in the hope of a new empty library. The app refuses to create a replacement catalog over a file it considers damaged. |
| **Unsupported model library schema.** | This build cannot open that catalog. Use the build that wrote it, or restore a catalog from this version. |
| **The model library is in use by another process.** | Close the other copy. A file lock on `catalog.sqlite` from a backup tool causes the same message. |
| **Could not find the local app data folder.** | `%LOCALAPPDATA%` is unavailable. Start the app with `--data-root` pointed at a folder you can write. |
| **Local library storage is unavailable.** | The explorer shows this when the store did not open. The red text above it is the reason. |
| **Output already exists** while sharing | Choose a new `.db` name. Sharing does not overwrite, and a failed share deletes its temporary file instead of replacing your library. |

`cache` is disposable. If a rebuilt VPK in `cache` is corrupt, close the app, delete `cache`, and start again. The catalog rebuilds the copies it still lists. Do not delete `store\blobs` to "clean up" unless you have a backup and intend to lose the stored mods.

**Remove from library** does not free that disk space. The difference is in [USER_GUIDE.md](USER_GUIDE.md).

## Collecting diagnostics

Include:

1. The version. **Help > About** shows **Deadlock Modworks 0.1.0** and **Build** plus the commit. From a terminal, `DeadlockModworks.exe --version` prints `Deadlock Modworks 0.1.0 (<commit>)`.
2. What you did, what you expected, and the exact text you saw. Right-click a status line and choose **Copy message**. Copy the startup dialog if there was one. For repairs, attach the JSON from **Save report...** if the report is not huge and does not contain paths you consider private.
3. Whether the game was connected, and whether you used `--data-root` or `DEADLOCK_MODWORKS_DATA_ROOT`. The path itself is enough. Do not zip the data root.
4. Windows version and the GPU name, if the failure is a black window or a Vulkan dialog.

Do not send `%LOCALAPPDATA%\DeadlockModManager`, `catalog.sqlite`, `blobs`, or your mod VPKs. Those are your library, not a bug report. A single small VPK is useful only if you choose to share that one file.

There is no in-app log to attach. If the process is started from a terminal, Vulkan failures also print `[vulkan] VkResult = N` on that terminal before the dialog. A double-click from Explorer does not keep a console.
