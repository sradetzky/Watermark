# Contributing

Watermark is a native Windows C++20 application licensed under MIT. See
[README.md](README.md) for setup and [PLAN.md](PLAN.md) for the intended behavior
and open work. [AGENTS.md](AGENTS.md) maps the code and records compatibility rules.

Build and test before submitting a change:

```powershell
.\tools\build.ps1
.\tools\build.ps1 -Configuration Debug
```

Keep changes focused and explain the behavior they fix, how they were verified,
and any remaining limits. The GUI and consoles must continue to share the core
and batch operations.

For algorithm changes, preserve CRC validation, wrong-key/unmarked negatives,
payload recovery, and the default >=40 dB PSNR bar. Changes to the source hash,
payload framing, keyed pattern, or coefficient conventions require an explicit
format-compatibility decision. Do not repair recovered bits using the expected
source or weaken tests to make tuning pass.

For a detection issue, include image dimensions, edit steps, output format/quality,
verdict, confidence, and bit accuracy. Supply images only when you have permission
to share them. Use a test passphrase rather than a production secret. Private keys,
local photos, and generated outputs belong outside the source tree or in ignored
directories.

Real-photo robustness and false-positive calibration are still open. Passing the
synthetic fixtures is required, but is not evidence for every photo or edit.
