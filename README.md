# DMX Receiver for the Raspberry Pi Pico

Firmware for a Raspberry Pi Pico (RP2040) that reads a DMX512 signal and drives four channels as RGBW — simultaneously as PWM on four power outputs and mirrored to a WS2812 pixel. It is operated with three buttons and a 128×64 OLED. Without a DMX signal, built-in auto effects run instead.
## Features
- DMX512 reception of the full 512-channel universe
- 4 output channels (R, G, B, W) from a freely selectable start address
- 800mA output current
- USB-C PD input 20V
- Configurable number of channels in use (1–4)
- RGBW output as PWM (500 Hz) and in parallel on a WS2812/NeoPixel
- OLED menu with buttons: start address, channel count, auto effect
- Settings are stored in EEPROM and restored on boot
- DMX takes priority: as soon as a signal is present, the auto effect is overridden
- Display turns off after 5 s of inactivity; the first button press wakes it
- At boot the firmware checks for DMX; without a signal the color fade starts automatically
## Auto effects

| Effect     | Description                                    |
| ---------- | ---------------------------------------------- |
| LED Auto   | Endless fade Red → Green → Blue                |
| Only White | White on all channels (W = 255 only)           |
| Blue-Green | Fade Blue ↔ Green with a 5 s pause at each end |
## Hardware / pinout

| Function    | GPIO | Note                                     |
| ----------- | ---- | ---------------------------------------- |
| DMX RX      | 0    | `dmxInput.begin(0, 1, 512)` – RX = GPIO0 |
| Button UP   | 3    | to GND, internal pull-up                 |
| Button MODE | 4    | to GND, internal pull-up                 |
| Button DOWN | 5    | to GND, internal pull-up                 |
| LED White   | 9    | PWM                                      |
| LED Blue    | 10   | PWM                                      |
| LED Green   | 11   | PWM                                      |
| LED Red     | 12   | PWM                                      |
| OLED SDA    | 14   | I2C (Wire1)                              |
| OLED SCL    | 15   | I2C (Wire1)                              |
| WS2812 data | 16   | 1 pixel                                  |
OLED: SSD1306, 128×64, I2C address `0x3C`.

The DMX input needs an RS-485 transceiver (e.g. MAX485) running at 3.3 V; twist the A/B pair and don't forget bus termination at both ends.
## Operation
**Main menu** (MODE to enter, UP/DOWN to scroll):
1. `DMX` → set start address and channel count
2. `AUTO` → choose an auto effect and start it with MODE

**In the DMX submenu:** MODE switches between *Start Addr* and *Num Channel*. UP/DOWN changes the value; MODE on the second field saves and returns. The address is automatically clamped so all channels fit inside the universe (max. `512 - channels + 1`).

**In the AUTO submenu:** UP/DOWN selects an effect, MODE starts it and returns to the main menu.
## Build
Platformio with the **Raspberry Pi Pico / RP2040** core (Earle Philhower).
Required libraries:
- `DmxInput` (DMX input via PIO, by ![jostlowe](https://github.com/jostlowe/Pico-DMX))
- `Adafruit SSD1306`
- `Adafruit GFX Library`
- `Adafruit NeoPixel`
- `EEPROM` (included in the RP2040 core)
Board: *Raspberry Pi Pico*, sketch: `dmx_receiver.ino`.
## Notes
- When idle without DMX, the display does not refresh continuously — it redraws only on button presses and every 250 ms in the main menu.
- DMX handling is non-blocking, so buttons and display stay responsive even when the signal drops out.
- `buffer[0]` is the DMX start byte, which is why channel evaluation begins at `address + i` in the buffered universe.
# Not included in the Repo
- Some LEDs to light up
## License
GPL-3.0-or-later, for Firmware, see `LICENSE`. Copyright (c) 2026 Per-Simon Saal.

CERN-OHL-S-2.0, for Hardware and Enclosure, see `LICENSE`. Copyriht (c) 2026 Per-Simon Saal. 
