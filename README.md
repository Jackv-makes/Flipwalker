# Flipwalker
This is a Pokewalker running on the Flipper Zero. It is a port of [Stickwalker](https://github.com/lumirth/stickwalker), which runs lumirth's reconstruction of the real Pokewalker firmware, so the actual game is the same thing that runs on the original hardware(step counting, Watts, Poke Radar, dowsing, the sounds and the IR protocol for HeartGold/SoulSilver). I didn't touch the game code, I just swapped out the Stick S3 hardware parts for Flipper ones. The reason I made this is that I wanted a Pokewalker I could carry around without paying too much an original, and the Flipper was already in my pocket.

With the add-on board below(or some hand wired parts) it connects to HeartGold/SoulSilver in both directions, I've sent a Pokemon out for a stroll and brought it back. Please note that this was built with some AI assistance: Cleaning up my code and helping with the documentation.

# Features
- Runs the real Pokewalker game logic, not a remake
- Loads a 64KB EEPROM dump from a real Pokewalker directly as a save
- Counts steps with a cheap I2C accelerometer on the GPIO header(LIS3DH, LIS2DH12, MPU-6050/6500/9250 and ADXL345 are all picked up automatically)
- Talks IrDA to the DS through the Flipper's USART1, no codec chip needed
- Two ways to draw the walker's grays on the 1-bit screen, including a real gray mode
- Open source, so do whatever you want with it

# Disclaimer
I am not a professional embedded developer, this is a hobby project and there are probably rough edges. You will need to plug parts into the GPIO header to get IR and step counting, so you need some basic soldering and a general idea of what you are doing. If you wire something wrong and let the smoke out of your Flipper, that is on you. If you see something you'd do better, feel free to fork the repo.

# Hardware
The Flipper doesn't have a motion sensor, and its built in IR only does 38kHz remote control stuff while the Pokewalker uses IrDA SIR. So both go on the GPIO header. The good news is the STM32's USART1 has an IrDA encoder/decoder built in, so an IrDA transceiver wires straight to it.

The easy way is [FlippIR](https://github.com/Jackv-makes/FlippIR-hardware), a little add-on board I made for this. It has the TFBS4711 transceiver, an LIS2DE12 accelerometer and reverse plug protection, and it just plugs into pins 9-18. If you'd rather wire your own, here is what goes where.

**IrDA transceiver(Vishay TFBS4711, the TFDU4101 works too)**

| TFBS4711 pin | Flipper pin |
| --- | --- |
| 1 VCC2(IR LED anode) | 9(3.3V) |
| 2 TXD | 13(USART1 TX, PB6) |
| 3 RXD | 14(USART1 RX, PB7) |
| 4 SD(shutdown) | GND, so it is always on |
| 5 VCC1 | 9(3.3V) |
| 6 GND | 8, 11 or 18(GND) |

**Accelerometer**

| Accel pin | Flipper pin |
| --- | --- |
| VCC | 9(3.3V) |
| GND | 8(GND) |
| SDA | 15(C1) |
| SCL | 16(C0) |

Most breakouts already have their own I2C pull-ups.

**Without a transceiver:** an IR LED on pin 13(through 100 ohm to GND) can transmit, but a bare BPV10NF photodiode on pin 14 probably won't receive. Stickwalker gets away with it by timing how fast the photodiode discharges the pin with a dedicated sampler, but here pin 14 goes straight into the hardware IrDA decoder through the ~40k pull-up and a bare diode can't fight that at normal distances. You would need a transistor or comparator stage in between.

# Build and Install
```
pip3 install --user ufbt
ufbt
ufbt launch
```

Build against the SDK that matches your Flipper's firmware or it won't load("API mismatch"). For 1.1.2 for example:

```
ufbt update --hw-target f7 --url https://update.flipperzero.one/builds/firmware/1.1.2/flipper-z-f7-sdk-1.1.2.zip
```

`ufbt launch` installs and starts it over USB, or you can copy `dist/flipwalker.fap` to `SD/apps/Games/` and find it under Apps > Games > Flipwalker.

# Playing
- Left/OK/Right are the walker's three buttons
- Back opens the Flipwalker menu(backlight auto/off, shading mode, which accelerometer was found with a rescan, whether an IR transceiver was found with a rescan, and the link state while connecting)
- Hold Back to save and exit
- When the walker's screen goes to sleep the Flipper backlight does too, any button wakes it
- During an IR connection the Flipper's LED shows how the link is doing: green while packets are getting through cleanly, red when bytes are coming in broken or the DS has gone quiet(move closer or line the windows up). IrDA has no signal strength as such, so this is really a "did the last second decode cleanly" light

The shading setting is how the walker's two grays get drawn on the Flipper's 1-bit LCD. "solid" is the default, light gray goes white and dark gray goes black for a crisp look. "gray" draws two dot patterns on alternating frames so the LCD's slow pixels blend them into actual gray. To keep that alternation steady the app takes over the display in that mode and pushes frames itself instead of going through the Flipper's GUI, locked to whatever rate the panel will take(up to 60 a second). It uses a bit more battery, so try it and see which you like. The approach comes from the [direct draw demo](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/debug/direct_draw) in the official firmware, written by skotopes at Flipper Devices, which is what made the gray mode work properly.

A fresh walker just shows the connect screen until you register it. In HeartGold/SoulSilver pick Connect to Pokewalker from the title menu, point the Flipper's IR at the DS and press OK. The game sends over the route, your Pokemon, the artwork and the sounds. Until then the walker is silent and doesn't count steps, same as the real one.

**The app has to be open to count steps.** Closing it is like pulling the battery out of a real walker. Your steps, Watts and progress for the day get saved to `SD/apps_data/flipwalker/` and when you open it again the walker's clock catches up, so hourly and daily rollovers still happen.

# Saves
The whole walker memory lives in `SD/apps_data/flipwalker/eeprom.bin`(64KB) in the exact same layout as a real Pokewalker's EEPROM, so a dump from a real one loads straight in. To swap saves, exit the app, replace `eeprom.bin`, delete the `clock.bin` next to it(it carries the old save's step count) and open the app again.

# Layout
- `lib/pokewalker/src`, `include` - the reconstructed Pokewalker firmware, untouched from upstream(only `types.h` is renamed to `pw_types.h` because the Flipper SDK already has one)
- `lib/pokewalker/stick/` - Stickwalker's hardware seam headers and glue
- `port/` - the Flipper adapters(scheduler, NT7508 panel emulation and dithering, IrDA on USART1, I2C accelerometer, EEPROM file with a 16KB write back cache, speaker, buttons, battery)
- `flipwalker.c` - the app shell, screen, keys, backlight and menu
- `art/walker_flipper.png` - the 32x32 connect-screen picture (four grays); `tools/walker_art.py` packs it into `rom_assets.h` (needs Pillow)

# Credit and Thanks
The game is lumirth's matching decompilation of the Pokewalker firmware, [pw](https://github.com/lumirth/pw)(CC0), as used in [Stickwalker](https://github.com/lumirth/stickwalker)(MIT, see `LICENSE.stickwalker`). None of this would exist without that work. The Pokemon artwork comes from your own copy of the game.

lumirth in turn built on a lot of other people's work, so the same thanks go out from here:
- [Dmitry Grinberg](https://dmitry.gr/?r=05.Projects&proj=28.+pokewalker) for the original Pokewalker hacking write-up, firmware disassembly and tools. Pretty much everything in this scene starts there.
- [UnrealPowerz](https://github.com/UnrealPowerz/pw_firm) for pw_firm, the first public go at a decompilation.
- [h4lfheart](https://github.com/h4lfheart/PocketWalker) for the PocketWalker emulator.
- Martin Korth for the Pokewalker sections of [GBATEK](https://problemkaputt.de/gbatek.htm#dscartinfraredpwalkerircommands)(IR commands, memory map, H8 registers), and nocash for the follow up research on [NESdev](https://forums.nesdev.org/viewtopic.php?start=30&t=21140).
- [mamba2410](https://github.com/mamba2410/reverse-pokewalker), [Pusty](https://github.com/Pusty/PokewalkerUtils) and [Lincoln-LM](https://github.com/lincoln-lm/pokewalker-course-injector) for the community reverse engineering, IR captures and tools.
- The [PRET](https://github.com/pret/pokeheartgold) team for the HeartGold/SoulSilver decompilation, which is how the DS side of the protocol is understood.
- Everyone in the [Pokewalker Hacking Discord](https://discord.gg/ymbTMsS), the people who keep this stuff going.

The full list of sources lumirth consulted is in [Stickwalker's references](https://github.com/lumirth/stickwalker/blob/main/docs/references.md).
