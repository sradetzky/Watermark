# Plan

Planning record for the Watermark tools. The product is two C++ programs on Windows, plus one shared library and one simple window.

## Implementation checkpoint — 2026-10-05

The core library, consoles, and Win32 interface are implemented and tested. Build with
`tools/build.ps1`; see `README.md` for current commands and `AGENTS.md` for code
ownership, algorithm conventions, and future-session context.

- Core: WIC RGBA I/O, 32x32 source DCT hash, 96-bit framed payload, BCrypt
  counter stream, 128x128 tiled spread pattern, texture-scaled DCT embedding,
  blind CRC-validated recovery, 5% candidate scales from 75% to 150%, dimension
  rounding refinement, and arbitrary crop phase synchronization at observed scale.
- Initial gate: deterministic synthetic scenes at 512/768 pixels pass PNG,
  JPEG 70/50, resize 75%/150%, brightness, light blur/noise, contrast/gamma,
  repeated JPEG, tile-aligned crop, negatives and wrong keys. Default PSNR is
  approximately 43.9 dB. This does not yet validate a real-photo population.
- Consoles: source or manifest identity, UTF-8 key files, one-level/recursive
  folders, skip-if-present, explicit force, PNG/JPEG output, versioned manifest,
  CSV reports, per-file errors, and protected/atomic output commits. Actual
  executable integration tests run through CTest.
- GUI: Embed/Detect tabs, native shell pickers, masked passphrase or UTF-8 key file,
  background shared jobs, per-file progress/log, cancellation, result list, and
  explicit CSV export. DPI-aware layout and an embedded common-controls manifest.
  Native-window tests cover embed/detect and cancellation/close, with rendered
  captures of both pages. Interactive shell dialogs still need manual checking.
- Broader synthetic checks: blind arbitrary crops, cropped JPEG 70, wrong-key and
  wrong-source crop cases, and seven additional rounded rectangular resize factors.
  Existing watermark format and default PSNR are preserved.
- Public distribution: MIT license, contribution guidance, automatic Visual Studio
  discovery, and Windows Debug/Release CI with a Release archive. Private keys,
  local photo corpora, generated reports, and build output stay out of Git.

Next work:

1. Add a representative photo corpus; measure detection and false positives,
   especially smooth scenes and combined edits. Keep the acceptance bar below.
2. Explore continuous scale estimation and combined crop/resize synchronization
   after measuring the supplied corpus. Current scale checks use 5% increments;
   arbitrary crop phases are searched at the observed scale.
3. Decide and test EXIF orientation/color/metadata handling; verify installed
   WebP decoding. Current WIC path uses the first frame and strips metadata.
4. Manually exercise shell dialogs, mixed-DPI displays, and long-running real
   photo batches. Calibrate confidence and false positives on the supplied corpus.

The sections below remain the intended product contract. The original sequence
uses the self-test as the gate; the implemented synthetic gate allowed console
work, while full real-photo validation remains open before declaring version 1 done.

## Goal

Stamp a watermark derived from a source image into host photos so a second tool can still recognize it after compression and ordinary manipulation, with measured accuracy.

The source image defines the mark. A full-resolution copy of that image will not survive JPEG, so version 1 embeds a short redundant payload. Detection reports present, weak, or absent, plus bit accuracy and a confidence score.

## Programs

Three build products, two user-facing tools:

| Product | Kind | Job |
|---|---|---|
| `watermark_core` | Static library | Load and save images, build the payload, embed, detect. |
| `wmembed` | Console | Embed into one file or a folder. |
| `wmdetect` | Console | Test one file or a folder. |
| `wmgui` | Win32 window | Both jobs, same library calls as the consoles. |

`wmembed` and `wmdetect` are the tools. The window is a front end so single-file and batch work share one code path.

### Embed

Inputs:

- Source image (the mark identity)
- Passphrase
- Strength (low, default, high)
- One image or a folder
- Output folder
- Output format: PNG by default, or JPEG at a chosen quality

Outputs:

- Watermarked images
- `mark.json` beside the run, storing the payload id (the 64-bit source hash and the parameters needed to repeat detection)

If an input already carries this mark, skip it unless the user forces a rewrite. Embedding twice weakens both copies.

### Detect

Inputs:

- The same source image, or the payload id from `mark.json`
- The same passphrase
- One image or a folder

Outputs, per file:

- Verdict: present, weak, or absent
- Bit accuracy of the recovered payload
- Confidence score
- Whether the recovered hash matches the source

Batch mode writes `report.csv`. The process exit code is non-zero when any file fails to read or write. A clean "absent" verdict is a successful run.

### Command line

```text
wmembed --source logo.png --key-file secret.txt --in photos --out stamped
wmdetect --source logo.png --key-file secret.txt --in stamped --report report.csv
```

`--key-file` is a text file holding the passphrase so the secret stays out of shell history. A `--passphrase` flag can exist for local one-off use.

Folder mode processes one directory level by default. A flag turns on recursion. Unsupported files are skipped and listed in the log.

## Payload

The passphrase is mixed into the embedding pattern. A wrong passphrase decodes as absent. The source file is treated as a fixed key image: editing it changes the hash, and hash comparison then fails even if the old mark is still in the host. `mark.json` keeps the original hash for that case.

Bits:

- 64-bit robust hash of the source
- 16-bit magic
- 16-bit CRC

The hash:

1. Convert the source to grayscale.
2. Normalize size.
3. Take the signs of selected low-frequency DCT coefficients.
4. Pack those signs into 64 bits.

Each payload bit is repeated and spread with a sequence derived from the passphrase. The stream comes from SHA-256 in Windows BCrypt (a counter keyed by the passphrase). No third-party crypto library.

The CRC rejects random images and wrong keys. The correlation score has to clear a threshold before a file is called present, so unmarked photos stay negative.

## Embedding and detection

Work in luminance only, so JPEG chroma subsampling does not remove the mark.

Embed:

1. Split the host luminance into tiles of about 128×128.
2. Run an 8×8 DCT in each tile.
3. Change mid-frequency coefficients, skipping DC and the highest AC terms.
4. Add a scaled, passphrase-derived step for each repeated payload bit.
5. Scale that step by local texture, so flat areas move less than detailed areas.
6. Inverse DCT, clamp, and save.

The same payload is written in every tile. A crop that keeps several tiles can still be identified. Default strength targets roughly 40 dB PSNR or better against the host. The GUI strength control scales that step.

Detect, without the original host:

1. Search a short list of scales (about 75% to 150%).
2. Correlate the same mid-frequency coefficients with the passphrase sequence.
3. Majority-vote repeated bits.
4. Check magic and CRC.
5. Compare the recovered hash to the source hash, when the source or `mark.json` is supplied.

Confidence combines the correlation strength and how cleanly the repetition agrees. Weak means the CRC matches with a low margin, or the correlation is only just over the threshold.

Minimum size for the robustness target is a short side of about 512 pixels. Smaller hosts can still be marked, with less averaging across tiles and a wider error.

## What version 1 is measured against

The self-test embeds a mark in a few images at 512 pixels and up, then checks:

| Case | Expected result |
|---|---|
| Unmarked images | Absent |
| Watermarked PNG, no further edit | Present, bit accuracy 1.0 |
| JPEG around quality 70 and quality 50 | Present, payload decodes |
| Resize to about 75% and about 150% | Present, using the detector's scale search |
| Brightness shift of about ±15% | Present |
| Small blur | Present |
| Wrong passphrase | Absent |
| Default strength | PSNR at or above about 40 dB |

Those checks are the acceptance bar. The self-test ships in `tests/` and runs before the GUI is treated as done.

Outside version 1:

- Large rotation
- Print-and-scan
- Heavy perspective change
- Generative edits and inpainting
- A viewable thumbnail recovered from the mark

A later logo mode can embed a tiny binary thumbnail of the source so detection can show a coarse picture. A wider rotation search can follow after the tiled DCT path is solid.

## Window

One Win32 window, two pages.

**Embed.** Source path, passphrase, strength, single file or folder, output folder, PNG or JPEG, progress, log.

**Detect.** Source path or `mark.json`, passphrase, single file or folder, a result list (verdict, bit accuracy, confidence), and export of `report.csv`.

File picking uses the shell file dialogs (`IFileOpenDialog`). The window calls the same library entry points as the consoles.

## Library surface

```text
payload_from_source(image, params) -> payload
embed(host, payload, params) -> image
detect(image, expected_payload_or_none, params) -> result
```

`result` carries the verdict, confidence, bit accuracy, recovered hash, and whether that hash matches the expected source.

## Repository layout

```text
Watermark/
  CMakeLists.txt
  README.md
  PLAN.md
  src/core/    WIC load/save, payload, embed, detect, BCrypt keystream
  src/cli/     wmembed and wmdetect
  src/gui/     Win32 window
  tests/       self-test
```

Suggested core files:

- `image_wic.cpp` — decode and encode through Windows Imaging Component
- `payload.cpp` — source hash, magic, CRC
- `embed.cpp` — tiled DCT embed
- `detect.cpp` — scale search, correlation, majority vote
- `crypto_win.cpp` — passphrase keystream via BCrypt

## Build environment

Original reference setup on this computer (Windows build 26200, 25H2).
The current build script discovers supported Visual Studio installations;
portable prerequisites and commands are in `README.md`.

Already installed:

| Piece | Location |
|---|---|
| Visual Studio Build Tools 2019, MSVC 14.29 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools` |
| `cl.exe` | `VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64\cl.exe` under that tree |
| Windows SDK 10.0.19041.0 | `C:\Program Files (x86)\Windows Kits\10` |
| CMake | `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` under the Build Tools tree |
| Ninja | `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` under the Build Tools tree |
| Developer environment | `VC\Auxiliary\Build\vcvars64.bat` |

`cl`, `cmake`, and `ninja` are not on the normal `PATH`. Load `vcvars64.bat`, then point CMake at the bundled executable, or call that executable by full path.

Generator: Ninja, x64, C++20.

Image decode and encode go through WIC (`windowscodecs`). The GUI goes through Win32, which is in the SDK above. Qt, OpenCV, vcpkg, and a separate CMake install are not part of this plan.

Git and Python 3.13 are on the machine. The C++ build does not use Python.

## Implementation order

1. Core library: WIC images, payload, embed, detect.
2. Self-test covering the table above. Tune the coefficient set, repetition count, and default strength until that table passes.
3. `wmembed` and `wmdetect`, including folder mode, skip-if-present, and CSV.
4. Win32 window with the two pages.

Step 2 is the gate. The consoles and the window wait on a passing self-test.

## Defaults to tune in the self-test

Starting points, not frozen constants:

- Tile about 128×128
- 8×8 DCT
- Mid-frequency AC coefficients only
- Payload repeated inside each tile and across tiles
- Default step size chosen so host PSNR stays at or above about 40 dB while JPEG quality 50 still decodes

The self-test is allowed to move these numbers. The robustness table is the contract.
