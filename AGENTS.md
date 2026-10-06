# Working on Watermark

## Project and current state

Watermark is a native Windows C++20 project. Read
`README.md` for usage and `PLAN.md` for the product contract and remaining work.
The source image defines a short watermark identity; it is not embedded as a
recoverable picture.
Since v0.2.1, in user-facing text call it **Watermark image**: it is the watermark artwork,
never the original host photo. GUI visible mode uses this same file for its stamp;
default detection needs only that artwork and the inspected image. The CLI prefers
`--watermark` and `--visible`; `--source` and `--visible-image` remain compatible.
Internal `Job::source`, source hashes, and `source-v1` keep their existing semantics.

Implemented:

- `watermark_core`: RGBA image buffers, WIC load/save, source DCT hash, payload
  framing/CRC, BCrypt keyed pattern, tiled luminance DCT embedding, blind detection.
- `wmembed` and `wmdetect`: file/folder operations, recursion, key files,
  `mark.json`, skip-if-present, PNG/JPEG output, CSV reporting.
- `wmgui`: native Embed/Detect tabs, shell file/folder dialogs, masked passphrases
  or key files, source-derived keys, background jobs, progress, cancellation,
  results, and CSV export. Optional visible silhouette with five positions,
  size and opacity controls; detection verifies the accompanying payload.
- CTest core robustness, real-process CLI, and native-window GUI tests.

One supplied concert photo passes source-key visible mode in PNG/JPEG 70; JPEG 50
is CRC-valid but weak at default strength, present at high strength. Broader
real-photo validation is pending. Continuous scale search,
combined resize/crop synchronization, and WebP codec validation remain open.
Passing synthetic tests does not establish the full real-world robustness target
in `PLAN.md`.

The public repository is `https://github.com/sradetzky/Watermark`, licensed under
MIT. Check the current Git status and remotes before committing or publishing.
Use `develop` for development; `main` and release tags hold published code.
Keep local photos in ignored `images/`, `photos/` or `test-output/`; never commit passphrases,
private images, generated reports, or build output. Only add image fixtures with
permission and a documented license.

## Build and verification

Use Visual Studio 2019 16.10 or newer with the C++ workload and Windows SDK.
The build script discovers the installation; CMake and Ninja can be on PATH or
bundled with Visual Studio. The original local setup uses MSVC 14.29.
No Qt, OpenCV, package manager, or Python dependency is needed.

From the repository root in PowerShell:

```powershell
.\tools\build.ps1
.\tools\build.ps1 -Configuration Debug
.\build\Release\watermark_selftest.exe
.\build\Release\watermark_cli_test.exe
.\build\Release\watermark_gui_test.exe
.\build\Release\watermark_gui_test.exe --capture
.\build\Release\wmgui.exe
```

`tools/build.ps1` imports `vcvars64.bat` into its process, configures Ninja, builds,
and runs CTest with failure output. Output directories are `build/Release` and
`build/Debug`. Use the script rather than depending on global PATH changes.
Tests create unique temporary child directories in their working directory and
remove only those directories. The core executable prints per-case confidence,
bit accuracy, detected scale, and PSNR.
The GUI test launches only a process it owns and keeps its window hidden. Optional
`--capture` paints without activation and writes page captures into the working
directory. Default CTest runs omit captures for CI. It checks window jobs and
shared export, not interactive picker dialogs.
Debug's broader synchronization tests are appreciably slower than Release.

`.github/workflows/windows.yml` builds and tests both configurations on Windows
and uploads a Release executable archive. Keep CI actions pinned to verified
commit hashes. Public build instructions must not depend on one local install path.

If a missing binary or CLI tool would significantly simplify the task, stop and
ask the user to install it. Do not install tools automatically.

## Code map

- `src/core/watermark.h`: public library API and result semantics.
- `src/core/image.cpp`: image validation, bilinear resize, RGB PSNR.
- `src/core/image_wic.cpp`: per-call COM/WIC ownership and codecs.
- `src/core/payload.cpp`: source hash and version 1 payload bit layout.
- `src/core/crypto_win.cpp`, `pattern.cpp`: BCrypt SHA-256 counter stream and chip mapping.
- `src/core/internal.h`, `dct.cpp`: shared coefficient positions and orthonormal 8x8 transforms.
- `src/core/embed.cpp`, `detect.cpp`: embedding and recovery using the same pattern.
- `src/core/visible.cpp`: alpha-mask bounds, proportional placement, white
  or black silhouette composition. Apply this before keyed embedding, never after it.
- `src/app/jobs.h`, `jobs.cpp`: shared typed jobs, progress events, summary/CSV,
  per-file errors, cancellation, and output protections.
- `src/app/files.cpp`, `files.h`: key files, UTF-8 conversion, manifests, traversal,
  and atomic file commits.
- `src/cli/common.cpp`: option parsing and console presentation of shared jobs.
- `src/gui/main.cpp`, `controls.h`: controls, DPI-aware layout, message loop,
  worker lifecycle, and presentation of progress/results.
- `src/gui/dialogs.cpp`: shell pickers on the STA UI thread.
- `tests/selftest.cpp`: deterministic algorithm fixtures and WIC round trips.
- `tests/cli_test.cpp`: actual executable invocations and file-level outcomes.
- `tests/gui_test.cpp`: native window operations, worker cancellation/close,
  rendered page captures, and shared export outcomes.

Keep both frontends on `app::run_job` and the core API. Do not duplicate batch/file
rules or the watermark algorithm, and do not invoke CLI programs from the GUI.
GUI controls belong to the UI thread. Worker notifications own their posted data;
closing requests cancellation and waits for completion before destroying the HWND.
Do not block the UI thread with image operations or read controls from the worker.

## Algorithm and compatibility

Version 1 carries 96 bits, MSB first: 64-bit source hash, 16-bit magic `0x574D`,
then CRC-16/CCITT-FALSE over the first 80 bits. The hash uses signs of DCT
coefficients `(u,v)` in `1..8` on a grayscale 32x32 source. It is a perceptual
identity with possible collisions, not a cryptographic digest of the source file.

v0.2.0 defaults in both frontends to a public source-derived key. Compute the hash
with source alpha composited onto white (`payload_from_source(source, true)`),
then use `Watermark/source-key/v1/` + 16 lowercase hex hash digits as the pattern
key. This avoids identical hashes for black transparent silhouettes of different
shapes. Private passphrase mode retains legacy hashing (`false`, the API default).
Source-key manifests have the optional `key_mode: source-v1`; missing means legacy
private mode. Reject unknown key modes and incompatible output manifest changes
unless force is explicit. Old v0.1.0 readers reject the additional manifest field;
the 96-bit wire format and coefficient constants are unchanged. Never treat this
public key as authentication, and never repair CRC bits using the expected hash.

Visible mode accepts a prepared transparent cutout. Alpha defines a black/white shape;
RGB is ignored. Trim transparent bounds, retain aspect ratio, default longest edge
15% of the host short side, black ink at 50% opacity, corner margin 2%. Four corners and center
are supported. The application does not automatically segment complex photos.
The supplied cutout was prepared with imagegen and remains local in `images/`.
Use the source image or manifest to detect; pasted silhouettes alone are not proof.
The >=40 dB gate in visible mode measures keyed embedding against the visible host.

The pattern repeats every 128x128 pixels (16x16 DCT blocks), spreads each bit over
eight mid-frequency coefficients, and derives permutation/signs from exact UTF-8
passphrase bytes through a domain-separated SHA-256 counter stream. Default step
is 6.0 with texture scaling 0.55..1.0; low/high multiply it by 0.7/1.4. Keep these
constants synchronized with the manifest. Changes to bit order, hashing, pattern,
coefficients, or embedding conventions need a format-version decision and tests.

Detection validates magic and CRC before exposing a recovered hash. Verdict means
a valid mark was recovered; `hash_matches` separately says whether it matches the
expected source. Bit accuracy is only defined with an expected payload. Confidence
is a correlation margin, not a calibrated probability. Do not use the expected
hash to fabricate or repair a recovered payload.

Scale candidates cover 5% increments from 75% to 150%; the best magic correlations
also search neighboring rounded dimensions. Crop synchronization uses separable
DCT filters on an upper-left sample capped at 512x512, searches all 64 pixel-grid
phases and 256 block phases, and ranks by the fixed magic. Payload recovery and
CRC validation use the full image, independently of the expected hash. It returns
the valid phase with strongest confidence. Rotation, continuous arbitrary scales,
and combined scale/crop search are not implemented. Broader search is cancellable
between rows/candidates. Very flat areas can lose small coefficient changes under
JPEG. WIC loads the first
frame, preserves alpha in PNG, and composites JPEG on white. Metadata and EXIF
orientation are not preserved or normalized. WebP reading depends on an installed
WIC decoder; WebP output is not implemented.

## CLI behavior to preserve

- With no key flag, use source identity; source-key manifests derive their key.
  Existing private-mode manifests still require the original passphrase/key file.
  In private mode prefer `--key-file`: one UTF-8 line, optional BOM and one terminal CRLF/LF;
  other whitespace is significant. Never log or store the passphrase.
- `mark.json` stores hash and versioned parameters, never the key. Its flat schema
  is validated strictly; detection accepts it with `--mark` instead of `--source`.
- Default traversal is one level. Recursion preserves directories and excludes
  an output folder nested inside the input. Unsupported extensions are logged.
- Append the output extension to the full input name (`photo.jpg.png`) to avoid
  basename collisions. Protect existing outputs unless `--force` is explicit.
- Skip an input already carrying the same keyed mark unless `--force` is supplied.
  Never overwrite an input image, key, source, or manifest with an output/report.
- Exit 0 means a successful operation, including an absent detection; 1 means
  read/write/processing failure; 2 means invalid arguments or manifest schema.
- Batch detection writes UTF-8 CSV, records individual file errors, and continues.
  Reports and images commit through a temporary file in the destination directory.
- CSV retains the original columns and appends detected `offset_x`/`offset_y`.
  GUI report export is explicit; cancelled batches retain completed results/files.

## Working practices

Surface assumptions when ambiguity matters. Define success criteria for nontrivial
work. Prefer the simplest working solution and surgical changes. Match existing
style (four-space indentation, explicit ownership/RAII, exceptions at API error
boundaries). Avoid speculative abstractions, features, and configuration. Remove
only dead code introduced by your own changes.

Run focused verification before finalizing. Algorithm changes must preserve
negative/wrong-key checks, exact payload recovery, and the default >=40 dB PSNR
bar. Do not weaken assertions to make tuning pass. Broaden the photo corpus before
claiming robustness beyond the documented fixtures. CLI changes should exercise
the actual programs, their exit codes, and filesystem outcomes.

Prefer existing development servers if one becomes relevant. An isolated worktree
may use a temporary server on its own local port. Never stop existing user processes.
