# User guide

Deadlock Modworks 0.1.0 is a desktop workspace for hero-model mods. It is not a marketplace, and **Build VPK...** does not install the file it writes. The procedural path is [QUICK_START.md](QUICK_START.md). Menu names below are the ones drawn by this version.

The captures in `docs/screenshots/` are the real window. `library.png`, `builder.png`, `history.png`, `repairs.png`, `inspector.png`, `shortcuts.png`, and `data-root-error.png` are an earlier pass with no model loaded. `library.png` is cropped to the window. `startup-empty-library.png` is the later empty library and shows **Connect game**, **Add mods**, and **Open quick start**. `startup-corrupt-settings.png`, `startup-readonly-data-root.png`, and `startup-missing-data-root.png` are the message boxes for those startup failures. A comparison of two loaded models was not captured, so this guide does not pretend that `inspector.png` shows one. The **Quick start** popup was not opened, and a drop onto the window was not performed.

## The workspace

The window title is **Deadlock Modworks 0.1.0**. The menu bar is **File**, **Edit**, **View**, **Tools**, and **Help**.

**Explorer** is the left pane. Its sections are **SOURCES**, **GAME**, and **LIBRARY**. **Workspace** is the tool area. The tabs are:

| Tab | Shortcut | What it is |
| --- | --- | --- |
| **Mod library** | Ctrl+1 | Skins and other files in the collection |
| **VPK builder** | Ctrl+2 | The current pack, then **Build VPK...** |
| **Version history** | Ctrl+3 | Stored models, packs, and versions |
| **Skin repairs** | Ctrl+4 | Analyze and repair supported skeleton changes |
| **Model inspector** | Ctrl+5 | One model: **Preview** and **Compare versions** |
| **Multi view** | Ctrl+6 | Up to four models of one character |
| **Mods** | Ctrl+7 | Enabled mods, load order, profiles, and **Apply to game** |
| **Get mods** | Ctrl+8 | Browse, search and download Deadlock mods from GameBanana |

**View > Explorer** (**Ctrl+Shift+E**) shows or hides the explorer. **View > Workspace** focuses the workspace. **View > Status bar** shows or hides the bar along the bottom. **View > Back** (**Alt+Left**) returns to the previous page. **View > Reset layout** puts the explorer on the left and the workspace on the right again. The tooltip says mods, the catalog, and thumbnails stay. It does not clear the library and it does not move the window. **View > Refresh library** (**F5**) reopens every source and reloads thumbnails.

The first launch with no saved layout docks **Explorer** and **Workspace**. Later launches restore `layout.ini`. A floating **Explorer** or **Workspace** that does not overlap the work area is moved back on screen. Docked panes stay docked. Reset layout deletes only `layout.ini`.

The status bar starts with **Deadlock connected**, **Connecting to Deadlock...**, or **Deadlock not connected**. It then shows import, library, repair, or history work, with **Cancel** while that work runs. The right side counts what is shown, how many skins and sources are loaded, and how many are in the pack.

**Help > About** shows the product title, **Build** and the git commit, and **Dear ImGui** with **Vulkan 1.3**. **Help > Keyboard shortcuts** (**F1**) opens the shortcut list. **Help > Quick start** opens a modal titled **Quick start**. **Help > Open logs** selects `<data-root>\logs\modworks.log` in Explorer when that file exists. With the default root the file is `%LOCALAPPDATA%\DeadlockModManager\logs\modworks.log`. If the file is missing, the status line says `No log file yet. Errors are written to <path>.` A start with no error does not create the log. **Help > Copy diagnostic details** copies the product title, the build commit, the data root, and the waiting error, or **No error is waiting.** The quick-start popup and those two menu items were not clicked.

## Keyboard shortcuts

These are the rows in the **Keyboard shortcuts** dialog.

| Keys | Action in the dialog |
| --- | --- |
| Ctrl+O | Add mods |
| Ctrl+Shift+O | Add every mod in a folder |
| Ctrl+G | Choose the game archive |
| Ctrl+B | Build the pack as a VPK |
| Ctrl+F | Search skins, packages and sources |
| F5 | Reopen every source and reload thumbnails |
| Ctrl+1 ... Ctrl+8 | Library, builder, history, repairs, inspector, multi view, mods, get mods |
| Alt+Left | Back to the previous page |
| Ctrl+Shift+E | Show or hide the explorer |
| View > Workspace | Focus the workspace |
| View > Reset layout | Restore the explorer and workspace split |
| Double-click | Inspect a skin, list a package's files, or view a version |
| Enter / Space | Inspect a skin or list a package's files / include it in the pack |
| Ctrl+Wheel | Thumbnail size in the gallery |
| Drag / right-drag / wheel | Orbit / pan / zoom a viewport |
| F | Frame the model in the hovered viewport |
| Right-click | More actions on skins, sources, versions, results and viewports |
| F1 | This list |

**Enter** opens the skin or the package file list. **Space** includes or removes the highlighted skin or package. In a viewport, left-drag orbits. Right-drag and middle-drag both pan. The wheel zooms. **F** frames the hovered viewport. **Ctrl** and the wheel, over the gallery, changes thumbnail size between 80 and 256. The **Grid** page also has a slider labeled in px.

## Mod library

**Add** and **File > Add mods...** take `.vpk` and `.vmdl_c` files. **Folder** and **File > Add folder...** take every mod VPK in one folder. Chunk names and the game archive (`pak01_dir.vpk` with its chunk files beside it) are skipped. A single-file mod that happens to be named `pak01_dir.vpk` is not treated as the game archive.

The window accepts dropped files, and the empty library says you can drop mod VPKs on it. A drop was not performed. Dropping files on the executable starts the app with those files. That launch path was not exercised for this note.

An empty library says **Your library has no mods yet.** Then the paragraph that browsing and adding mods work without a connected game, and that textures in the game, comparison, repair, and skin packs that use base-game files need the game archive. A pack made only of other files is copied as stored and does not need the game. The next line is **Building a pack does not install it. Open quick start for how to install or remove one.** The buttons are **Connect game**, **Add mods**, and **Open quick start**. Under them: **You can also drop mod VPKs on the window. Your source files stay available for later builds.** An empty source list says **No sources yet. Connect the game, add mods, or open the quick start from the library.**

**Connect game** uses the explorer's typed path when that field is filled, and otherwise opens **Choose game archive...**. Steam fills the game archive when none is saved. **Connect** and **Browse...** stay under **GAME**. The field hint is **game\citadel or pak01_dir.vpk**.

The toolbar has **Refresh**, the search field **Search skins, packages and sources (Ctrl+F)**, a source list whose first entry is **All sources**, **In pack** (on the library page only), **Grid**, and **List**. **Grid** is the thumbnail grid. **List** is a sortable table with columns for the pack checkbox, a thumbnail, **Name**, **Model path**, and **Source**.

**No mods match.** appears when filters hide everything. **Clear filters** resets the search, the source, and **In pack**. While archives are opening, the page can say **Loading your collection...** or `Reading N source archives...`.

A card with no image says **Loading...** or **No preview**. Hover the card. The tooltip names the model and, when rendering failed, the error in red. The last line is **Double-click to inspect. Right-click for more.**

Right-click a skin:

- **Inspect** (**Enter**)
- **Multi view**, when the path is a hero model
- **Analyze this model**, when the game is connected and the library is not busy
- **Include in pack** or **Remove from pack** (**Space**)
- **Pack** — include or remove the skins currently shown, or every skin of that source
- **Show only this source**
- **Copy name**, **Copy model path**, **Copy source path**
- **Reveal source in Explorer**

**Reveal source in Explorer** opens the library's rebuilt copy, under `cache`, not necessarily the folder you imported from.

Right-click a non-model package card for **Show files** (**Enter**), which opens **Package files**, plus **Include in pack** and **Reveal source in Explorer**.

Right-click a source in the explorer:

- **Show skins**
- **Include every skin in the pack** / **Remove every skin from the pack**
- **Include other files in the pack** / **Remove other files from the pack**, when the source has files that are not skins
- **Rename...**
- **Copy path** and **Reveal in Explorer**
- **Remove from library**

**Rename...** changes the name used for the rebuilt copy. An empty name goes back to the derived name. The next load uses the new file name. **Edit > Show every source** clears a source filter. **Edit > Find skins** (**Ctrl+F**) focuses the search.

**File > Open library folder** opens the `store` directory (the catalog and blobs), not the whole data root.

Import does not change the original file. After a successful import, that original can be moved. The gallery uses the copy rebuilt under `cache`.

## VPK builder

The builder is the same listing, restricted to the pack, plus a line such as **0 skins in pack**, **Build VPK...**, and **Clear pack**. **File > Build VPK...** (**Ctrl+B**) is enabled only when the pack is not empty and no import, repair, or build is running.

**Build VPK...** asks for a new `.vpk`. The status line then says **Collecting files and validating your pack...**. On success it says `Exported N file to <path>.` or `Exported N files to <path>.`

What the export includes:

- Each selected model, then the compiled resources it references, including supported KV3 references, from that model's own archive.
- Files from a source you included with **Include other files in the pack**. Model files in that walk are skipped because skins are selected on their own. A package with no non-model files is rejected.
- One copy of identical bytes at the same internal path.

What it does not include:

- Base-game files that are not overridden. Those stay in the game. With the game connected, a reference that is in neither the mod nor the game is left out (the game ignores a stale reference). With the game not connected, a missing reference blocks the export: **Missing dependency ... Connect the installed game or use a complete source VPK.**
- Dependencies that exist only in some other mod you did not select.
- Unreferenced scripts, sounds, or other extras, unless you included that source's other files.
- A repair. Building a pack does not run **Skin repairs**.

Different bytes at the same path fail with **Conflicting file**, the two source paths, and **Deselect one of the models using this file.** No output is published in that case. **Output already exists. Choose a new filename; existing files will not be overwritten.** The destination must not be one of the source archives.

**Tools > Cancel VPK build** stops the run. The message is **Cancelled.**

The app does not copy the result into `addons`. Right-click the status line for **Copy message** or **Dismiss**. That menu is not **Reveal in Explorer**. After a successful build, a modal titled **Pack assembled** shows the summary, then **Assembly writes a VPK. It does not install the pack and does not guarantee compatibility with Deadlock.** Then **Install or remove the pack from Deadlock yourself. The quick start has those steps.** The buttons are **Reveal in Explorer**, **Open quick start**, and **Close**. No pack was built for this note, so that modal was not shown. To try the pack in Deadlock, close the game and copy the `.vpk` into `game\citadel\addons`. Do not replace `pak01_dir.vpk` or the game's chunk files. Close the game and remove that file from `addons` to uninstall it. See [QUICK_START.md](QUICK_START.md).

## Model inspector

The page title is **Model inspector**, or **Inspect:** plus the file name once a model is open. The **<** button goes back.

**Preview** has the viewport and a side pane:

- **Model** — path, triangles, vertices, meshes shown, bones, LODs, textured materials, and whether clips can play.
- **Meshes** — **All**, **None**, and **Reset**. Each row has a checkbox. Right-click a mesh for **Show only this mesh**, **Hide** or **Show**, **Show all meshes**, and **Copy name**.
- **Material notes**, when the model has any.

The viewport toolbar is **Bones**, **Grid**, a coloring list (**Textured**, **Clay**, **Material colors**, **Bone colors**), **Frame**, and an **LOD** slider when the model has more than one level. Right-click the viewport for **Frame model**, **Show bones**, **Show grid**, the same coloring entries, and **Level of detail**.

The clip list shows **Bind pose** or **No animation clips**. **Play** and **Pause** start and stop. The speed slider tooltip is **Playback speed**. Clips come from the connected game when the model uses them. Without an animation skeleton the **Model** pane says **No animation skeleton**.

Right-click the model path in the inspector header for **Copy model path**, and, when the model came from a source, **Copy source path**, **Reveal source in Explorer**, **Show source in library**, **Multi view**, and **Analyze this model**.

## Compare versions

Open **Compare versions** on the inspector. The empty page says **Inspect a skin from the library to compare it with the game.**

Each side is a list of **Mod (before)**, **Repaired (after)**, and **Game (current)**. Entries that were not produced are disabled. A side with nothing to draw says **Not available**. **Swap** exchanges the two sides. **Render** compares the render skeleton (the bones the mesh is skinned to). **Animation** compares the animation skeleton. **Link** moves both cameras together. The viewport menu also has **Link cameras** and **Swap sides**.

**Repaired (after)** is a proposed repair held for the preview. Looking at it does not write a VPK. The **Repair** section states **Preview of the model and animation-skeleton migrations only.** A full update also checks the archive's other dependent resources. That happens when you run a repair, not when you only look at this tab.

**Differences** reports the render or animation skeleton as identical or as counts of **added**, **moved**, **reparented**, and **removed** bones. The bone table columns are **Bone**, **Change**, **Moved**, and **Parent**. **Show unchanged bones** includes rows marked **same**. Skin bindings are either unchanged, not compared, or reported as vertices driven by different bones.

**Play** on this tab plays the clip on both viewports. **clip mismatch** means that viewport could not apply the clip. If the game is not connected, comparison stops with **Set the game folder to compare this model with the installed game.**

## Multi view

Right-click a skin and choose **Multi view**, or open the tab and pick **Character**. The count list is **1**, **2**, **3**, or **4**. **Link** keeps orbit, pan, and zoom on one camera. One clip plays on every viewport that has an animation skeleton.

The page fills viewports from `models/heroes/<character>/...`: the model you opened, then one model from each other mod of that character, then further files from those mods. Each viewport can show another model of that character. An empty slot says **Choose a model** or **Empty**. With nothing selected the page says **Right-click a skin in the library and choose Multi view, or pick a character above.**

## Skin repairs

Close Deadlock, and wait until Steam is finished updating it, before repairing. The help marker on this page says the same thing.

**Analyze** checks every mod in the library against the installed game. It does not write a VPK. **Tools > Analyze mods** opens this page and starts that check. Without a game, the page says **Connect Deadlock (File > Choose game archive) to enable analysis.**

The idle line is **Ready to analyze. Repairs save a new version in your library and keep the original.**

Results are a table with **Outcome**, **Archive**, **Model**, and **Detail**. Hover a row for the full text, warnings, and a backup path when one was written. Click an outcome count in the toolbar to hide or show those rows. **Filter results by archive, model or detail** narrows the table.

| Outcome | Meaning |
| --- | --- |
| Repairable | A supported fix passed the checks and can be applied. |
| Updated | The repaired archive was written and validated. |
| Current | No supported rig change was required. This is not a promise that the mod works in Deadlock. |
| Needs review | Something the automatic repair will not change. JSON and the command line use the name `NeedsReview`. |
| Failed | The file could not be read, validated, or repaired. |
| Skipped | No supported hero model was found. The detail for a whole archive with none is **No supported hero models. This MVP cannot assess this mod.** |

**Repair N supported** repairs every analyzed archive that can be repaired. When you checked one model, the button is **Repair this model** and other models in that archive are left as they are. **Tools > Repair supported mods** is the same repair. **Cancel** stops after the current archive. Completed rows stay. **Tools > Cancel repair** is the same control.

The repair runs on the library's copy of the archive. Your original download is not the file that is replaced. The previous contents stay in the library as an older version. The file that was replaced also gets a backup beside it, named `.bak-` plus a timestamp and an id. Right-click the result and choose **Reveal backup in Explorer** when **Backup:** is shown. Afterward the status says the original archive was kept, or **Repair finished. Review the results and test the mod in Deadlock.**

**Save report...** writes JSON. The suggested name is `model-report.json`. The status line says **Saved** and the path, or the error.

Supported repair is VPK version 2, hero `.vmdl_c` models, and AG2 `.vnmskel_c` animation-skeleton overrides. It can migrate compatible animation bindings, unambiguous case-only bone renames, and supported embedded skeletons, and it keeps authored poses and skin bindings. It does not treat every difference from the game as a defect. Custom proportions, helper bones, cloth, and reference poses can be intentional. It does not invent arbitrary renames, recompile source files, or rewrite animation clips, physics, or cloth. It reads the mod being repaired and the installed game, not the other addons in the library.

Changing the game or the library clears the analysis. Selection is disabled while analysis, repair, or import is running.

## Version history

The left column has **Models (N)** and **Packs (N)**. Model columns are **Model**, **Packs**, and **Hash**. Pack columns are **Pack**, **Models**, **Seen**, and **Observed**. The prompt when nothing is selected is **Select a model to see its versions and copies.**

The toolbar is **View**, **Compare A/B**, **Compare with game**, **Repair**, **Assemble...**, and **Refresh**.

Click a version to make it **A**. The version that was **A** becomes **B**. **View** opens **A** in the inspector. **Compare A/B** needs two different versions. **Compare with game** needs a connected game. Double-click a version to view it.

Right-click a version for **View**, **Compare with A**, **Compare with game**, **Set as A**, **Set as B**, **Copy hash**, and **Copy line**.

**Repair** uses the selected copy's pack. The tooltip says the repaired pack is saved as a new version and this one is kept. The pack file on disk is replaced only when that repair runs; the stored previous version remains.

**Assemble...** asks for a new `.vpk` and writes that file from the selected copy's pack with the selected model. The summary is `Assembled a pack at <path>.` It opens the same **Pack assembled** modal. It still does not install the pack.

**COPIES** lists **Pack**, **Path**, and **Observed**. **Hide** or **Hide this copy** drops that observation from the history. The tooltip says the pack file is not changed. Hiding a copy is not **Remove from library** and it does not delete blobs. When the list has unproven matches, a header named **Unproven matches** shows them.

There is no command that makes an older version the gallery's current source. **View** and **Compare A/B** still open it. **Assemble...** can write a new VPK from a selected copy.

## Get mods

**Get mods** (**Ctrl+8**, or **File > Get mods from GameBanana**) reads Deadlock mods from GameBanana's public API. It needs an internet connection and nothing else; no GameBanana account is used.

- The field at the top takes a GameBanana mod link, such as `https://gamebanana.com/mods/691764`, a `.../mods/download/...` link, a mod manager link that ends in `,Mod,<id>`, or a bare mod id. Anything else is searched as text. An empty field shows every Deadlock mod.
- Without a search, the page browses with a sort (**Most downloaded**, **Most liked**, **Most viewed**, **Newest**, **Recently updated**) and a category. The arrows on the right change pages.
- Mods whose authors set a content rating show **Rated content** instead of their preview. **Show rated previews** shows them.
- A mod already in your library carries an **In library** badge.

Click a mod to see its screenshots, its description and its files, newest first. Each file has **Download & enable** and **Download**. A download:

1. is fetched over HTTPS from gamebanana.com only, and checked against GameBanana's MD5 when the mod lists one;
2. is opened if it is a zip, 7z or rar archive. Only the `.vpk` files inside are written. Readme files, images and nested archives are skipped, and paths inside the archive cannot place a file outside a temporary folder under `<data-root>\downloads`, which is removed afterwards. A plain `.vpk` file is used as it is;
3. is stored in the library like any added mod, named after the GameBanana mod, and added to the collection.

**Download & enable** also enables it in the current profile on the **Mods** page. Neither button changes the game folder. A file that is not a zip, 7z, rar or VPK, and an archive without a mod VPK, report that the mod needs to be installed by hand from its page. Downloads share the status bar's progress and **Cancel** with imports, and wait while other library work runs.

## Mods

**Mods** (**Ctrl+7**) lists every mod in the library with a checkbox. A ticked mod is enabled in the current profile.

- **Load order.** The list is the load order. Drag a mod, use the arrows, or right-click it for **Move to top** and **Move to bottom**. The number beside an enabled mod is its position. When two enabled mods contain the same file, the one higher in the list wins. Such mods show **overrides** or **overridden**; hover it to see which mods share files and how many.
- **Profiles.** **Profile** switches between named sets of enabled mods. **New** starts a profile, optionally with the mods enabled now. **Rename** and **Delete** change the current one. Every profile shares the one load order. The last profile cannot be deleted.
- **Enable all** and **Disable all** change the current profile. The explorer's right-click menu on a source has **Enable for the game** or **Disable for the game** too.
- Right-click a mod for **Show skins in library**, **Open GameBanana page** for downloaded mods, and the path items. **Source** says **GameBanana** or **Local file**.

Nothing reaches the game until **Apply to game**. It needs a connected game and Deadlock closed. It makes `game\citadel\addons` hold exactly the enabled mods, as `pak01_dir.vpk`, `pak02_dir.vpk` and so on in load order: mods already in place stay, moved ones are renamed, new ones are copied, and Modworks files that are no longer enabled are removed. The page then says whether the addons folder matches the enabled mods, or **Changes not applied yet** when it does not.

Modworks records the files it wrote in `addons\.modworks.json`. **It never writes, renames or deletes any other file in `addons`.** Mods installed by hand or by another mod manager are listed under **Other files in addons**, and their numbers are skipped, so they keep loading where they are. A Modworks file that was changed by hand is treated the same way from then on. Deadlock loads at most `pak01` to `pak99`.

Deadlock only reads `addons` when `game\citadel\gameinfo.gi` lists `Game citadel/addons` in its search paths. The page warns when the line is missing, and **Apply to game** then offers **Add the line and apply**, which keeps the original as `gameinfo.gi.modworks.bak`, or **Apply without it**. A game update can replace `gameinfo.gi` again.

Enabled mods, the load order, profiles and download sources are saved in `<data-root>\mods.json`. A damaged `mods.json` is left as it is and the page says so; changes made in that session are not saved over it. **Share library...** does not include it.

The VPK builder works as before. **Apply to game** does not install a pack you build; copy it into `addons` yourself, or add it to the library and enable it like any mod.

These pages were exercised with a disposable data root and a stand-in game folder: browsing, a pasted link, and **Download & enable** followed by **Apply to game** with the mod `haze black recolor`, next to a foreign `pak03_dir.vpk` that stayed untouched. They were not run against a real Deadlock install, so loading in game was not observed.

## Sharing a library

**File > Share library...**, or **Share...** under **LIBRARY**, writes one `.db` file. The tooltip says it saves mods, pack selections, and thumbnails. Nothing is written if the destination already exists. On success the status line says `Saved your whole library to <path>. Others can add it with Add library.` The menu item that adds it is **Add shared library...**, not a menu named Add library. The explorer button is **Add shared...**.

**File > Add shared library...** adds that `.db` file's mods, pack selections, and thumbnails. The tooltip says nothing in your library is replaced. A game path from another computer is used only when that file exists on this computer. The same command also opens a JSON library list from an earlier release and imports the VPKs named there.

The first launch reads older files if they are present, and leaves them in place: `library.db`, `library.json`, `library.backup.json`, and VPKs under `mods`. Those files are not deleted after the merge.

## Where data is stored

The default root is `%LOCALAPPDATA%\DeadlockModManager`. Pass `--data-root <directory>` to use another directory. If the flag is omitted, `DEADLOCK_MODWORKS_DATA_ROOT` is used when it is set. The flag wins. A missing directory after `--data-root` shows **--data-root needs a directory.** and does not open a library. The override is the whole data root, not only the `store` folder.

| Path under the data root | Role |
| --- | --- |
| `layout.ini` | Docking layout. **View > Reset layout** deletes this file only. |
| `window.ini` | Window position and size. Reset layout does not remove it. |
| `cache\` | VPKs rebuilt from the library so the gallery can open them. Safe to delete while Modworks is closed. The next launch rebuilds what the collection still needs. |
| `store\catalog.sqlite` | The catalog: file identities, versions, the collection, pack selections, the connected game archive, and thumbnails. |
| `store\blobs\sha256\` | File bytes, one blob per SHA-256. |
| `mods.json` | Enabled mods, load order, profiles and GameBanana sources for the **Mods** page. |
| `downloads\` | Temporary folders for GameBanana downloads. Each is removed when its import ends. |
| `library.db`, `library.json`, `library.backup.json`, `mods\`, `thumbnails\` | Older releases. Read once if present, then left on disk. |

**File > Open library folder** opens `store`, not the data root. The explorer prints that `store` path under **LIBRARY**.

Do not open the live library from two copies of the app. The second open reports **The model library is in use by another process.**

## Backup and restore

Close Modworks before copying the data root. Copy the whole directory, including `store\catalog.sqlite` and `store\blobs`. Restoring means closing the app and putting that copy back. Do not delete `catalog.sqlite` and expect the blobs alone to reopen as a library.

**Share library...** is a portable copy of the mods, the collection, and thumbnails. **Add shared library...** merges it into the current library and does not replace entries that are already there. It is not a full-disk image of `layout.ini` or `window.ini`.

A damaged `catalog.sqlite` is not replaced. The message is **The model library is damaged and was not replaced. Restore catalog.sqlite from a backup.** Put the backup file back while the app is closed. Launching again must not be used as a way to start an empty catalog over the damaged file.

Failed imports, cancelled imports, and failed exports leave the last committed catalog and any previously published output in place. A failed repair does not publish a half-written VPK.

To undo a repair from the command line, see below. Inside the app, use **Version history** to view the version from before the repair. The `.bak-` file beside the replaced cache copy is the bytes from just before that replacement. Copying it back by hand is only useful if you also know which cache file the gallery is opening. Prefer the history view, or a data-root copy you made before the repair.

## Remove from library is not delete

**Remove from library** takes that source out of the collection. It does not delete the original file you imported, the exported pack, blobs, or version history. The model can still appear under **Version history**.

There is no **Delete stored data** command. To throw away stored bytes, close the app and delete or replace the data root yourself. Deleting only `cache` does not forget the library. Deleting `store` does.

**Hide this copy** hides one history observation. It does not remove the source from **Mod library** and does not delete the pack file.

**Clear pack** and **Remove from pack** change the export selection only.

## Command line

These commands talk to the repair tool or the content store. They do not open the window. `--data-root`, `--game`, and `--clip` do open the window, as does passing only mod files.

Run them from a terminal. The game argument for analyze, update, and repair is the `citadel` folder, not `pak01_dir.vpk`.

```powershell
DeadlockModworks.exe --version
DeadlockModworks.exe --help

DeadlockModworks.exe --analyze "<citadel-folder>" "<mod.vpk>" --report "report.json"
DeadlockModworks.exe --update "<citadel-folder>" "<mod.vpk>" "<new-output.vpk>" --report "report.json"
DeadlockModworks.exe --repair "<citadel-folder>" "<mod.vpk>" --report "repair.json"

DeadlockModworks.exe --store "<store-directory>" import "<file.vpk>"
DeadlockModworks.exe --store "<store-directory>" models
DeadlockModworks.exe --store "<store-directory>" history <64-hex-blob>
DeadlockModworks.exe --store "<store-directory>" duplicates
```

`--store` takes the content-store directory (`store` under the data root, or any other directory you pass). It does not take the data root unless that directory itself is a store.

Each of `--analyze`, `--update`, and `--repair` takes one mod. `--update` writes a new file and will not overwrite. `--repair` replaces the file you named and keeps a `.bak-<timestamp>-<id>` backup beside it. The help text says **Repair replaces the mod with a validated archive and keeps an original backup.** That sentence is the command-line repair, not **Remove from library**.

`--report` is optional, must be last, and must be a `.json` path. **Ctrl+C** cancels. A cancelled repair prints **Cancelled. No pending repair was installed.**

Exit codes: **0** supported analysis or a completed repair, **1** needs review or unsupported content, **2** invalid input, failure, or cancellation.

To undo `--repair`: close Deadlock, move the repaired VPK aside, and copy the `.bak-` file back onto the original filename. If the VPK is a directory VPK, keep its original chunk files. The backup still refers to them. `--update` does not need that undo. Delete or ignore the new file. The input was not replaced.

Opening the window on another directory:

```powershell
DeadlockModworks.exe --data-root "D:\ModworksData"
```

## Startup files

The data root is `--data-root`, then `DEADLOCK_MODWORKS_DATA_ROOT`, then `%LOCALAPPDATA%\DeadlockModManager`. Under it:

| Path | What it is |
| --- | --- |
| `logs\modworks.log` | Errors only. **Help > Open logs** selects this file. A quiet start does not create it. |
| `store\settings.json` | The saved game directory, when one has been written. A corrupt file is left unchanged. |
| `store\catalog.sqlite` | The model library. A read-only catalog is not replaced. |

A corrupt `settings.json` shows **settings.json could not be read. Saved preferences were left unchanged, and the model library was not replaced.** The catalog stays. The window continues after that box. A data root that cannot be written, including a read-only catalog, shows **Deadlock Modworks cannot write its data folder at ... The model library was not replaced.** and exits without replacing the catalog.

`vulkan-1.dll` is delay-loaded. A missing loader is supposed to show **vulkan-1.dll was not found. Install or update the graphics driver that provides the Vulkan loader, then start Deadlock Modworks again. The model library was not opened.** That dialog was not observed. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## What 0.1 does not do

- It does not install, update, or remove mods inside Deadlock by itself. **Pack assembled** says so, and **Install or remove** in the quick start is still a manual copy.
- It does not ship an installer, an auto-updater, code signing, or a mod marketplace. `v0.1.0` is not tagged, and nothing has been published. A clean-machine launch, code signing, and `scripts/Publish.ps1` on this documentation update were not run.
- It does not repair every mod. Archives that need review are left unchanged.
- It does not embed `vulkan-1.dll`. The missing-loader dialog was not observed.
- It does not treat a clicked **Quick start** popup, a finished **Pack assembled** modal, a two-model comparison screenshot, or a drag-and-drop as something this documentation update ran. The labels above are the ones in source. The quick-start sentences were not given an extra step.
