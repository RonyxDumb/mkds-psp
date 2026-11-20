# Mario Kart DS for PSP

A native PSP port of *Mario Kart DS*, built from the [mkds](https://github.com/RonyxDumb/mkds) decompilation and the
DS platform layer of [mkds-android](https://github.com/RonyxDumb/mkds-android).

This is **not an emulator**: the game's C code is compiled for the PSP's MIPS CPU and runs directly, with the Nintendo
DS hardware replaced by a native platform layer (the DS 3D engine on the PSP's GE, the 2D engines in optimised
software, the DS sound driver mixing into the PSP's audio output).

**No game content is included.** You need your own dump of **Mario Kart DS (Europe)**, game code `AMCP`
(SHA-1 `ce97d9b43f0d3ca0d48b781983e8a16f6393378f`).

## Requirements
- A PSP (any model) or PS Vita with custom firmware able to run homebrew, or the PPSSPP emulator
- About 40 MB free on the Memory Stick (the ROM is 32 MB)

## Installation
1. Copy `EBOOT.PBP` to `ms0:/PSP/GAME/MKDS/` (any folder name under `PSP/GAME` works).
2. Copy your ROM into the same folder, named `mkds.nds` (any `.nds` file there that is the game is found too).
3. Start *Mario Kart DS* from the Game menu.

The save file (`mkds.sav`) and a log (`mkds.log`) are written next to the EBOOT.

`EBOOT.PBP` comes from the GitHub Actions build of this repository (`.github/workflows/psp.yml`, artifact
`mkds-psp`), or from `dist/`.

## Controls
| PSP | DS |
| --- | --- |
| D-pad / analog stick | D-pad (steering) |
| Cross | A (accelerate, confirm) |
| Circle | B (brake, back) |
| Square | Y |
| Triangle | X |
| L / R | L (item) / R (hop, drift) |
| START | START (pause) |
| SELECT (pressed alone) | SELECT |
| SELECT + analog stick (or D-pad) | moves the touch pointer on the touch screen |
| SELECT + Cross | touches the touch screen at the pointer |
| SELECT + L / SELECT + R | previous / next screen layout |

Screen layouts: by default races show the top screen as large as the PSP's screen allows, with the touch screen
(the map) small on its right; menus show both screens side by side. SELECT + L / R switches between: top screen large,
touch screen large, top screen only, side by side (and back to automatic after the last one).

## Performance
The game runs at its full speed (60 frames per second) on a PSP at 333 MHz; the picture is drawn as often as there
is time for it (about 30 to 40 frames per second in races), the rest are skipped without slowing the game down.

## Building
Requirements: the [PSP SDK](https://github.com/pspdev/pspdev) (`psp-cmake`), CMake, Ninja, Python 3.
```shell
export PSPDEV=/path/to/pspdev PATH=$PSPDEV/bin:$PATH
psp-cmake -S port -B build/psp -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/psp
```
`build/psp/EBOOT.PBP` is the result.

### Linux test build
A headless build runs the game on a PC (32-bit x86) and writes both screens as PNG images, for debugging:
```shell
CC=clang CFLAGS=-m32 cmake -S port -B build/host -G Ninja
ninja -C build/host
build/host/mkds rom.nds --frames 1800 --dump-every 100 --out frames --press 330:A:4
```
`MKDS_F2CHECK=N` compares the fast 2D renderer used on the PSP with the complete one every N frames, pixel by pixel.

## How it works
| Path | Contents |
| --- | --- |
| `decomp/` | The decompilation, with the changes the port needs made directly in its sources |
| `tools/portify.py` | Prepares the sources the port compiles: checks register writes, relocates the DS addresses |
| `port/platform/` | DS platform layer: memory, OS, 2D and 3D graphics, sound driver, file system from the ROM |
| `port/platform/fast2d.c` | The DS 2D engines, drawn tile by tile into the PSP's 16-bit texture format |
| `port/psp/` | The PSP side: entry point, kernel threads, GE video (`ge3d.c`: the DS 3D layer), controls, sound |
| `port/host/` | The Linux test build |

- **Memory**: the PSP has no MMU, so the DS address space (main RAM, I/O registers, palettes, VRAM, OAM) cannot be
  reserved at the DS addresses. Every DS region lives in one block, the *DS window*
  (`port/platform/include/ds_addr.h`); `tools/portify.py` rewrites the DS addresses in the game's sources into
  `DSA(address)`. The VRAM banks move between their CPU view and their own storage when they are remapped.
- **3D**: the DS geometry engine runs in C; its polygons (in clip space) are clipped, taken back to view space with
  the inverse of their projection and drawn by the GE with that projection: perspective-correct textures, depth test,
  translucency, toon shading, shadow volumes (through the stencil, as the DS keeps polygon IDs).
- **2D**: rendered in software into 5551 textures (the DS's own colour format) and composited by the GE around the
  3D layer, with the master brightness applied by the GE. Display capture (dual-screen 3D, race intros) reads the
  composited screen back from the PSP's VRAM.
- **ROM**: read from the Memory Stick on demand through a small cache (32 MB do not fit in the PSP's memory).
- **Wireless**: not available.

## Legal
This project contains no Nintendo code or assets. *Mario Kart DS* and Nintendo DS are trademarks of Nintendo.
This is an unofficial fan project, not affiliated with or endorsed by Nintendo. Only use it with a game you own.
