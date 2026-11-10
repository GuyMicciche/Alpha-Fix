# Optional minimal Windows FFmpeg experiment

`buildffmpegwin.bat` builds FFmpeg 8.1 with MSVC for Windows x64 into
`third_party/ffmpeg-win-minimal`. The source/build cache is `Win/ffmpeg-minimal`.
It does not replace `ffmpeg-shared`, change the default Windows build, install
into After Effects, or change release CI.

Prerequisites: Visual Studio 2022 C++ tools, MSYS2, and GNU Make (`pacman -S make`
in an MSYS2 shell). No GCC or NASM installation is needed for this profile.
Set `MSYS2_ROOT` if MSYS2 is outside `C:\msys64`. `GNU_MAKE_DIR` can point to a
portable MSYS2 GNU Make executable directory instead. Run:

```bat
buildffmpegwin.bat
```

The profile enables ProRes decoding, MOV demuxing, file input, swscale and
swresample. Other codecs, encoders, network input and optional external libraries
are disabled. x86 assembly is disabled to match the initial Mac profile; this
may be slower than the full build. License and source/configuration notices are
saved under the resulting prefix's `licenses` directory.

When you deliberately want to build/package the plugin against this prefix,
use a new Command Prompt in the repository root:

```bat
set "FFMPEG_PATH=%CD%\third_party\ffmpeg-win-minimal"
buildwin.bat --no-install --no-pause
```

That command uses the existing Windows packaging flow and updates its `Win/`,
`Release/` and generated ZIP outputs. It does not install into AE. Closing the
Command Prompt clears the override; the default remains `ffmpeg-shared`.

## Local comparison

The experimental build was compiled successfully and AlphaFix linked against it.
Using AlphaFix's actual decoder, all 180 frames of `Test Project/Black-Car-01.mov`
(3840x2160, 24000/1001 fps), plus five seek/cache requests, produced byte-identical
alpha output against the existing full Windows FFmpeg build. 1,534,464,000 bytes
were compared. SHA-256 of either alpha output:

`a5d288345f187da59134f01453c1809b824079c78ac853cf074383dbdb88a6de`

Single-run decoder timings were 9.913 seconds full versus 10.757 seconds minimal.
These include writing the alpha data to disk and are not a controlled benchmark.
The FFmpeg DLLs depend only on sibling FFmpeg libraries and Windows system DLLs;
no MSYS2 runtime is needed. After Effects UI/render testing remains to be done.
