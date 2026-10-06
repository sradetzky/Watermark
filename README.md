# Watermark

[![Windows build and test](https://github.com/sradetzky/Watermark/actions/workflows/windows.yml/badge.svg)](https://github.com/sradetzky/Watermark/actions/workflows/windows.yml)

Experimental Windows tools that stamp a mark into images and later test whether that mark is still there. The mark is derived from your watermark artwork, with an optional private passphrase. Detection needs the image to inspect and that artwork, without the original photo. The project targets recovery after compression and ordinary photo edits; broader real-photo validation is still pending.

The core library, both command-line tools, and the Win32 window are implemented. Behavior and the robustness target are written down in [PLAN.md](PLAN.md); working context for future sessions is in [AGENTS.md](AGENTS.md).

## Tools

| Tool | Role |
|---|---|
| `wmembed` | Turn watermark artwork into a payload and embed it in one file or a folder of images. |
| `wmdetect` | Recover that payload from one file or a folder and report whether it matches the watermark. |
| `wmgui` | Embed and detect through a native Windows interface. |

All three programs use the same core library and batch operations. They support single-file and folder runs.

The watermark image is the identity of the mark. The tools embed a short redundant payload built from a robust hash of that artwork. An optional visible silhouette can also appear on the photo; it is accompanied by the detectable payload.

## Robustness target

On photos whose short side is at least about 512 pixels, version 1 aims to keep the mark detectable after:

- JPEG around quality 70 and quality 50
- A second JPEG or WebP save
- Resize in a modest range (about 75% to 150%)
- Brightness, contrast, and gamma shifts
- Light blur and light noise
- Cropping that leaves several intact tiles

Large rotation, print-and-scan, heavy perspective, and generative edits are outside version 1. Exact acceptance checks are in [PLAN.md](PLAN.md).

## Build

Requirements:

- Windows 10 or 11, x64
- Visual Studio 2019 16.10 or newer (or Build Tools), with **Desktop development with C++** and a Windows SDK
- CMake 3.20 or newer and Ninja; the **C++ CMake tools for Windows** component supplies both
- Windows Imaging Component for JPEG, PNG, TIFF, and BMP; WebP decoding depends on the installed WIC codec

Clone the project, then build and run the core, CLI, and GUI test suites from PowerShell:

```powershell
git clone https://github.com/sradetzky/Watermark.git
cd Watermark
.\tools\build.ps1
.\tools\build.ps1 -Configuration Debug
```

The script discovers a supported Visual Studio installation with `vswhere`, loads its x64 developer environment for the current process, and finds CMake and Ninja on PATH or in that installation. Executables are written to `build/Release` or `build/Debug`.

For manual configuration in a developer command prompt:

```bat
cmake -S . -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
ctest --test-dir build/Release --output-on-failure
```

Image I/O and the window stay on Windows APIs that are already installed. Qt and OpenCV are not required.

GitHub Actions builds and tests Debug and Release on Windows. Successful Release jobs provide a `Watermark-windows-x64` archive under the workflow run's artifacts (downloading requires a GitHub login). The executables require the [Microsoft Visual C++ x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe).

## Layout

Current tree:

```text
Watermark/
  CMakeLists.txt
  README.md
  PLAN.md
  src/core/    image load/save, payload, embed, detect
  src/cli/     wmembed and wmdetect
  src/app/     shared batch operations, manifests, reports, file protections
  src/gui/     Win32 window and shell dialogs
  tools/       PowerShell build and test entry point
  tests/       core, CLI, and GUI tests
```

## Usage

Start the graphical interface with:

```powershell
.\build\Release\wmgui.exe
```

The **Embed** page takes your watermark image, the photo/folder to watermark, an output
folder, strength, and PNG/JPEG settings. The **Detect** page takes the same **Watermark
image** and the **Image or folder to inspect**, then shows a result row for each file.
The original unmarked photo is never required. **Watermark identity** is the default
detection key mode: no passphrase or manifest is needed, and no secret field is shown.
`mark.json` remains an alternative to the watermark image. You can also select a masked passphrase
or a UTF-8 key file for private-key mode and existing v0.1.0 marks.
Native file/folder pickers fill the paths; subfolders are optional.
Processing runs in the background, with a log, file progress, and **Cancel**.
Cancellation keeps files already committed. Closing during a run requests cancellation
before releasing the worker. After detection, **Export CSV** saves the completed
results, including partial results from a cancelled batch. The GUI does not write
a report automatically.

### Visible silhouette

On the Embed page, choose a transparent silhouette PNG as the **Watermark image**
and check **Visible watermark**. The same file supplies the visible shape and detection
identity. The alpha channel defines the shape; the tool renders it black or white, preserving
transparent openings and trimming empty borders. Prepare the cutout before using
it: the application does not segment a person from a landscape JPEG automatically.
Opaque photographs and entirely empty cutouts are rejected.

Choose **Bottom right** (default), **Bottom left**, **Top right**, **Top left**, or
**Center**. **Size %** controls the longest edge relative to the photo's short side
(1..50, default 15); **Opacity %** is 1..100 (default 50). **Color** defaults to
black for a subtle thumbprint; white is available for dark photos. Corner marks have a 2%
margin. The original aspect ratio is retained. Uncheck visibility for the original
invisible-only workflow.

The visible stamp is applied before embedding the detectable payload. Detection
uses the same watermark PNG; `mark.json` is optional. It needs no original photo,
position setting, or opacity setting. It can recover a payload even if a crop removes the visible corner,
provided enough marked image remains. A silhouette pasted by another application
without the payload does not establish a detected mark. PSNR logged in visible
mode measures only the keyed change against the already stamped image.

### Command line and detection keys

Default usage needs no secret:

```powershell
.\build\Release\wmembed.exe --watermark silhouette.png --visible --visible-position bottom-right --in photo.jpg --out stamped
.\build\Release\wmdetect.exe --watermark silhouette.png --in stamped/photo.jpg.png
```

Omit `--visible` for invisible-only output. Optional `--visible-size` and
`--visible-opacity` use the same percentages as the GUI. `--visible-color black|white`
selects the ink. Use `--in stamped --report report.csv` for batch detection.
The legacy `--source` spelling remains an alias for `--watermark`.
Advanced CLI usage can still choose different artwork with `--visible-image` instead
of `--visible`; detection then requires the identity artwork or its manifest.

Source-key mode composites source transparency onto white before computing its
64-bit perceptual identity, then uses `Watermark/source-key/v1/` followed by the
16 lowercase hexadecimal hash digits as the input to the existing keyed pattern.
The manifest records `key_mode: source-v1` and lets detection derive the same key.
This is public identification, not private authentication: the watermark or manifest
is sufficient to reproduce a mark, and the perceptual identity can collide.
Keep the same watermark artwork for later detection; the manifest is an optional substitute.

For private-key mode, put the passphrase in a single UTF-8 line in `secret.txt`, then run:

```powershell
.\build\Release\wmembed.exe --watermark logo.png --key-file secret.txt --in photos --out stamped
.\build\Release\wmdetect.exe --mark stamped/mark.json --key-file secret.txt --in stamped --report report.csv
```

Detection also accepts `--watermark logo.png` instead of `--mark`. `mark.json` keeps the original watermark hash and format parameters; it contains no passphrase. Existing v0.1.0 manifests require their original secret. Private-key mode retains the v0.1.0 hash and payload behavior. Source-key mode requires v0.2.0; its optional manifest field is rejected by v0.1.0 readers, while the embedded payload format remains version 1. An output folder cannot change watermark identity or key mode without `--force`. For older marks made with different identity and visible images, retain the original identity artwork or use the saved manifest.

Use `--recursive` for subdirectories. Embed defaults to PNG; add `--format jpeg --quality 70` for JPEG, or `--strength low|default|high` to change strength. Output names append an extension to the complete input name, so `photo.jpg` becomes `photo.jpg.png`. Input images are preserved. Existing outputs require `--force`; an input already carrying the same mark is skipped unless forced.

Folder detection writes `report.csv` in the working directory unless `--report` specifies another path. CSV includes verdict, bit accuracy, confidence, recovered hash, source match, and errors. A recovered mark can be present with `hash_matches=false` if a different source was supplied. An absent result exits successfully. Exit codes: `0` success, `1` processing/read/write failure, `2` invalid arguments or manifest schema. Each program supports `--help`.

Key files accept an optional UTF-8 BOM and one terminal LF/CRLF; spaces are significant. `--passphrase` is also supported for one-off use. The tools do not log or save the secret.

## Verified scope and current limits

The core self-test uses three deterministic synthetic scenes at 512 and 768 pixels.
It checks exact PNG recovery, JPEG 70/50, bilinear resize, brightness shifts, light
Gaussian blur/noise, contrast/gamma, repeated JPEG saves, crops, unmarked negatives,
and wrong keys. Additional tests recover arbitrary pixel crop origins (including
37,23 and 127,99), a crop saved as JPEG 70, and rounded rectangular resizes at
80%, 85%, 95%, 105%, 115%, 130%, and 145%. Crop recovery also runs without an
expected source and with a mismatching expected source. Default PSNR remains
about 43.9 dB; the embedding format is unchanged.

The CLI suite starts both real executables and checks files, manifests, CSV,
traversal, overwrite protection, and failures. The GUI suite operates its own
window, embeds and detects, checks cancellation/close behavior, verifies shared
CSV export. It does not automate the interactive shell picker dialogs. CTest runs
the GUI checks without screen capture so they also work on CI. For local layout
inspection, run `.\build\Release\watermark_gui_test.exe --capture`;
it renders both pages and saves `wmgui-embed.png` and
`wmgui-detect.png` in the working directory.

The supplied 1066x1600 concert photo has also been checked locally with a visible
bottom-right black photographer silhouette and a source-derived key. PNG and JPEG 70
recover all bits with a present verdict; JPEG 50 recovers all bits with a weak
verdict at default strength and present at high strength. The original photo and
a wrong source remain absent. Default keyed PSNR is about 45.56 dB against the
visibly stamped photo. This single photo does not establish population robustness;
the representative photo corpus and false-positive calibration remain open.

Scale search covers
5% increments from 75% to 150%, with neighboring dimensions checked around strong
magic correlations to undo rounding. Crop synchronization searches all pixel/block
phases at the observed scale, ranking the fixed magic before full-image CRC
verification. CSV appends `offset_x` and `offset_y` for the detected tile phase.
Arbitrary continuous resize factors, combined resize-plus-crop synchronization,
rotation, and very small surviving regions remain outside the verified scope.
Very smooth images can lose their mark under JPEG. Confidence is a correlation
margin rather than a probability. The perceptual source hash can collide and is
not proof of file identity.

WIC uses the first frame of an image. PNG preserves alpha; JPEG composites on white. Metadata and EXIF orientation are not preserved or normalized. WebP decode availability has not been verified on this machine. Only PNG and JPEG output are implemented.

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) for verification and image-sharing guidance.
Keep local photo corpora in `images/`, `photos/`, or `test-output/`; these folders, build output,
`secret.txt`, `key.txt`, and generated reports are ignored by Git. Other key files
must also stay out of commits.

Released under the [MIT License](LICENSE).
