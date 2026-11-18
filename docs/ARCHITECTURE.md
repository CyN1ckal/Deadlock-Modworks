# Architecture

The diagram is [architecture.svg](architecture.svg). Regenerate it with `python scripts/architecture_diagram.py`, which needs `networkx`.

It is a UML component view of every `#include` under `src/`, arranged in architectural levels by dependency stability. Every arrow points from the module that uses something to the module it uses. All of them point up: a module never depends on anything below it.

- **L0** holds the third-party libraries and the OS. These are the most stable.
- **L1 and L2** are plain records, interfaces and basic utilities: `content_types`, `library_types`, `resource_reader`, `error`, `strings`, `cancellation`, `kv3`, `theme` (Dear ImGui colors only), and the `model_tools` and `snapshot_reader` interfaces. `shell_window`, the Win32 window of the model tools, sits here too because it uses only the OS and Dear ImGui. Many modules use these, and they rarely change.
- **L3 to L7** are mostly the file formats, the store, the repair logic and the library workflows: `vpk`, `resource`, `model_geometry`, `archive_cache`, `latest_task`, `content_store`, `mod_store`, `model_versions`, `version_builder`, `clip_decoder`, `updater` and `library_session`.
- **L8 to L12** are mostly rendering, the UI panels, the workspace that arranges them, and the entry points. They use the most modules and change the most often.

A module sits one level below the lowest module it uses. Entry points that nothing includes (`app/main`, `preview/main`, `standalone`) are pinned to the bottom row.

Each box lists its instability `I = Ce / (Ca + Ce)`, where `Ca` counts the project modules that use it and `Ce` counts the project modules it uses. Each box also lists how many commits have touched its `.hpp`/`.cpp` pair. Arrows are the transitive reduction of the include graph, so an arrow that a longer path already implies is not drawn.

## Interfaces

Realization arrows mark where a stable interface replaces a dependency on volatile code:

- `app/main` calls `RunModelTools` through `model_tools`, and `preview/main` implements it.
- `updater` records repairs through `repair_journal`. `store_reader` reads blobs and snapshots through `snapshot_reader`. `content_store` implements both, so neither module depends on SQLite.

## Targets

- `skeleton_repair_core` (`src/core`) is everything that does not draw. Besides the formats, the store and the repairs, it holds the model-tools workflows that have no UI: `library_session` (the local library, its one background job and saving), `archive_cache` (archives opened once and shared between threads), `latest_task` (a worker whose newest result wins), and the records the panels show (`library_model`, `character_view`, `store_reader`, `history_compare`). All of it is tested by `skeleton_repair_tests` without a GPU, and no source file is compiled into more than one target.
- `sr_render` (`src/render`) draws models with Vulkan.
- `sr_model_tools` (`src/preview`) is the model-tools window. `preview/main` checks the data root and the Vulkan loader, then builds four objects that each own one resource and release it in their destructors, in reverse order: `ShellWindow` (the Win32 window and its saved placement), `VulkanWindow` (the instance, device and swapchain), the Dear ImGui context, and `Workspace`. `Workspace` holds the menus, shortcuts, status bar, explorer and pages. It hands library work to `library_session`, model decoding to `model_inspection`, and the dock layout and help dialogs to `shell_layout`. Each page is its own panel.
- `DeadlockModworks` (`src/app`) dispatches the command line, the repair-only window, and the model tools.

## Records apart from the code that produces them

Several modules hold only plain records, so code that reads the records does not depend on the code that produces them:

| Records | Producer | Readers that no longer depend on the producer |
|---|---|---|
| `content_types` | `content_store` | `history_compare`, `library_model` |
| `library_types` | `model_library` | `character_view`, `thumbnail_cache` |
| `bone_transform` | `animation_clip` | `skeleton_pose` |
| `model_versions` | `version_builder` | `comparison_view`, `multi_view` |
| `animation_clip` | `clip_decoder` | `model_viewport` |

## Rules

`python scripts/architecture_diagram.py --check` enforces three rules. CTest runs it as the `dependency_levels` test when Python 3 with networkx is available.

1. The include graph has no cycles.
2. A CMake target only includes its own modules and those of the targets before it: `core`, then `render`, then `preview`, then `app`.
3. A module does not depend on a less stable one by more than 0.10 instability. Smaller gaps flip whenever a module gains or loses one user, so they are allowed. A larger gap that is deliberate goes in `ACCEPTED_SDP` with its reason. Today the only entry is `animation_player -> clip_decoder`, because the player is where clips are loaded.
