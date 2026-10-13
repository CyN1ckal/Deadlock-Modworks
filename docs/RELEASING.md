# Releasing

This is how a maintainer builds `DeadlockModworks-0.1.0-win-x64.zip` and what the unpublished tag workflow does. `v0.1.0` is not tagged. Nothing has been pushed. Do not tag, push, or run `gh release create` as part of editing these docs.

`scripts/Publish.ps1` must not be pointed at an arbitrary directory. It only accepts paths inside this repository, and it stages under `build\package-staging`.

## What you need

The [build tools](BUILDING.md): Visual Studio 2026 (the `windows-release` preset) or VS 2022, CMake 3.24 or newer, and the Vulkan SDK. Python 3 with `networkx` is required for `scripts/architecture_diagram.py --check`.

Run PowerShell from the repository root. If the execution policy refuses the script, start that process with `-ExecutionPolicy Bypass`. That is a local policy, not a switch inside the script.

## Configure, test, then package

Configure and build before trusting a package. `Publish.ps1` builds Release itself, but it does not run the tests.

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/release -C Release --output-on-failure
.\build\release\bin\Release\skeleton_repair_tests.exe --gtest_brief=1
.\build\release\bin\Release\render_tests.exe --gtest_brief=1
py -3 scripts/architecture_diagram.py --check
```

For Visual Studio 2022, configure `build\release-vs2022` as in [BUILDING.md](BUILDING.md) and pass that directory as `-BuildDirectory`. The script will not configure a non-default build directory that has no `CMakeCache.txt`, and it will not delete a non-empty `build\release` that has no cache.

Then:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Publish.ps1
```

Defaults, all inside the repository:

| Argument | Default | Constraint |
| --- | --- | --- |
| `-BuildDirectory` | `build\release` | Must stay under `build\`. |
| `-Configuration` | `Release` | Any other value is rejected. The build command is always Release. |
| `-PackagingRoot` | `build\package-staging` | Must stay under `build\`. |
| `-OutputDirectory` | `dist` | Must already be empty of unrelated files, or not exist yet. |
| `-OutputZip` | `dist\DeadlockModworks-0.1.0-win-x64.zip` | The file name must be that zip name. |

The script refuses a filesystem root, the repository root, a relative path that resolves outside the repository, a path that overlaps the build tree or the sources, a missing caller directory, and a caller directory that already contains something it did not put there. It does not recursively delete those paths.

What it deletes is limited to the previous `DeadlockModworks-0.1.0-win-x64.zip`, the matching `.sha256` file, a `.partial` zip, and staging directories under `build\package-staging` that it marked with a `.owner` file. It does not delete `build\release`.

Staging is `build\package-staging\<timestamp>-<pid>`. The script writes the zip only after that stage exists. The checksum file is `dist\DeadlockModworks-0.1.0-win-x64.zip.sha256`.

The zip contains `DeadlockModworks.exe`, `README.md`, `THIRD_PARTY_NOTICES.md`, `PACKAGE_MANIFEST.txt`, the third-party files in `licenses\`, and `LICENSE` when the root license file exists. Shaders are embedded. It does not contain `docs\`, tests, PDBs, game assets, or `vulkan-1.dll`. A package built before `LICENSE` was added recorded `ProjectLicense: absent`. A package built after this tree will copy `LICENSE` and record `ProjectLicense: LICENSE`. This documentation change did not run the script, so it did not produce a new zip.

The executable is linked `/MT`. `Publish.ps1` fails if a static build still needs a VC runtime DLL beside the exe, or if some other non-system DLL is missing. `vulkan-1.dll` is recorded as a driver import and is not copied into the zip.

## What the draft workflow does, and that it has not run

`.github/workflows/build.yml` runs on push, pull request, and manual dispatch. It configures with Visual Studio 2026 or 2022, builds Release, runs CTest and both test executables, runs `py -3 scripts/architecture_diagram.py --check`, runs `Publish.ps1`, and uploads the zip and the `.sha256` file. Hosted CI does not launch the Vulkan window. `render_tests` may skip when no Vulkan 1.3 device exists. A skip is not a test failure. There is no separate GPU smoke job.

`.github/workflows/release.yml` runs only when a tag matching `v*` is pushed. It repeats those checks, uploads the same artifact, and runs `gh release create --draft`. The draft notes say to verify the SHA-256 sidecar before publishing, and that hosted CI does not launch the Vulkan window. The workflow does not publish the draft. Nobody has pushed a `v*` tag for this release, and `gh release create` has not been run from a workstation.

Do not treat a green CI run on an older commit, or a zip SHA recorded in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md), as the artifact for a later commit. Build the commit you intend to ship, and record the new checksum.

## Not done for 0.1

- Clean-machine launch of an extracted zip, on a PC without Visual Studio, the Vulkan SDK, or a development `PATH`, was not run.
- The executable is not code-signed. There is no certificate step.
- No installer and no auto-update.
- Tagging `v0.1.0` and opening the draft are still a separate publication decision. Do them only after the checklist gates that are still **NOT RUN** are either passed or explicitly accepted.
