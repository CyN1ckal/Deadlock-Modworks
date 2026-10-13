---
name: Bug report
about: A problem in Deadlock Modworks 0.1
title: ""
---

## App version

From **Help > About**, or from `DeadlockModworks.exe --version`. Include the **Build** commit, not only `0.1.0`.

## What you did

Steps, and whether you used the window or the command line. If you set `--data-root` or `DEADLOCK_MODWORKS_DATA_ROOT`, say that you did. The path is optional.

## Expected

What you expected to happen.

## Actual

What happened instead. Paste the dialog text, the status line (**Copy message**), or the command output. A repair JSON report from **Save report...** is useful when the report is small.

## Diagnostics

- Windows version.
- GPU and whether the failure was a missing `vulkan-1.dll` or a Vulkan dialog.
- Was Deadlock connected?

Do not attach your library. Do not zip `%LOCALAPPDATA%\DeadlockModManager`, `catalog.sqlite`, `store\blobs`, or your mod VPKs. There is no log folder to upload. A single file is welcome only if you want that file shared.
