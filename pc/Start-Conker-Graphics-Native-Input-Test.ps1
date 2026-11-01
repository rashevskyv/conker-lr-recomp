$ErrorActionPreference='Stop'
Set-Location -LiteralPath $PSScriptRoot
$env:NFSU2_GAME_DIR=Join-Path $PSScriptRoot 'game'
$env:CLX_OPTIONS=Join-Path $PSScriptRoot 'clx_options.txt'
$env:CLX_MOUSE_OPTIONS=Join-Path $PSScriptRoot 'clx_mouse_options.txt'
$env:RECOMP_PROG_CACHE='0'
$env:CLX_START_SINGLE='1'
$env:RECOMP_PAD_MASK='0x20'
$env:RECOMP_QUIET='1'
$env:CLX_NATIVE_MOUSE='1'
$env:CLX_BORDERLESS='1'
$env:RECOMP_INPUT_DIAG='1'
$env:CLX_GFX_INPUT_DIAG='1'
$env:RECOMP_USB_HC='0'
$env:RECOMP_USB_PADS='1'
$env:RECOMP_USB_PORT='2'
$env:RECOMP_USB_NDP='4'
$env:CLX_NATIVE_PAD='1'
# Physical input only for the user's test; no synthetic presses.
'RECOMP_PAD_LIVE','RECOMP_PAD_SCRIPT','RECOMP_PAD2_SCRIPT','RECOMP_GL_TRACE' | ForEach-Object {Remove-Item "Env:$_" -ErrorAction SilentlyContinue}
Start-Process -FilePath (Join-Path $PSScriptRoot 'clx-graphics-native-test.exe') -WorkingDirectory $PSScriptRoot -RedirectStandardOutput (Join-Path $PSScriptRoot 'graphics-native-input-test.log') -RedirectStandardError (Join-Path $PSScriptRoot 'graphics-native-input-test.err.log')
