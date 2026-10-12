# Third-party notices

Deadlock Skeleton Repair does not link ValveResourceFormat. The binary KV3 reader and writer, and the Source 2 resource block layout, follow that project at release 20.0.6980 (`a06886f7d06049052d32a7381ec05523064a2ca0`). The portable package includes the ValveResourceFormat MIT license as attribution for that format work.

## ValveResourceFormat

Source: https://github.com/ValveResourceFormat/ValveResourceFormat

Copyright (c) 2015 ValveResourceFormat Contributors. Licensed under the MIT License:

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Libraries linked into DeadlockSkeletonRepair.exe

- LZ4 — https://github.com/lz4/lz4 — BSD 2-Clause. Version 1.10.0.
- Zstandard — https://github.com/facebook/zstd — BSD License. Version 1.5.7.
- nlohmann/json — https://github.com/nlohmann/json — MIT License. Version 3.11.3.
- meshoptimizer — https://github.com/zeux/meshoptimizer — MIT License. Version 0.24.

The portable package copies each of those license files into `licenses/`.

## Optional model preview

`ModelPreview.exe` is built only when the Vulkan SDK and `glslc` are installed. It is not part of the portable repair zip. That target also compiles Dear ImGui (v1.91.9b-docking, MIT) and uses the Vulkan loader from the SDK, which is not redistributed.
