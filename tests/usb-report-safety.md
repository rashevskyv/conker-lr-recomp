# USB report safety regressions

These Windows/MinGW tests include the real `usb_gamepad.c` with a synthetic host
input provider. No game executable, generated code, controller or assets are needed.

From the repository root:

```sh
clang tests/test_report_concurrency.c -std=gnu11 -O2 \
  -I xboxrecomp/src -I xboxrecomp/src/platform -I xboxrecomp/include \
  -o test_report_concurrency.exe
clang tests/test_pad_live_environment.c -std=gnu11 -O2 \
  -I xboxrecomp/src -I xboxrecomp/src/platform -I xboxrecomp/include \
  -o test_pad_live_environment.exe
./test_report_concurrency.exe
./test_pad_live_environment.exe
```

The concurrency test checks 20,000 reports from four simultaneous readers, including
hook exclusion, report bytes and an output canary. The live-input test reuses the
environment entry's storage on mutation, making a saved `getenv` pointer unreliable;
the original script value and the originally absent state must both be restored.
The live-input test fails against the unmodified code and passes with the copy.
