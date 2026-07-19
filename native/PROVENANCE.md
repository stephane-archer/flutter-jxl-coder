# Native dependency provenance

The codec artifacts in `jxl_coder` are release inputs, not downloaded at
application build or runtime. Static archives are not code-signed. Their
sources and hashes are verified here; the application publisher signs the
final Apple bundle or Windows package.

## Sources

| Component | Exact source | Download SHA-256 | License |
|---|---|---|---|
| libjxl | tag `v0.12.0` | source archive `03e9be69a30be4011f559da75328b6d7cea8ad921fabfbd551ce10bf45cdc992` | BSD-3-Clause (`windows/third_party/licenses/LICENSE.libjxl`) |
| Highway | libjxl pin `457c891775a7397bdb0376bb1031e6e027af1c48` (`v1.2.0`) | included through the verified libjxl source | Apache-2.0 (`windows/third_party/licenses/LICENSE.highway`) |
| Brotli | libjxl pin `028fb5a23661f123017c060daa546b55cf4bde29` (`v1.2.0`) | included through the verified libjxl source | MIT (`windows/third_party/licenses/LICENSE.brotli`) |
| skcms | libjxl pin `96d9171c94b937a1b5f0293de7309ac16311b722` | included through the verified libjxl source | BSD-3-Clause (in the libjxl source archive) |

The Windows x64 libjxl inputs come from the official
`jxl-x64-windows-static-v0.12.0.7z` release artifact, SHA-256
`ff147dc7ac4ce55392974ccc70f2a8a8ec0eff3ae28529b072258b66c8f01ab2`.

## Windows build configuration

The official libjxl release workflow builds the selected artifact as Release,
triplet `x64-windows-static`, architecture `-A x64`, toolset `-T ClangCL`, with
the following relevant configuration:

```text
JPEGXL_STATIC=ON
BUILD_TESTING=OFF
JPEGXL_ENABLE_OPENEXR=OFF
JPEGXL_ENABLE_PLUGINS=OFF
JPEGXL_ENABLE_TCMALLOC=OFF
JPEGXL_ENABLE_VIEWERS=OFF
VCPKG_TARGET_TRIPLET=x64-windows-static
```

The upstream release also builds tools and devtools, but `jxl_coder` copies
only `jxl`, `jxl_cms`, Highway, and Brotli static archives plus the required
public headers. The plugin supplies its own process-wide parallel runner, so
the separate `jxl_threads` archive and runner-only headers are not packaged.
No tool, example, benchmark, executable, or DLL from the release artifact is
stored or packaged. The COFF directives in the upstream input declare the
static multithreaded CRT (`MT_StaticRelease`); the Windows plugin uses CMake
`MSVC_RUNTIME_LIBRARY=MultiThreaded` to match.

## Apple build configuration

Each architecture is built independently with CMake in Release mode, then
combined only after all builds succeed:

```text
BUILD_SHARED_LIBS=OFF
JPEGXL_ENABLE_BENCHMARK=OFF
JPEGXL_ENABLE_DEVTOOLS=OFF
JPEGXL_ENABLE_EXAMPLES=OFF
JPEGXL_ENABLE_FUZZERS=OFF
JPEGXL_ENABLE_JNI=OFF
JPEGXL_ENABLE_MANPAGES=OFF
JPEGXL_ENABLE_OPENEXR=OFF
JPEGXL_ENABLE_PLUGINS=OFF
JPEGXL_ENABLE_SJPEG=OFF
JPEGXL_ENABLE_TESTS=OFF
JPEGXL_ENABLE_TOOLS=OFF
JPEGXL_ENABLE_VIEWERS=OFF
JPEGXL_ENABLE_BOXES=ON
JPEGXL_ENABLE_SKCMS=ON
JPEGXL_ENABLE_TRANSCODE_JPEG=ON
```

Targets are macOS x86_64/arm64, iOS arm64, and iOS Simulator x86_64/arm64.
The release inputs are repacked with `native/tools/repack_without_libjpeg.sh`.
The final combined archives contain only libjxl, libjxl_cms, Highway, Brotli,
and skcms objects required by the plugin. The separately linked libjpeg-turbo
objects and headers were removed because JPEG reconstruction in libjxl does
not call that pixel codec. The upstream resizable and fixed thread-runner
objects and headers are also removed because the shared plugin scheduler
implements the `JxlParallelRunner` interface directly.

## Shipped artifact hashes

```text
30d2fa76da17d8650bc0217d7f6d0175a50f1d5c9c34fc360ff0d012db0dd596  macOS universal libjxl_coder_native.a
873a792d5e583f9ba263c3dd3fcbfa9af8b443e4c4abc4d55092e894a3e57c5d  iOS arm64 libjxl_coder_native-ios-arm64.a
065405d233345a0fc5a4dd64949e2386e777b0ab98dc4d80f0f4ec0c1a5eb5c6  iOS Simulator universal libjxl_coder_native-simulator.a
640c91ce0dc22e55417ada0c9f82d0e8f6c575fa15cde3e49802bcd57511430e  Windows x64 jxl.lib
f781162a00917fbc82ca24618651e8b40a5c3d9b2ab25c8d50c7e32de5d1bf6d  Windows x64 jxl_cms.lib
a3a5e056c7b5b3203177c62669fd95dfc014e442a73145eae21f8ebf9e077ade  Windows x64 hwy.lib
a963c955e05a5b24f4ec71e2df60ae995599156846b631768a83d93f848a8def  Windows x64 brotlienc.lib
fcf1041906c619a00748d8ebb72d33bff04710e1ab68c4aac79df28dda3ebdf9  Windows x64 brotlidec.lib
0edca6392704cc751f0d8fcc738c736674e8fc4e6c29a84afab58f541022e019  Windows x64 brotlicommon.lib
```

Recompute the table with `shasum -a 256` (Apple) or `Get-FileHash -Algorithm
SHA256` (Windows) before a release. Any change requires updating this document
and `sbom.spdx.json` from the same reviewed build.
