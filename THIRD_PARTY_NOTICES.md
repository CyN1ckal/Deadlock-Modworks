# Third-party notices

Deadlock Modworks does not link ValveResourceFormat. The binary KV3 reader and writer, and the Source 2 resource block layout, follow that project at release 20.0.6980 (`a06886f7d06049052d32a7381ec05523064a2ca0`). The portable package includes the ValveResourceFormat MIT license as attribution for that format work.

## ValveResourceFormat

Source: https://github.com/ValveResourceFormat/ValveResourceFormat

Copyright (c) 2015 ValveResourceFormat Contributors. Licensed under the MIT License:

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Libraries linked into DeadlockModworks.exe

- LZ4 — https://github.com/lz4/lz4 — BSD 2-Clause. Version 1.10.0.
- Zstandard — https://github.com/facebook/zstd — BSD License. Version 1.5.7.
- nlohmann/json — https://github.com/nlohmann/json — MIT License. Version 3.11.3.
- meshoptimizer — https://github.com/zeux/meshoptimizer — MIT License. Version 0.24.
- Dear ImGui — https://github.com/ocornut/imgui — MIT License. Version 1.91.9b-docking.
- Tracy — https://github.com/wolfpld/tracy — BSD 3-Clause License. Version 0.13.1.
- SQLite — https://www.sqlite.org — Public domain. Version 3.54.0.
- libarchive — https://www.libarchive.org — BSD 2-Clause (see its COPYING). Version 3.8.1. Reads downloaded zip, 7z and rar mods.
- zlib — https://zlib.net — zlib License. Version 1.3.1. Used by libarchive for zip.
- liblzma from XZ Utils — https://tukaani.org/xz/ — BSD Zero Clause (0BSD). Version 5.8.1. Used by libarchive for 7z.

The portable package copies each of those license files into `licenses/`. SQLite is in the public domain and has no license file.

## Model tools

The default GUI and `ModelPreview.exe` use the Vulkan loader provided by the graphics driver. The SDK is needed to build them; the SDK and loader are not redistributed. Dear ImGui's and Tracy's licenses are included in the portable package.
