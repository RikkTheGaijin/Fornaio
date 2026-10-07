# Third-party components

The application is built from its supplied C++ source with the following components. License texts accompany the portable and source packages in `licenses/`. Qt is dynamically linked; replacement of compatible Qt DLLs and debugging/relinking modifications are permitted. No additional restriction on reverse engineering for debugging modifications to LGPL libraries is imposed.

| Component | Version / source revision | License / source |
|---|---|---|
| Qt base + image formats | 6.8.3, unmodified official MSVC2022 binaries | LGPL-3.0; third-party notices and exact SBOMs supplied; [Qt base source](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz), [image formats source](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtimageformats-everywhere-src-6.8.3.tar.xz) |
| ufbx | `26a482ae66871d7de36eb722aa060bce95bce274` | MIT; https://github.com/ufbx/ufbx |
| stb_image (TGA fallback) | `2c980bb59875b0d32144a71867fbdebb2f77cd20` | MIT or public domain; https://github.com/nothings/stb |
| MikkTSpace | `mikktspace.c/.h` (Morten S. Mikkelsen), unmodified | zlib; https://github.com/mmikk/MikkTSpace |
| NVIDIA OptiX headers | 9.1, `f1f6dd803f3159992d248178f6e09421c6eb8b6d` | NVIDIA SDK license; https://github.com/NVIDIA/optix-dev |
| NVIDIA CUDA runtime | 12.8, statically linked | NVIDIA CUDA EULA; https://developer.nvidia.com/cuda-toolkit |
| Microsoft Visual C++ runtime | 14.44 | Microsoft redistributable runtime, supplied app-local from Visual Studio's Redist directory |

Qt's bundled third-party code includes image codecs, compression libraries, text/font shaping, and platform integration. The exact component/version/license/copyright mapping is supplied in the two Qt SPDX JSON files. The Qt source archives, when retrieved by the packaging helper, accompany the release under `dist/qt-sources/`; they contain the unmodified corresponding source and its original license notices. Source rebuild instructions are included in those archives. The application's CMake project supports linking against a replacement compatible Qt build.

Generated sample geometry and textures were created specifically for this application and do not contain third-party art.
