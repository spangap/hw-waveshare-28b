# hw-waveshare-28b — INTERNALS

```
Waveshare28bBoard::onStart          start band, before spangapInit(), one task
 ↓
boardBusInit()   i2c_new_master_bus on port 0, SDA 15 / SCL 7
                 + the PCA9554 as a device at 0x20
 ↓ bus failed → return; nothing below can work, and everything says so on its own
exioInit()       output register first, THEN the direction register
                 resets pulsed low 20 ms, released, 50 ms settle
 ↓
panelInit()      GPIO 1/2 as outputs; kSt7701Init walked as
                 <count> <cmd> <params...>, each command framed by expander P2
 ↓
sdSelect()       expander P3 low, and it stays low for the life of the device
 ↓
spangapInit()    fs_mount_sd() takes GPIO 1/2/42 for SPI host 2
```

The single fact this file exists to state: **the four steps above are ordered by
what owns which pins, not by taste, and each one is the last moment its work is
possible.**

## 1. Bring-up: why this order

- **The bus before anything.** The expander is on it, and so are the three
  chips that other straddles reach later by adopting the bus this call makes
  (`CONFIG_LCD_TOUCH_I2C_ADOPT`, `CONFIG_RTC_I2C_PORT`, `CONFIG_IMU_I2C_PORT`).
  It is created on a *named* controller because those three name it by number.
- **The expander before the panel and the touch.** Both resets are its lines,
  and a PCA9554 powers up with every line an input — so until it is written,
  the panel and the touch controller sit at whatever their pull resistors give
  them. It is also where both chip-selects are.
- **The panel before the SD card.** The ST7701S's configuration channel is
  GPIO 1 and GPIO 2, which are the microSD's MOSI and SCK. The panel speaks
  once, here; the card speaks from `spangapInit()` onwards. Between the two
  there is exactly one moment when those pins are plain GPIOs, and this is it.
- **The SD select last.** It is asserted after the panel is finished, so the
  card never sees the panel's traffic — a card listening to a nine-bit
  bit-banged register table would be reading a long burst of nonsense
  addressed to somebody else.

`onStart` returns early if the bus does not open. Nothing downstream needs a
flag for that: each adopting straddle discovers there is no bus on its port and
says so in its own words, and the panel and card fail the way an absent panel
and an absent card fail.

## 2. The expander: write outputs, then directions

`exioInit` writes the output register **before** the configuration register.
Reversed, each line would spend the gap between the two writes as an output
driving whatever the output register happened to hold — which for a reset line
means an unasked-for reset, and for a chip-select means a device selected at
random.

`exioSet` read-modify-writes against `s_exioOut`, our own copy of the output
register, and never against the chip. The PCA9554's *input* register reports the
PINS, not the latch: a line driven by something else reads back as what that
something else is doing, and a read-modify-write through it would latch that
value into our own outputs.

## 3. The panel channel: nine bits, bit-banged, and slow on purpose

The ST7701S here is wired for 3-wire SPI: clock, data, chip-select, **no
data/command pin**. The D/C bit is therefore the first bit of a nine-bit frame,
MSB first, sampled on the rising edge (mode 0). `lcdSpiFrame` is that, at about
250 kHz.

It is bit-banged rather than handed to an SPI peripheral because it runs once,
for about two hundred bytes, on pins that belong to the SD card for the rest of
the device's life: claiming a host and releasing it again is more code and more
ways to go wrong than the loop.

The chip-select is on the expander, so each `lcdCmd` costs two I2C transactions
on top of ~20 µs of clocking — call it 400 µs a command, 20 ms for the table.
That is the price of a select nobody can toggle quickly, and it is paid once.

`kSt7701Init` is `<param count> <command> <params...>`, with a count of
`ST_DELAY` meaning "the next byte is a wait in milliseconds". The table is the
panel vendor's, verbatim, and is **not derivable**: it is the specific glass
Waveshare fitted, not the ST7701S in general. Its shape is a series of
`0xFF 0x77 0x01 0x00 0x00 0x1x` bank selects with each bank's settings between
them; the tail (`0x36`, `0x3A`, `0x29`) is the standard prologue any driver
appends after such a list, and is ours rather than the vendor's.

## 4. The microSD's select is a wiring decision, not a driver setting

`CONFIG_SPANGAP_SDCARD_SPI_PIN_CS=-1` tells spangap-core's sdspi host to drive
no select line at all, and the board holds the card selected from `onStart`
onward. This is sound because the card is the only device left on that bus, but
it is worth knowing what it gives up: the SD specification's power-up sequence
expects a number of clocks with the select **de**asserted, which no driver can
produce here. Cards in practice enter SPI mode on the first CMD0 regardless. If
a particular card refuses to mount, that is the reason, and the fallback is to
drop the slot (`--kconfig CONFIG_SPANGAP_SDCARD=n`) rather than to invent a
per-transaction expander toggle.

## 5. Everything else is Kconfig VALUES, not sources

The display, the touch controller, the clock, the motion sensor and the card
are all generic straddles. This board contributes their pins in `straddle.yaml`
and no C code, which is why `esp-idf/src/` holds two files. When something
about one of those parts is wrong, the fix is nearly always a number in
`straddle.yaml` rather than a line here.

The gated blocks (`when: spangap/…`) exist because those `CONFIG_*` symbols are
DEFINED by the straddle that owns the part: applying them in a build that
dropped the straddle would warn as unknown symbols.

## 6. Battery: divider ratio and the OCV curve

`BAT_DIV_NUM/DEN` is 3/1, Waveshare's 200k/100k pair. The reading is 16 ADC
samples averaged, converted through the SoC's curve-fitting calibration where
that is available and a nominal 3100 mV full-scale where it is not, then put
through a light EMA (weight 3:1) so a status-bar icon does not flicker on noise.
`s_ocvMv` is a single lithium cell's open-circuit voltage at each 10%, linearly
interpolated between the decade points.

None of that is a measurement of charge. There is no gauge on this board and no
register interface on its charger, so "percent" is a voltage with a story
attached — good enough to show, not good enough to schedule against.

## 7. Pitfalls

- **Nothing may touch GPIO 1 or 2 between `panelInit()` and `fs_mount_sd()`.**
  They are the panel's configuration channel and then the card's bus, with no
  overlap and no third claimant. A straddle that took an SPI host on those pins
  in its own `onStart` would break whichever of the two ran after it.
- **The panel is configured long before it is refreshed.** `onStart` writes the
  registers; spangap-lcd starts the timing generator in the init band. Between
  those two the glass is on and receiving no pixels, which is why the backlight
  starts at zero and lcd.cpp raises it after the first frame.
- **The landscape turn is paid for in software.** The SoC scans the framebuffer
  out in the order the glass reads it, so an RGB panel has no rotation to ask
  for: a quarter turn makes spangap-lcd transpose every rendered strip into the
  framebuffer (`lcdPanelBlitRgb`), one extra pass over each repainted pixel and
  one draw buffer of PSRAM. The panel timings stay native — they describe the
  glass, not the picture. 0 and 180 are the free turns, and they are the right
  choice for a build that wants the refresh back. `CONFIG_LCD_ROTATION_270` here
  is only the shipped value of `s.lcd.rotation`; the operator owns it.
- **The GT911's reset is the board's, and it is a sequence, not a pulse.** The
  part reads its INT pin (GPIO 16) as it leaves reset to choose between its two
  I2C addresses, and wants that pin driven across the edge and held for 50 ms
  after it. `exioInit` drives it low — which selects 0x5D — and hands it back as
  an input before spangap-lcd claims it as the touch interrupt. Left floating,
  the part still answers the bus at whichever address it guessed, so the failure
  shows up not as a missing controller but as one that never reports a finger.
- **A starved RGB panel does not glitch, it slides.** The DMA refreshing this
  glass must deliver two bytes per pixel clock — 32 MB/s during an active line —
  out of the same PSRAM every repaint writes to, and data it misses is not a
  dropped frame: the picture walks down the screen and stays where it walked.
  `CONFIG_LCD_RGB_BOUNCE_LINES=10` is what prevents it, and it was measured
  rather than assumed — the walk comes back, repeatably, with bouncing off.
  Note what does NOT help, because both look like they should: a lower frame
  rate (the demand inside a line is set by the pixel clock, and blanking only
  adds recovery time between lines) and `CONFIG_LCD_RGB_RESTART_IN_VSYNC`, which
  re-aligns the frame from an interrupt whose latency then lands in the picture
  as a few pixels of offset that move.
- **A flash write jerks the picture, once, and it heals itself.** Writing flash
  means the cache goes down — NOR flash cannot serve a read from any address
  while it is programming or erasing, and every instruction fetch is such a read
  — and on this SoC the same cache fronts PSRAM. So for the 30-45 ms of a sector
  erase the CPU cannot reach the framebuffer, the bounce refill does not happen,
  and the glass shows a few lines of whatever was left in the buffer. Storage
  commits do it; an OTA does it continuously for as long as it runs.

  What makes it a blemish rather than a fault is the bouncing: the DMA still
  gets its bytes on time, from a buffer nobody refilled, so the frame TIMING
  never slips and the next frame is correct. Without bounce buffers the same
  window costs the DMA real bytes, the frame comes up short, and the picture
  slides permanently. Same cause, and the difference between a flicker and a
  broken screen.

  The cure, if a build ever needs the glass spotless through an update, is
  `CONFIG_SPIRAM_XIP_FROM_PSRAM` (code and constants off flash, so the cache is
  never disabled) plus `CONFIG_LCD_RGB_ISR_IRAM_SAFE` — which is also the only
  arrangement in which IRAM-safety is legal here, since an IRAM ISR that reaches
  for a framebuffer in PSRAM with the cache down panics outright. It costs 1-2 MB
  of PSRAM and a slower boot, which is a poor trade against a millisecond of
  smear on a device that writes flash rarely.
- **The pixel clock has a floor of about 9 MHz.** Below it this panel stops
  locking and stretches a slice of the frame across the glass — 6 MHz shows a
  band of the middle, enlarged. The useful window measured on this unit is
  9–20 MHz; 16 is the shipped value.
- **The IMU's interrupts are on the expander, so there are none.**
  `CONFIG_IMU_INT_PIN=-1` is correct, not a stub: the straddle polls the part's
  latched status once a second, and the read is what clears the latch, so
  polling cannot miss an event — only be up to a second late about it.
- **`detect_hw` writes the expander.** It has to: the touch controller's reset
  is one of its lines and the chip powers up holding nothing. The write is the
  same one `onStart` makes moments later, which is what makes it a scratch
  write in the sense `detect_probe.h` means.
