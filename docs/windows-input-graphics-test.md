# Experimental Windows input recovery test

This opt-in bridge is a diagnostic fallback for stalled guest USB input. It is
not a root-cause USB fix and remains disabled in normal builds.

## Graphics correction

The menu's green/rainbow textures are fixed upstream in v0.54.0 by the larger
render-surface pool from #10. The corrected LAHF translation must remain enabled.
The earlier per-function graphics exception has been removed. Regenerate a clean
private C directory if you used the previous version of this patch tool.

## Build the opt-in input test

1. Generate C normally from your own retail NTSC XBE into a separate private directory.
2. Run `python tools/windows_input_compat.py <generated-directory>`.
   This only renames the original input function so it remains available as a fallback.
3. Configure Windows/MinGW with `-DNFSU2_GEN_DIR=<generated-directory>` and
   `-DCLX_EXPERIMENTAL_INPUT_COMPAT=ON`, then build normally.
4. Copy the executable as `clx-graphics-native-test.exe` beside the normal
   distribution and copy `pc/Start-Conker-Graphics-Native-Input-Test.*` there.
   These historical filenames are retained; the test no longer patches graphics.
5. Run `Start-Conker-Graphics-Native-Input-Test.bat`.

The launcher enables `CLX_NATIVE_PAD=1` and the diagnostic virtual-port layout.
The original guest implementation remains the fallback when that variable is
unset or zero. This test preserves the normal executable and settings.
Never commit generated game code or game assets.

## Current status

The earlier player-tested executable was v0.53 + #8/#9, before #10. Its release
is retained as historical test evidence, not as the recommended graphics fix.
New tests must use v0.54.0 or freshly generated current-main code without a LAHF
exception. The USB report lock and environment-pointer fix are split into #12.
The startup-stall root-cause investigation is separate from this bridge.

Synthetic checks:

- `python tests/test_windows_input_compat.py`: input-only renaming, preserving
  unrelated translation, idempotency, and rejection without writes.
- `tests/test_native_pad.c`: open handles, fresh packets, report layout, invalid
  inputs and stdcall ABI, with a synthetic generated-header fixture.
- `tests/test_report_concurrency.c`: 20,000 real reports from four readers.

The bridge's repeated cold-start reliability, mouse interaction, multiple
controllers and other platforms are not established. Keep this PR experimental.
