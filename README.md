### Conker: Live & Reloaded for Switch, Linux and Windows

![License: MIT](https://img.shields.io/badge/license-MIT-blue) ![Platform: Nintendo Switch](https://img.shields.io/badge/platform-Nintendo%20Switch-red) ![No game data](https://img.shields.io/badge/game%20data-not%20included-lightgrey)

[Українська](README.uk.md) · [Documentation](docs/README.md)

An unofficial port of the original Xbox game **Conker: Live & Reloaded** (Rare, 2005). It runs
natively on a hacked Nintendo Switch, on Linux and on Windows. It is not an emulator: a script
lifts the game's x86 code to C and builds it for your platform. The Xbox GPU commands go to
Vulkan (Switch) or OpenGL (all platforms).

> **This repository has no game code and no game data.** Ready-made builds are in
> [Releases](https://github.com/rashevskyv/conker-lr-recomp/releases). They contain code made from the game, but no game data: you need
> your **own unpacked copy** of the game. You can also build the program yourself, from your own
> `default.xbe`.

**Status:** the single player game is playable. The Switch runs it at a stable 30 fps. True
16:9, 60 fps (experimental), a free camera and an in-game settings menu are included.
Multiplayer and Xbox Live do not work.

<!--
## Screenshots

![Title screen](docs/images/01-title.png)
![Settings menu](docs/images/03-settings.png)
![4:3 and 16:9](docs/images/04-aspect.png)
-->

| Platform | How you build | Renderer | Tested |
|---|---|---|---|
| Nintendo Switch (Atmosphère) | Linux, WSL2 or Docker | Vulkan (NVK) or OpenGL | on a console; the OpenGL app also in the Eden emulator |
| Linux x86-64 | Linux | OpenGL 3.3 | builds and renders the game (software GL, frame dumps) |
| Windows 10/11 x64 | MSYS2 | OpenGL 3.3 | plays on one PC (Windows 10, NVIDIA): keyboard and menu tested |
| Android 11+ arm64 | Linux | Vulkan 1.3 | experimental: builds, not tested on a phone yet ([android/README.md](android/README.md)) |

## What you need

- Your **own** copy of the game for the original Xbox: the **retail NTSC (North America)** version.
  Its `default.xbe` must have this SHA-1: `2232a1b3874d67b4bb4cf53a940420d2dab59535`.
  Other versions are not tested. The build script stops on them.
- A PC to build on: 16 GB of RAM and about 20 GB of free disk.
- **Switch:** a Switch with [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) and
  hbmenu, and 5 GB free on the microSD card.
- **Linux and Windows:** a GPU with OpenGL 3.3. A gamepad is optional: the keyboard works.

## How to install

**The easy way:** download your platform from [Releases](https://github.com/rashevskyv/conker-lr-recomp/releases), unpack it, and put
your unpacked game folder next to it (see the notes of the release). **Or build it yourself**
from your own copy of the game, as below. More details are in [Building](docs/building.md).

### Step 1. Unpack your disc

Unpack the disc image to a folder, for example with
[extract-xiso](https://github.com/XboxDev/extract-xiso):

```bash
extract-xiso -x Conker.iso -d conker-disc
sha1sum conker-disc/default.xbe
```

The folder has about 881 files (4.5 GB). Check that the SHA-1 is the one above.

### Step 2. Get this repository

Clone it and go into the folder. On Windows with WSL2, clone **inside** Linux (`~/`, not
`/mnt/c/`). The Windows drives are slow there.

### Step 3. Build and install

<details open>
<summary><b>Nintendo Switch</b></summary>

With Docker (the easy way). The first build takes about 1 hour.

```bash
docker build -t clx-build .
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -v "$HOME/conker-disc:/xbe:ro" \
  clx-build ./build_from_xbe.sh /xbe/default.xbe
```

Without Docker, see [Building](docs/building.md).

Then:

1. Copy the contents of `out/sdmc/` to the root of the microSD card.
2. Copy your unpacked disc folder to `sdmc:/switch/clx/game/`.
   The file `sdmc:/switch/clx/game/default.xbe` must exist.
3. Start the app with **title takeover**: hold **R** while you open any game, then pick the
   app in hbmenu. (Album mode does not have enough memory.)
4. Let the first start finish. It compiles shaders and can stutter for a few minutes.

Use `clx-vulkan.nro` (recommended). `clx.nro` is the OpenGL fallback.
</details>

<details>
<summary><b>Linux</b></summary>

```bash
sudo apt install build-essential cmake ninja-build pkg-config git python3-pip \
  libsdl2-dev libssl-dev libepoxy-dev
pip3 install --break-system-packages capstone==5.0.7 Pillow
VARIANTS=linux ./build_from_xbe.sh ~/conker-disc/default.xbe
out/linux/clx.sh ~/conker-disc
```

The `out/linux` folder is complete. The game saves into the disc folder, so use a folder you
may write to.
</details>

<details>
<summary><b>Windows</b></summary>

1. Install [MSYS2](https://www.msys2.org) (for example `winget install MSYS2.MSYS2`).
2. In PowerShell, in the repository folder:

   ```powershell
   .\build_from_xbe.ps1 C:\conker-disc\default.xbe -Install
   .\out\windows\clx.bat C:\conker-disc
   ```

The `out\windows` folder is complete. You can move it. The game saves into the disc folder, so
use a folder you may write to. Messages go to `clx_log.txt` next to `clx.bat`.
</details>

## How to play

Open the **settings window** at any time. It saves `clx_options.txt`.

| Platform | Settings button |
|---|---|
| Switch | **−** (Minus) |
| PC gamepad | **Back** / **View** |
| PC keyboard | **Esc** |

### Controls

| Game | Switch | PC gamepad | PC keyboard |
|---|---|---|---|
| Move | left stick | left stick | W A S D |
| Camera | right stick | right stick | arrows or I J K L |
| A / B / X / Y | A / B / X / Y | A / B / X / Y | Space / E / Q / R |
| White / Black | L / R | left / right shoulder | Z / X |
| Triggers | ZL / ZR | triggers | Left Shift / Left Ctrl |
| Start | + | Start | Enter |
| Skip a cutscene | + , A or ZL | Start, A or left trigger | Enter, Space or Left Shift |

A gamepad can be plugged in or out while the game runs. The keyboard is always pad 1. In the
settings window you can see the pad that is in use, **remap the gamepad and the keyboard**, reset
the controls and **exit the game**. (Or edit `key_a=Return` and `pad_a=b` in `clx_options.txt`.) All controls and keys are in
[Settings and controls](docs/settings-and-controls.md).

### Options

One `key=value` per line in `clx_options.txt` (Switch: `sdmc:/switch/clx/`; PC: next to
`clx.sh` / `clx.bat`).

| Key | Default | Meaning |
|---|---|---|
| `aspect` | `4:3` | `16:9` is a wider view, not a stretch. The HUD and menus are stretched. |
| `fps` | `30` | `60` is experimental. Needs a restart. |
| `camera` | `classic` | `free`: the camera does not swing back behind Conker. |
| `camera_invert_y` | `0` | `1`: flip the right stick up/down. |
| `bubble` | `wide` | Subtitle box: `wide` is 40 characters x 6 lines; `classic` is the game's 20 x 4. |
| `volume` | `100` | Sound volume, 0 to 100. |
| `fps_counter` | `0` | `1`: show the FPS digits. |
| `scale_handheld`, `scale_docked` | `150`, `225` | Switch render scale in percent. On a PC, `scale_handheld` is the render scale (default 100). |
| `aniso`, `fxaa` | `16`, `1` | Anisotropic filtering and FXAA (Vulkan app). |
| `vignette` | `1` | `0`: no darkened screen edges in the levels. |
| `grass_wind` | `1` | Distant grass sways in the wind like the near grass; `0`: the game's own. |
| `language` | `english` | Menu and subtitle text: `english`, `french`, `german`, `spanish`, `italian`, `japanese` (the voices stay English). Needs a restart. |

## Troubleshooting

- **Hang on the loading logo (0 fps).** You quit during the first start. Delete the cache folder
  and start again: `sdmc:/switch/clx/save/Cache` on the Switch, the `CACHE` folder in the disc
  folder on a PC.
- **The Switch app crashes or lacks memory.** Use title takeover: hold **R** while you open a game.
- **"Failed to load default.xbe" on a PC.** Give the disc folder: `clx.sh <folder>` or
  `clx.bat <folder>`.
- **The build says the SHA-1 is wrong.** You need the retail NTSC (North America) version.

More in [Troubleshooting](docs/troubleshooting.md).

## Testing: what to send with a bug report

This is a test build. If something goes wrong on a Switch, please send these four files from
`sdmc:/switch/clx/` on the microSD card:

| File | What it is |
|---|---|
| `clx_log_vk.txt` | the log of the last run (`clx_log.txt` if you ran the OpenGL app) |
| `clx_log_vk.prev.txt` | the log of the run before it (a crash is often in this one) |
| `clx_options.txt` | your settings |
| `clx_env.txt` | the advanced switches |

Also say the version shown in hbmenu (for example `0.54.257ded-vk`), where in the game it
happened, and what you did just before. Please do not send game files.

## FAQ

**Do the downloads include the game?** No. They have no game data (no disc files, textures or
sound). You need your own unpacked copy of the game, retail NTSC (North America). The programs do
contain code made from the game. If you prefer, build them yourself from your own `default.xbe`.

**Can I use a PAL or another version?** Not tested. The build stops unless the XBE matches.

**Does it run in an emulator?** The OpenGL Switch app runs in Eden. The Vulkan app needs a real
Switch. On a PC the game runs natively, no emulator.

**Is multiplayer or Xbox Live possible?** No. The target is the single player game.

**Is 60 fps safe?** It is experimental. Some timing (cutscenes, physics) can differ.

**Can I translate the game?** Yes. See [Translating the game](docs/translation.md). The wide
subtitle box (`bubble=wide`) gives longer lines room.

**Does the PC build play sound?** It is built in, but PC sound is not tested well yet.

## Translation

A kit in `tools/l10n/` unpacks the game's text, fonts and voices to JSON, PNG and WAV, and packs
them back. See [Translating the game](docs/translation.md) and
[Text and subtitle limits](docs/conker-text-limits-picoripi.md).

## Credits and licences

- **sp00nznet**: [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (MIT), the Xbox static
  recompilation toolkit (in `xboxrecomp/`).
- **antoxa2584x**: [flatout2-sw](https://github.com/antoxa2584x/flatout2-sw) (MIT). This port
  started as a copy of it. Ideas also from his NFSU, NFSU2, NFS Carbon and Burnout 3 ports.
- **danfromtico**: [mesa-switch](https://github.com/danfromtico/mesa-switch), Mesa NVK for the
  Switch GPU. The **Mesa** project: NVK.
- **xemu** developers: the MCPX APU code in the toolkit (LGPL-2.1, see `xboxrecomp/NOTICE`).
- **nastys**: disc format tools for this game (CAFF text, XWB banks).
- devkitPro and libnx, Atmosphère, hbmenu, SDL2, glslang, MSYS2.

The source code is under the MIT licence: [LICENSE](LICENSE). Third-party parts and their
licences: [THIRD_PARTY.md](THIRD_PARTY.md).

This is an unofficial fan project. It is not connected to Microsoft or Rare. Conker is a
trademark of its owners. No game code or assets are distributed.
