# Settings and controls

## The settings menu

Open and close it with the **Back** button:

| Platform | Button |
|---|---|
| Switch | **−** (Minus) |
| PC gamepad | **Back** / **View** |
| PC keyboard | **Esc** |

The game never sees this button. (On the Xbox it only opened the Xbox Live sign-up.)

While the menu is open, the game gets no input. It keeps running: a cutscene goes on.

- **Up / Down**: choose a line.
- **Left / Right** or **A**: change the value.
- **B** or **Back**: close the menu and save `clx_options.txt`.

| Line | Values | Takes effect |
|---|---|---|
| Controller (PC) | pad 1's name, or NONE ("plug in or pair a pad"); left/right make the next pad pad 1 | at once |
| Aspect | 4:3, 16:9, 21:9 (Hor+: a wider view; HUD stretched) | at once |
| Frame rate | 30, 60 (60 is experimental) | after a restart |
| Camera | classic, free (no swing back behind Conker) | at once |
| Invert camera up/down | off, on | at once |
| Aim speed | 50 to 300 % of the game's aim speed (slingshot, weapon aim camera) | at once |
| Subtitle box | wide (40 characters x 6 lines), classic (the game's 20 x 4) | at once |
| FPS counter | off, on | at once |
| Button icons | Switch, Xbox, PlayStation (default: Switch on the Switch, Xbox on a PC) | after a restart |
| Volume | 0 to 100 | at once |
| Scale handheld / docked (Switch), Render scale (PC) | render scale in % | Vulkan at once; OpenGL after a restart |
| Anisotropic filter | 1, 2, 4, 8, 16 (Vulkan build only) | at once |
| FXAA | off, on (Vulkan build only) | at once |
| Remap controller (PC) | A: press the input for each control in turn (A B X Y, white, black, triggers, Start, stick clicks, d-pad); Back cancels, the ones done stay | at once, saved as `pad_<control>=` |
| Remap keyboard (PC) | the same for keys, plus the two sticks' directions; Esc cancels | at once, saved as `key_<control>=` |
| Reset controls (PC) | A: default keys and pad buttons, the `key_`/`pad_` lines removed | at once |
| Exit game | A, then A again ("PRESS A AGAIN"): saves and quits | — |

The list scrolls (about 11 lines at once). On a PC the window's close button
and Alt+F4 also quit (the settings are saved if the menu is open).

## Skipping movies and cutscenes

A, START (+) or L skips a cutscene, also one seen for the first time; the
first press only shows "SKIP THIS CUTSCENE? PRESS AGAIN", and a second press
within 3 seconds skips it. Cutscenes the game marks unskippable play to the
end. The boot movie and other movies work the same way with A or START
("SKIP THIS MOVIE? PRESS AGAIN"). `CLX_SKIP_CUTSCENES` in `clx_env.txt`:
`press` (default), `all` (also the unskippable ones), `1` (every cutscene
ends at once), `0` (the game's own rules).

## clx_options.txt

The menu saves its values here. You can also edit the file with a text editor. One
`key=value` per line. A line that starts with `#` is a comment.

| Where | Path |
|---|---|
| Switch | `sdmc:/switch/clx/clx_options.txt` |
| PC | `clx_options.txt` next to `clx.sh` / `clx.bat`, or the path in `CLX_OPTIONS` |

| Key | Values | Meaning |
|---|---|---|
| `aspect` | `4:3`, `16:9`, `21:9` | Picture shape. 16:9 and 21:9 are a wider view, not a stretch. |
| `fps` | `30`, `60` | Frame rate. 60 is experimental. Needs a restart. |
| `camera` | `classic`, `free` | `free`: no automatic swing behind Conker. |
| `camera_invert_y` | `0`, `1` | Flip the right stick up/down. `camera_invert` is the same key. |
| `aim_speed` | `25`-`400` | Aim speed in % while aiming (slingshot, frying pan, bat): how fast the right stick, or a gyro mapped onto it, turns the aim. Default 100 (the game's own). Env: `CLX_AIM_SPEED`. |
| `bubble` | `wide`, `classic` | Subtitle box size. |
| `bubble_wrap`, `bubble_lines`, `bubble_height` | numbers | Set the subtitle box by hand (for translations). Not in the menu. |
| `fps_counter` | `0`, `1` | Show the FPS digits top-left. |
| `volume` | `0`-`100` | Sound volume. |
| `scale_handheld` | percent | Switch handheld render scale (150 = 960x720). On a PC: the render scale (100 = 640x480). |
| `scale_docked` | percent | Switch docked render scale (225 = 1440x1080). |
| `aniso` | `1`, `2`, `4`, `8`, `16` | Anisotropic filtering (Vulkan build). |
| `fxaa` | `0`, `1` | FXAA (Vulkan build). |
| `button_glyphs` | `switch`, `xbox`, `ps` | Button icons in the game (default: Switch on the Switch, Xbox on a PC). |
| `rumble_damage` | `0`, `1` | The pad rumbles when Conker is hurt (default on). The game itself rumbles only for a few scripted events; this adds hits and falls. The game's own Vibration setting also turns it off. |
| `rumble_hit` | `0`, `1` | A short rumble when Conker's weapon hits an enemy, a stronger one when it finishes it (default on). The game's Vibration setting also turns it off. |
| `vignette` | `0`, `1` | The game's darkened screen edges in the levels (default on). `0` draws the picture without them. |
| `grass_wind` | `0`, `1` | Distant (fading) grass sways in the wind like the near grass (default on). `0` is the game's own: only the near grass moves. |
| `language` | `english`, `french`, `german`, `spanish`, `italian`, `japanese` | The console language the game reads at start: menu and subtitle text from the disc's own banks (the voices stay English). Takes effect at the next start. Env: `RECOMP_LANGUAGE`. |
| `key_<control>` | a key name | PC keyboard binding. See below. |
| `pad_<control>` | a pad input | PC gamepad binding. See below. |

Only the PC launchers (`clx.sh`, `clx.bat`) read these three keys. The game ignores them:

| Key | Values | Meaning |
|---|---|---|
| `game` | a folder | The unpacked disc, if you do not give it on the command line. |
| `skip_intro` | `0`, `1` | `1`: skip the boot movie. |
| `start_single` | `0`, `1` | `1` (default): the title cursor starts on Game 1. |

Lines the menu does not own are kept when it saves.

A setting in `clx_env.txt` (Switch) or in the environment (for example `CLX_ASPECT`) wins
over the file at start. The exceptions are `fps_counter` and `volume`: the file wins.

## Button icons

The game draws Xbox button icons (pause board, speech bubbles, tutorials).
With `button_glyphs=switch` they are drawn as Switch buttons with the same
letters (the Xbox A icon becomes a Switch A icon), because the Switch buttons
act by their names (see Controls below); with `RECOMP_PAD_LAYOUT=position`
in `clx_env.txt` the icons follow the position instead (Xbox A becomes the
Switch's B). With `ps` they are drawn by position: the Xbox bottom button A
becomes ✕, B becomes ○, X becomes □, Y becomes △. The triggers become ZL/ZR
(L2/R2) and the stick a plain stick. The glyphs are the
port's own drawings (`tools/make_glyphs.py`, preview `docs/glyphs_preview.png`),
matched to the game's icon textures by the hash of their pixels
(`src/clx_buttons.c`). Not replaced yet: the Xbox controller picture on the
pause menu's Options page, and icons that have not been seen in a test (Z,
the arrows, Start/Back, white/black).

## Controls

### Switch

The Switch buttons keep their names: **A** is A, **B** is B, **X** is X, **Y** is Y.

| Switch | Game |
|---|---|
| Left stick | move |
| Right stick | camera |
| D-pad | d-pad |
| A / B / X / Y | A / B / X / Y |
| L / R | White / Black |
| ZL / ZR | left / right trigger |
| + | Start |
| − | settings menu |
| Stick click | stick click |

**+**, **A** or **ZL** (the left trigger) skips a cutscene. On a PC: **Start**, **A** or the left trigger (keyboard: Enter, Space or Left Shift). To use the buttons by position (the Xbox layout),
set `RECOMP_PAD_LAYOUT=position` in `clx_env.txt`.

### PC gamepad

Any SDL2 game controller works: XInput, DirectInput, Switch Pro, DualShock. You can plug it
in or out while the game runs. It uses the Xbox layout. **Back** opens the settings menu.

### PC keyboard

The keyboard is always pad 1, together with a gamepad on that port.

| Keys | Game |
|---|---|
| W A S D | left stick (move) |
| Arrows, I J K L | right stick (camera) |
| Space | A |
| E | B |
| Q | X |
| R | Y |
| Z / X | White / Black |
| Left Shift / Left Ctrl | left / right trigger |
| Enter | Start |
| Esc | settings menu |
| C / V | left / right stick click |
| 1 2 3 4 | d-pad up / down / left / right |

To change a key, add `key_<control>=<key name>` to `clx_options.txt`. Use SDL key names.
For example `key_a=Return` or `key_rs_up=Keypad 8`. A binding replaces both default keys of
that control.

The controls are: `up`, `down`, `left`, `right`, `start`, `back`, `lthumb`, `rthumb`, `a`, `b`,
`x`, `y`, `black`, `white`, `lt`, `rt`, `ls_up`, `ls_down`, `ls_left`, `ls_right`, `rs_up`,
`rs_down`, `rs_left`, `rs_right`.

Pad buttons work the same way: `pad_<control>=<SDL game-controller input>`,
for example `pad_a=b`, `pad_lt=leftshoulder`, `pad_black=+righty` (an axis
half: `+` or `-` and the axis name). Controls: `up down left right start
lthumb rthumb a b x y black white lt rt`; Back stays the menu button. The
menu's Remap lines write these lines; Reset controls removes them.

### PC mouse camera and fullscreen

The PC build reads relative mouse motion and feeds it through the game's own
right-stick input, so the camera follows the mouse with the game's normal
camera logic. It uses the free camera, so the view stays where you point it.

| Input | Action |
|---|---|
| Mouse | camera |
| F8 | release / capture the mouse |
| Esc | settings menu (the mouse is released while it is open) |
| Alt+Enter, F11 | borderless fullscreen on / off |

The mouse is captured only while the game has focus and the in-game camera is
active, so the title screen and menus use a normal pointer.

Settings live in `clx_mouse_options.txt` next to the executable:

| Key | Meaning |
|---|---|
| `sensitivity` | degrees of rotation per mouse pixel (default 0.06) |
| `sensitivity_y` | the same vertically (0 or absent: use `sensitivity`) |
| `invert_x` | `1` reverses left/right |
| `invert_y` | `1` = mouse up looks up (the shipped default); `0` = inverted |
| `aim_gain` | extra gain while a weapon's aim camera is active (default 1.8) |

The mouse also drives the weapon aim camera (frying pan, baseball bat); the
slingshot's aim and the vehicles keep the pad. The aim camera turns more
slowly than the normal one, hence `aim_gain`.

Start the Windows build with `clx.bat` (`clx.sh` on Linux). Mouse and borderless
fullscreen are enabled from `clx_options.txt` with `mouse=1` and
`fullscreen=borderless`; both are in the shipped PC options file.

Not yet: a window-size setting.
