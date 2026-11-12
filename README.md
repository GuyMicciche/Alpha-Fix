# Alpha Fix — After Effects ProRes 4444 Alpha Repair Plugin

![GitHub Release](https://img.shields.io/github/v/release/GuyMicciche/Alpha-Fix?style=for-the-badge)

![Black-Car-01](https://github.com/user-attachments/assets/54c993b8-560a-4f39-ac43-ea99a82401c2)

## The Problem

When encoding ProRes 4444 with `-alpha_bits 8` via FFmpeg, or just random encounters, Adobe After Effects (and Premiere Pro) incorrectly interprets the 8-bit alpha channel, producing black fringing and edge artifacts on transparent areas. Other applications (Nuke, DaVinci Resolve, etc.) import the same files correctly.

This is an Adobe-side decoder bug. The "Interpret Footage > Alpha" settings do not fix it.

## The Solution

Alpha Fix is a SmartFX effect plugin that:

1. Reads the original `.mov` file directly using FFmpeg's libraries
2. Decodes the correct alpha channel per-frame
3. Replaces AE's broken alpha with the correct one
4. Handles un-premultiplication and re-premultiplication to fix RGB edge colors

## Usage

1. Import your ProRes 4444 (`-alpha_bits 8`) footage into After Effects as usual
2. Apply **Effect > Channel > Alpha Fix** to the layer
3. The plugin automatically detects the layer's source file and repairs the alpha

### Parameters

| Parameter | Description |
|-----------|-------------|
| **Enable** | Toggle the fix on/off for A/B comparison |
| **Alpha Only** | Debug view — displays the corrected alpha as grayscale |
| **Alpha Mode** | `Replace` (default), `Multiply`, or `Maximum` |
| **Frame Offset** | Adjust sync if frames don't align (rare edge case) |
| **Debug Log** | Writes per-frame decode info to a log file for troubleshooting |

### Alpha Modes

- **Replace** — Completely replaces AE's broken alpha with FFmpeg's correctly decoded alpha. This is the right choice in almost every situation.
- **Multiply** — Multiplies both alpha channels together. Useful if you've applied masks or effects that modify the alpha and want to preserve those on top of the fix.
- **Maximum** — Takes the greater of both alpha values per pixel. A niche option for specific compositing scenarios.

---

## Building

### Prerequisites

Before configuring the build, you need to extract two dependency archives into the project root:

**After Effects SDK**

Download the AE SDK from [Adobe Developer](https://developer.adobe.com/after-effects/) and place the archive at:

```
AfterEffectsSDK\AfterEffectsSDK.zip
```

Extract it in place so the result looks like:

```
AfterEffectsSDK\
  Headers\
  Resources\
  Util\
  ...
```

**FFmpeg Shared Build (Windows)**

Download `ffmpeg-8.1-full_build-shared.7z` from [gyan.dev FFmpeg builds](https://www.gyan.dev/ffmpeg/builds/) and place the archive at:

```
ffmpeg-shared\ffmpeg-8.1-full_build-shared.7z
```

Extract it in place so the result looks like:

```
ffmpeg-shared\
  bin\
  include\
  lib\
```

You need at minimum: `avformat`, `avcodec`, `avutil`, `swscale`.

**Other requirements:**

- CMake 3.20+
- Visual Studio 2022 (Windows) or Xcode (macOS)

---

### Windows Build

`buildwin.bat` installs into the newest detected stable AE version under
`C:/Program Files/Adobe` by default. Run as Administrator for installation.
Set `AE_BASE_DIR` to choose another AE application directory. Build failures stop
the script before install. Use `buildwin.bat --no-install --no-pause` for CI or
packaging only. The staged folder is `Release-Alpha-Fix/Support Files/Plug-ins/Alpha Fix`.
The script discovers any Visual Studio 2022 edition with the C++ tools installed.


Once both dependencies are extracted, run `buildwin.bat` **as Administrator** from the project root, or build manually using `x64 Native Tools Command Prompt for VS 2022` **Run as Administrator**:

```bat
set AE_SDK=AfterEffectsSDK
set FFMPEG=ffmpeg-shared

cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
    -DAE_SDK_PATH="%AE_SDK%" ^
    -DFFMPEG_PATH="%FFMPEG%"

cmake --build build --config Release

cmake --install build --config Release
```

### macOS Build

Install full Xcode (including its command-line tools), CMake, and Python 3.
Select Xcode with `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`.
Then run:

```bash
bash buildmac.sh
```

The script extracts the tracked AE SDK archive if necessary, builds FFmpeg 8.1
on the first run, bundles its dylibs, signs the bundle ad hoc, and installs into
the newest stable `/Applications/Adobe After Effects */Plug-ins/Alpha Fix` folder.
It asks for sudo when installing. Close AE first and restart it after installation.
The default architecture is the current Mac's architecture. macOS 12+ is targeted;
the installed After Effects version may require a newer OS.

```bash
bash buildmac.sh --no-install           # build only
bash buildmac.sh --arch arm64           # Apple Silicon
bash buildmac.sh --arch x86_64          # Intel
AE_BASE_DIR="/Applications/Adobe After Effects 2026" bash buildmac.sh
```

Output: `Mac/<architecture>-Release/Alpha Fix.plugin`. FFmpeg is inside
`Contents/Frameworks`; Homebrew is not required at runtime. Only ProRes decoding,
MOV demuxing, file input, and required conversion libraries are enabled in this
Mac FFmpeg build. It is intentionally smaller than the Windows full FFmpeg build.
Use a separate architecture build rather than mixing libraries of different architectures.

Overrides: `AE_SDK_PATH`, `FFMPEG_PATH`, `AE_BASE_DIR`, `ALPHAFIX_VERSION`,
`MACOSX_DEPLOYMENT_TARGET`, and `CODESIGN_IDENTITY`. An external FFmpeg prefix
must contain shared libraries with no dependencies beyond macOS and the sibling
FFmpeg libraries. The bundler rejects unresolved external dependencies.

Mac releases are ad-hoc signed, not Developer ID signed or notarized. Downloaded
builds may be blocked by macOS quarantine; for a trusted download, remove quarantine
from the extracted bundle before installation:

```bash
xattr -dr com.apple.quarantine "/path/to/Alpha Fix.plugin"
```

### Automatic GitHub releases

Commit and push the source changes, including `.github/workflows/release.yml`.
The existing tracked archives `AfterEffectsSDK/AfterEffectsSDK.zip` and
`ffmpeg-shared/ffmpeg-8.1-full_build-shared.7z` must remain available to checkout.
GitHub Actions must be enabled with permission for workflows to create releases.
No custom tokens or Mac hardware are needed.

```bash
git tag v1.0.3
git push origin v1.0.3
```

Use an unused `vMAJOR.MINOR.PATCH` tag. CI builds Windows x64, Mac Apple Silicon,
and Mac Intel on GitHub-hosted runners. Only after all three succeed does it
publish a release with three ZIP files and `SHA256SUMS.txt`. Failed builds do not
publish a partial release. Re-running a tag workflow replaces that tag's assets.
Mac FFmpeg builds are cached separately per architecture. A manual Actions run
on a branch builds downloadable workflow artifacts without publishing a release.

To install a release, close AE, extract the ZIP, then copy Windows's `Alpha Fix`
folder from `Support Files/Plug-ins/` or Mac's `Alpha Fix.plugin` bundle into AE's
own `Plug-ins` folder. Choose the Mac ZIP matching the architecture AE runs under
(Intel for an AE process running under Rosetta). FFmpeg is included in each package.

The release tag sets CMake/package and Mac bundle versions. The existing AE effect
version comes from `src/AlphaFixVersion.h`; tag and product version must agree
when changing the effect's own version metadata.

### PiPL Resource Note (Windows)

The PiPL resource (`AlphaFix_PiPL.r`) is compiled automatically by the CMake build using `PiPLtool.exe` from the AE SDK. In some cases PiPLtool may omit commas in the generated `.rc` file — if After Effects shows a PiPL error after installing the plugin, open `build/AlphaFix_PiPL.rc` and add a comma after `"4L"` on lines containing `eVER`, `eINF`, and `aeFL` properties, then rebuild.

---

## Architecture

```
Alpha Fix/
├── CMakeLists.txt          # Build configuration
├── buildwin.bat            # Windows convenience build script
├── README.md               # This file
└── src/
    ├── AlphaFix.h          # Plugin header, parameter definitions
    ├── AlphaFix.cpp        # Main plugin entry point and SmartFX render
    ├── AlphaFix_PiPL.r     # PiPL resource for AE plugin registration
    ├── AlphaFix.def        # DLL export definitions (Windows)
    ├── AlphaFixDecoder.h   # FFmpeg decoder interface
    └── AlphaFixDecoder.cpp # FFmpeg decoder implementation
```

### How It Works

1. **On apply** — The plugin initializes and reads the layer's source file path via AEGP suites.
2. **On render:**
   - Opens the source `.mov` with FFmpeg's `avformat`
   - Seeks to the current frame using PTS-based rational math (frame-accurate for all-intra ProRes)
   - Decodes the frame with `avcodec`
   - Extracts the alpha channel from the decoded frame, handling `yuva444p10le`, `yuva444p`, `rgba`, and other pixel formats
   - Caches decoded alpha frames in an 8-frame LRU cache to avoid redundant decoding during AE's multi-pass rendering
   - Replaces AE's mangled alpha with the correct one
   - Un-premultiplies RGB using the old (broken) alpha and re-premultiplies using the corrected alpha

### Thread Safety

- Each effect instance maintains its own FFmpeg decoder context
- Decode and cache operations are mutex-protected
- `PF_OutFlag2_SUPPORTS_THREADED_RENDERING` is enabled

---

## Known Limitations

- Requires the original `.mov` file to remain accessible at its original path. If the file is moved or renamed, the plugin falls back to passthrough.
- Frame-accurate seeking depends on the source file having proper keyframe structure. ProRes is all-intra, so this is not an issue in practice.
- The first render after applying the effect may be slightly slower while the decoder initializes.

---

## License

MIT

## Author

Guy Micciche

## Updating to 1.0.6

The effect is named **Alpha Fix** in Effect > Channel and Effect Controls.
Remove the previous `AlphaFix.aex` / `AlphaFix.plugin` installation before installing
`Alpha Fix.aex` / `Alpha Fix.plugin`, so AE loads one copy. Existing projects keep
working through the unchanged internal match name. The About box now includes the
full release version, Guy's Dojo copyright and description in the standard AE format.
The source folder and CMake target remain AlphaFix.
