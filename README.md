# hw-waveshare-28b — Waveshare ESP32-S3-Touch-LCD-2.8B board HAL

```
boot: detect_hw()          expander 0x20 + PCF85063 + QMI8658 + GT911 on SDA15/SCL7
 ↓ all four answer                          no → halt: this image is on another board
Waveshare28bBoard::onStart (before spangapInit — order is load-bearing)
 ↓
 1. I2C bus on port 0      SDA 15 / SCL 7   → touch, RTC and IMU later ADOPT it
 2. PCA9554 at 0x20        resets released, panel deselected, card deselected
 3. ST7701S register table over the panel's 3-wire SPI (GPIO 1/2, CS on P2)
 4. microSD chip-select    expander P3 asserted, and left asserted for good
 ↓
spangapInit()              fs_mount_sd() on the two wires the panel just freed
 ↓
Waveshare28bBoard::onInit  publishes sys.board
Waveshare28bBattery::onInit  ADC on GPIO 4, then 1/min → battery.millivolt/percent
 ↓
spangap-lcd                RGB timing generator starts; the glass has been
                           configured since step 3 and is waiting for pixels
```

**hw-waveshare-28b** is the board-support straddle for the **Waveshare
ESP32-S3-Touch-LCD-2.8B**: an ESP32-S3R8 (16 MB flash, 8 MB **octal** PSRAM)
behind a 2.8" **480x640** IPS panel — an **ST7701S** on a 16-bit RGB parallel
bus — with **GT911** capacitive touch, a **QMI8658** IMU, a **PCF85063** clock,
a microSD slot, a buzzer, a 3.7 V lithium connector with an onboard charger, and
a **PCA9554** IO expander carrying the lines that were left over. Board
reference: <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8B>.

**No radio but the SoC's own.** There is no LoRa modem on this board, so it does
not stage `iface-lora` and an image built on it has no LoRa settings section, no
`s.lora.*` keys and no LoRaMon tile — nothing offers an operator a radio that is
not there. What this device joins a mesh with is WiFi, BLE and ESP-NOW. Its
network graph still draws the community's LoRa links in LoRa's own colour:
`rnsd` publishes that, not the radio straddle.

It is a **non-buildable** component — it decides nothing about what the device
*does*. A buildable assembler (`reticulous/reticulous`) adds it and inherits the
board: `spangap build reticulous/reticulous --with spangap/hw-waveshare-28b`.
The mesh stack, the IP/web platform, `app_main`, the partition layout, the
update story and the browser SPA all come from the buildable and its other
straddles — not from here.

The board stages `spangap-lcd` (it is a screen with a computer behind it; drop
with `--no-lcd`), `spangap-rtc` and `imu`. There is **no hardware keyboard** and
no button an operator can reach — BOOT is the only one and it is under the case
— so the glass is the whole of this board's input: spangap-lcd's on-screen
keyboard for text, and touch-to-wake as the only way back from standby.

## ⚠️ Verify before trusting

The pin map, the panel timings and the ST7701S register table were assembled
from Waveshare's own documentation and from a published working configuration
for this exact board, **not** from a board in hand. Confirm against your unit:

- **The panel's register table** (`kSt7701Init` in `esp-idf/src/waveshare28b.cpp`)
  is the panel vendor's, verbatim, plus the three commands any driver appends
  after such a list: `MADCTL` (0x36), `COLMOD` (0x3A) and display-on (0x29).
  `COLMOD` says **18 bit** on a sixteen-line bus, which is the whole trick of
  this wiring: the SoC's sixteen lines land on the top of an eighteen-bit input
  (`B1..B5`, `G0..G5`, `R1..R5`), so each channel is short its least significant
  bits and nothing else. Told 16 bit, the panel looks for red where green is,
  and the picture comes back in the wrong colours.
- **Backlight on GPIO 6.** Waveshare's pin table names it there. If the screen
  is drawn but dark, the backlight is the thing to look for on the expander
  instead — some boards in this family put it on an expander line.
- **The microSD's permanently-asserted select** (see below). If the card never
  mounts, this is the first thing to suspect; a board with no card in the slot
  looks the same in the log.
- **Battery divider.** The 3/1 ratio is Waveshare's usual 200k/100k pair. Trim
  `BAT_DIV_NUM/DEN` in `esp-idf/src/waveshare28b.cpp` if a multimeter disagrees.

## What it does, and how it fits

The board contributes hooks that the buildable's generated init dispatcher
calls. There is nothing to call by hand: if the straddle is in the build, the
board comes up automatically.

| Hook | Band | Present when | Brings up |
|---|---|---|---|
| `Waveshare28bBoard::onStart` | start | always | I2C bus, PCA9554 expander, ST7701S register table, microSD select |
| `Waveshare28bBoard::onInit` | init | always | publishes `sys.board` |
| `Waveshare28bBattery::onInit` | init | always | 1/min ADC sampling → `battery.millivolt` / `battery.percent` |

Everything else on the board belongs to a generic straddle, configured from
`straddle.yaml` and given no C code here:

| Part | Owner | Configured by |
|---|---|---|
| ST7701S RGB panel + backlight | [spangap-lcd](../spangap-lcd) `lcd_panel_rgb.cpp` | `CONFIG_LCD_BUS_RGB`, `CONFIG_LCD_RGB_*` |
| GT911 touch | [spangap-lcd](../spangap-lcd) `lcd_touch.cpp` | `CONFIG_LCD_TOUCH_*` |
| PCF85063 clock | [spangap-rtc](../spangap-rtc) | `CONFIG_RTC_CHIP_PCF85063`, `CONFIG_RTC_I2C_*` |
| QMI8658 motion | [imu](../imu) | `CONFIG_IMU_BUS_I2C`, `CONFIG_IMU_I2C_*` |
| microSD | [spangap-core](../spangap-core) `fs.cpp` | `CONFIG_SPANGAP_SDCARD_*` |

**The panel's registers are the exception, and they are here for a reason.** An
RGB panel has no command channel while it runs — the SoC just refreshes it — so
its controller must be configured beforehand, over a side channel that on this
board is a 3-wire SPI whose chip-select is on the IO expander and whose other
two wires belong to the microSD. Nothing generic can be said about a channel
like that, so the sequence lives in the board's `onStart`, at the one moment
those pins are free. By the time spangap-lcd starts the timing generator, the
glass is configured and waiting.

## Board identity (`detect_hw`)

`esp-idf/src/detect.cpp` answers whether the hardware under this firmware is a
2.8B: 16 MB of flash, then the IO expander at 0x20 on SDA 15 / SCL 7, then —
after releasing the resets that expander holds — the PCF85063 at 0x51, the
QMI8658 identifying itself at 0x6B, and the GT911 reporting its "911" product
ID. The last of those is what separates this board from the bare-LCD variant of
the same PCB.

spangap-core calls it before any bus is claimed and halts the device awake on a
mismatch, because every pin in this image would otherwise belong to someone
else's board. The same function is copied by hand into flashmon's standalone
detector — change one, change the other (`flashmon/docs/detect.md`).

## Hardware & pin map

Nearly every pin is spoken for by the display: sixteen data lines and four sync
signals. What is left is one I2C bus, one two-wire channel used twice, one ADC
pin and the expander.

### Display (ST7701S, 16-bit RGB, 480x640 glass, held landscape at 640x480)

| Signal | GPIO |
|---|---|
| HSYNC / VSYNC / DE / PCLK | 38 / 39 / 40 / 41 |
| Blue, bit 0→4 | 5, 45, 48, 47, 21 |
| Green, bit 0→5 | 14, 13, 12, 11, 10, 9 |
| Red, bit 0→4 | 46, 3, 8, 18, 17 |
| Backlight (LEDC PWM) | 6 |
| Configuration SDA / SCL (3-wire SPI, 9-bit frames) | 1 / 2 — shared with the microSD |
| Configuration CS / panel reset | expander P2 / P0 |

Timing: 16 MHz pixel clock, hsync 10/70/60 (pulse/back/front, pixel clocks),
vsync 10/20/20 (lines) — a 620×690 total frame at about 37 Hz, roughly 32 MB/s
of PSRAM read.

That read is why **bounce buffers are on** (`CONFIG_LCD_RGB_BOUNCE_LINES=10`),
and on this board they are not optional — the picture walks down the glass
without them, repeatably. Straight from PSRAM the panel's DMA queues behind
every cache miss the CPU takes, *and* esp_lcd writes back the whole
framebuffer's worth of cache lines on every flush; a starved RGB panel does not
glitch and recover, its frame starts in the wrong place and stays there. Ten
lines of internal RAM, twice over, removes both. The refill they cost is a CPU
copy against a 0.7 ms deadline, which this board meets at 80 MHz — so nothing
here pins the processor (`panel cpu 1` does, if a build ever needs it).

The glass is held **landscape** and the picture is turned to suit
(`s.lcd.rotation`, shipped at 270, all four turns offered on System → Display).
An RGB panel has no rotation of its own, so spangap-lcd transposes each rendered
strip into the framebuffer; the timings above are the glass and never turn.

### The one I2C bus (SDA 15 / SCL 7, port 0)

| Chip | Address | Reached by |
|---|---|---|
| PCA9554 IO expander | 0x20 | this board |
| GT911 touch (INT on GPIO 16, reset on expander P1) | 0x5D or 0x14 | spangap-lcd |
| QMI8658 IMU | 0x6B | imu |
| PCF85063 clock | 0x51 | spangap-rtc |

The board creates this bus on a **named** controller (0) in `onStart`; the three
straddles above adopt it by number rather than putting a second master on the
same two wires.

### PCA9554 expander lines

| Line | Direction | Use |
|---|---|---|
| P0 | out | ST7701S reset |
| P1 | out | GT911 reset |
| P2 | out | ST7701S 3-wire chip-select |
| P3 | out | microSD chip-select — asserted once, then left asserted |
| P4 / P5 | in | QMI8658 INT1 / INT2 — unread |
| P6 | in | PCF85063 interrupt — unread |
| P7 | out | buzzer — held silent |

### microSD (SPI host 2)

| Signal | GPIO |
|---|---|
| SCK / MOSI / MISO | 2 / 1 / 42 |
| CS | expander P3 (`CONFIG_SPANGAP_SDCARD_SPI_PIN_CS=-1`) |

**The select is asserted once and never released.** Toggling it per transaction
would put an I2C round trip inside every SPI transfer. The card is the only
device left on that bus once the panel is configured, which is the same
electrical situation as a single-device slot with its select tied low — and the
board asserts it *after* the panel sequence, so the card never hears the traffic
that was addressed to the panel.

### Battery

VBAT through a 200k/100k divider (÷3) into GPIO 4 (ADC1), no enable gate.

**There is no state of charge to ask for.** The charger on this board is a
fixed-function part with no register interface — no gauge, no charging flag — so
this divider is the whole of what the device knows about its battery, and
`battery.percent` is a voltage put through a discharge curve rather than a
measurement of anything.

### Present but unwired

- **Buzzer** (expander P7) — the platform has no engine for one; the line is
  driven low and left there.
- **The three interrupt lines** on the expander (IMU INT1/INT2, RTC) — reading
  one means an I2C transaction, so nothing polls them. The IMU straddle polls
  the part's own latched status instead, which is what clears it anyway, and
  nothing here wants an RTC alarm.
- **RTC battery header** — a coin cell there keeps the PCF85063 running across a
  power cut, which is what makes the clock survive one. Nothing in software
  needs to know whether it is fitted; the chip's own integrity flag says whether
  the time can be trusted.

### Memory / flash (published from `kconfig:`)

| Symbol | Value | Why |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | `y` | the real chip |
| `CONFIG_SPIRAM_MODE_OCT` | `y` | not a detail: the panel reads a 600 KB framebuffer ~37 times a second, which quad PSRAM cannot feed |
| `CONFIG_SPANGAP_MAX_FIRMWARE_KB` | `8192` | where `/state` begins; the rest of the chip is `/state`. Changing it relocates `/state` → a factory reset on the next boot |

## Storage variables

| Key | Written by | Meaning |
|---|---|---|
| `sys.board` | `Waveshare28bBoard::onInit` | the board's display name |
| `sys.hw` | spangap-core, from `detect_hw` | `hw-waveshare-28b` |
| `battery.millivolt` | `Waveshare28bBattery` | VBAT in mV, smoothed |
| `battery.percent` | `Waveshare28bBattery` | 0..100 from the discharge curve |

Everything else this board shows — `lcd.touch`, `rtc.*`, `imu.*` — is published
by the straddle that owns the part.

## Dependencies

`spangap-core` (hard), and the three soft installs `spangap-lcd`, `spangap-rtc`
and `imu`, each droppable with `--without` (or `--no-lcd`), which takes its
gated `kconfig:` block with it.

## Read next

- [INTERNALS.md](INTERNALS.md) — why the bring-up order is what it is, the
  panel channel, and the pitfalls.
- [spangap-lcd](../spangap-lcd) — the RGB transport, the touch controller and
  the UI zoom this board sets to 150%.
