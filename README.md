# DOOM for LILYGO TTGO VGA32

Playable port of [doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom / Linux Doom) to the **LILYGO TTGO VGA32 v1.4** board (FabGL VGA32): VGA 320×200 **64 RGB222 colors**, PS/2 keyboard and IWAD on SD card.

Same hardware and same PlatformIO (`board = pico32`, FabGL, SD pins) as [ESP32-Basic-vga](https://github.com/fg1998/ESP32-Basic-vga).

## What you need

- LILYGO TTGO VGA32 v1.4 (ESP32 WROVER **with PSRAM**)
- VGA monitor
- PS/2 keyboard (purple port)
- FAT32 microSD card with `doom1.wad` (or another IWAD from the table below)
- PlatformIO (`pio`)

## How to flash

Remove the SD card, plug in USB, then:

```bash
pio run -e lilygo-vga32 -t upload
pio device monitor -b 115200
```

In Arduino IDE: ESP32 Dev board / Huge APP, PSRAM enabled, FabGL library, open the project folder.

After flashing, put the SD back in and reset.

SD: tries MISO=2 (LilyGO) and, if that fails, MISO=35. CS=13, SCK=14, MOSI=12.

For [ESP32 Bootloader](https://github.com/ESP-WORKS/ESP32_Bootloader): run `pio run` and copy `.pio/build/lilygo-vga32/firmware.bin` to the folder on the SD (together with `version.txt`). `setup()` erases the `otadata` partition so the next reset goes back to the menu. `ota_0` slot limit: 2816 KB.

## IWAD

The engine needs the original game data. It's not shipped in the repo.

```bash
./tools/download_shareware_wad.sh   # doom1.wad shareware
```

Copy the `.wad` to the **root** of the SD. Recognized names: `doom1.wad`, `doom.wad`, `doom2.wad`, `freedoom1.wad`, `freedoom2.wad`.

Without a WAD, the board sits on a wait screen (VGA + serial) until the file shows up.

## bootl.rc (optional)

On the SD root, `bootl.rc` can override the PS/2 keyboard pins (and the button). If the file isn't there, the LilyGO VGA32 default applies (DAT=32, CLK=33, button=36).

```
kbddat=32
kbdclk=33
magicb=36
```

The file is read at startup, **before** PS/2 is brought up. DAT/CLK pins must be RTC GPIO (2, 4, 12–15, 25–27, 32, 33).

## Controls

| Action | PS/2 | Serial (115200) | IO36 button |
| --- | --- | --- | --- |
| Move / turn | arrows or WASD | WASD | — |
| Fire | Ctrl or Z | `z` / `f` | pressed |
| Use / open | Space or E | space / `e` | — |
| Run | Shift | — | — |
| Strafe | Alt, `,` `.` | — | — |
| Menu | Esc, Enter | Esc, Enter | — |

## Video and memory

- VGA **640×200@70 Hz DoubleScan** (`VGADirectController`): the same 400-line timing as the original 320×200, just 2× wider horizontally. Plain 640×400 and 640×480 stretched Doom vertically on this monitor.
- Bayer 4×4 dither only on the *remainder* of the RGB222 quantization.
- The ISR only copies the pre-built line. The 640×200 Bayer dither (the earlier one) runs in the game, not in the ISR.
- Serial should show `VGADirect 640x200 70Hz DS` and `colors=64`.
- In-game, **F11** cycles Doom's gamma (brighter / darker).
- Engine tables and game heap (~2–3 MiB) live in **PSRAM**.

- **Sound:** SFX and **music** on the VGA32's **3.5 mm** jack (DAC GPIO25 and GPIO26, I2S0). WAD music is MUS synthesized as chiptune (not the original AdLib). Volume in the Options menu.

## Layout

```
src/main.cpp                 boot, game task, WAD wait
src/doomgeneric_fabgl.cpp    FabGL VGA, PS/2, SD, keyboard
src/i_sound_esp32.cpp        SFX on GPIO25 jack (I2S0 DAC)
src/doom/                    doomgeneric engine (GPLv2+)
platformio.ini               VGA32 / pico32, huge_app, PSRAM
```

## License

- Doom / doomgeneric engine: **GPL-2.0-or-later** (`src/doom/COPYING`)
- FabGL: **GPL-3.0**
- This port: **GPL-3.0-or-later** (combined work)

The shareware `doom1.wad` can be redistributed; commercial WADs cannot.