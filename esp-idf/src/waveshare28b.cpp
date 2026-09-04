/**
 * waveshare28b.cpp — Waveshare ESP32-S3-Touch-LCD-2.8B board support, end to
 * end.
 *
 * Single owner of all 2.8B hardware bring-up. See waveshare28b.h for the pin
 * map and README.md for the hardware reference. Layout:
 *
 *   1. The board I2C bus and the PCA9554 expander.
 *   2. The ST7701S panel's register sequence, over its 3-wire SPI.
 *   3. The microSD chip-select.
 *   4. Battery monitor (Waveshare28bBattery).
 *
 * 1-3 all run in Waveshare28bBoard::onStart, in that order, and the order is
 * the whole design of this file:
 *
 *   the bus         → because the expander is on it, and so are the three
 *                     straddles that will adopt it (touch, RTC, IMU)
 *   the expander    → because both resets and both chip-selects are on it
 *   the panel       → while its clock and data lines are still plain GPIOs
 *   the SD select   → after the panel is done with those same two wires, and
 *                     before spangapInit()'s fs_mount_sd claims them
 *
 * There is no power rail to drive: everything on this board is on as soon as
 * the board is.
 */
#include "waveshare28b.h"

#include "i2c_helper.h"     /* SPANGAP_I2C_PULLUP (shared bus wiring policy) */
#include "log.h"
#include "storage.h"        /* battery.* ephemerals, sys.board */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

/* =========================================================================
 * 1. The board I2C bus and the PCA9554 expander
 *
 * The bus is created on a NAMED controller (BOARD_I2C_PORT) rather than an
 * auto-allocated one, because three other straddles adopt it by number:
 * spangap-lcd's touch (CONFIG_LCD_TOUCH_I2C_ADOPT), spangap-rtc
 * (CONFIG_RTC_I2C_PORT) and spangap/imu (CONFIG_IMU_I2C_PORT). Everything on
 * this board that is not a display data line is on these two wires.
 *
 * The expander is what makes the board work at all: three resets and two
 * chip-selects that no driver can reach, because a driver knows about GPIOs
 * and these are register bits behind an I2C transaction. It comes out of
 * power-on with every line an input, so until this runs, the panel and the
 * touch controller are held in whatever state their pull resistors give them.
 * ========================================================================= */

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_exio = nullptr;
/* The expander's output register, as we last wrote it. Read-modify-write
 * against this rather than against the chip: the input register reflects the
 * PINS, so a line held by something else reads back as what that something
 * else is doing and a read-modify-write would latch it. */
static uint8_t s_exioOut = 0;

static bool exioWrite(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return s_exio && i2c_master_transmit(s_exio, buf, sizeof(buf), 100) == ESP_OK;
}

/* One expander line. Slow on purpose to read: every call is an I2C
 * transaction, which is exactly why nothing time-critical is on this chip. */
static void exioSet(int bit, bool high)
{
    uint8_t next = high ? (uint8_t)(s_exioOut | (1u << bit))
                        : (uint8_t)(s_exioOut & ~(1u << bit));
    if (exioWrite(EXIO_REG_OUTPUT, next)) s_exioOut = next;
}

static bool boardBusInit(void)
{
    i2c_master_bus_config_t bus = {};
    bus.i2c_port          = BOARD_I2C_PORT;
    bus.sda_io_num        = (gpio_num_t)BOARD_I2C_SDA_PIN;
    bus.scl_io_num        = (gpio_num_t)BOARD_I2C_SCL_PIN;
    bus.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = SPANGAP_I2C_PULLUP;
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
        err("board i2c bus would not open (SDA=%d SCL=%d)\n",
            BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
        s_bus = nullptr;
        return false;
    }

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = BOARD_EXIO_ADDR;
    dev.scl_speed_hz    = 400000;
    if (i2c_master_bus_add_device(s_bus, &dev, &s_exio) != ESP_OK) {
        err("no PCA9554 at 0x%02X\n", BOARD_EXIO_ADDR);
        s_exio = nullptr;
        return false;
    }
    return true;
}

/* Every driven line to its resting state in one write, then the direction
 * register — in that order, so no line is ever an output holding whatever the
 * output register happened to contain.
 *
 * Resting state: both resets released (high), the panel deselected (high), the
 * card deselected (high — it stays that way until the panel is configured, see
 * below) and the buzzer silent (low). The three interrupt lines stay inputs:
 * nothing here reads them (see the IMU note in README.md). */
static void exioInit(void)
{
    s_exioOut = (uint8_t)((1u << EXIO_LCD_RST) | (1u << EXIO_TP_RST) |
                          (1u << EXIO_LCD_CS)  | (1u << EXIO_SD_CS));
    exioWrite(EXIO_REG_OUTPUT, s_exioOut);
    exioWrite(EXIO_REG_POLARITY, 0x00);
    exioWrite(EXIO_REG_CONFIG,
              (uint8_t)((1u << EXIO_IMU_INT1) | (1u << EXIO_IMU_INT2) |
                        (1u << EXIO_RTC_INT)));

    /* Reset both parts that have a reset line here. The GT911 samples its INT
     * pin as it comes out of reset to choose between its two I2C addresses;
     * nothing drives that pin at this point, and spangap-lcd's touch bring-up
     * tries both addresses anyway, so the pulse is all that is needed. */
    exioSet(EXIO_LCD_RST, false);
    exioSet(EXIO_TP_RST,  false);
    vTaskDelay(pdMS_TO_TICKS(20));
    exioSet(EXIO_LCD_RST, true);
    exioSet(EXIO_TP_RST,  true);
    vTaskDelay(pdMS_TO_TICKS(50));      /* the ST7701S wants ~50 ms after reset */
}

/* =========================================================================
 * 2. The ST7701S panel's register sequence
 *
 * An RGB panel has no memory and no command channel once it is running — the
 * SoC simply refreshes it forever (spangap-lcd's lcd_panel_rgb.cpp). But
 * before that, its controller has to be told its gamma curves, its power
 * settings, its gate timing and its interface mode, and that conversation
 * happens over a 3-wire SPI: clock, data, chip-select, and no data/command
 * pin — so each frame is NINE bits, D/C first, MSB first, sampled on the
 * rising edge.
 *
 * It is bit-banged, at a few hundred kHz, because it happens once for about
 * two hundred bytes and because its two wires belong to the microSD for the
 * whole of the rest of the device's life. Handing them to the SPI peripheral
 * and taking them back would cost more code than the loop below.
 *
 * The sequence itself is the panel vendor's, verbatim, and is not derivable
 * from anything: it is the specific glass Waveshare fitted, not the ST7701S in
 * general. Its shape is a series of CND2BKxSEL writes (0xFF ... 0x1x) that
 * page the register bank, with the settings for each bank between them.
 * ========================================================================= */

/* One nine-bit frame. CS is held by the caller across a whole command. */
static void lcdSpiFrame(bool data, uint8_t val)
{
    uint16_t bits = (uint16_t)((data ? 0x100 : 0x000) | val);
    for (int i = 8; i >= 0; i--) {
        gpio_set_level((gpio_num_t)BOARD_LCD_SCL_PIN, 0);
        gpio_set_level((gpio_num_t)BOARD_LCD_SDA_PIN, (bits >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level((gpio_num_t)BOARD_LCD_SCL_PIN, 1);
        esp_rom_delay_us(1);
    }
}

/* A command and its parameters, framed by one chip-select. The select is on
 * the expander, so each of these costs two I2C transactions on top of the
 * ~20 µs of bit-banging — around 400 µs a command, 20 ms for the table. */
static void lcdCmd(uint8_t cmd, const uint8_t* params, int n)
{
    exioSet(EXIO_LCD_CS, false);
    lcdSpiFrame(false, cmd);
    for (int i = 0; i < n; i++) lcdSpiFrame(true, params[i]);
    exioSet(EXIO_LCD_CS, true);
}

/* The table, as <param count> <command> <parameters...>. A count of ST_DELAY
 * is not a command: the byte after it is a wait in milliseconds. */
#define ST_DELAY 0xFF
static const uint8_t kSt7701Init[] = {
    /* bank 13: the vendor's private page */
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13,
    1, 0xEF, 0x08,
    /* bank 10: driver, panel and gamma */
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x10,
    2, 0xC0, 0x4F, 0x00,
    2, 0xC1, 0x10, 0x02,
    2, 0xC2, 0x07, 0x02,
    1, 0xCC, 0x10,
    16, 0xB0, 0x00, 0x10, 0x17, 0x0D, 0x11, 0x06, 0x05, 0x08,
                0x07, 0x1F, 0x04, 0x11, 0x0E, 0x29, 0x30, 0x1F,
    16, 0xB1, 0x00, 0x0D, 0x14, 0x0E, 0x11, 0x06, 0x04, 0x08,
                0x08, 0x20, 0x05, 0x13, 0x13, 0x26, 0x30, 0x1F,
    /* bank 11: power, VCOM and the gate/source timing tables */
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x11,
    1, 0xB0, 0x65,
    1, 0xB1, 0x71,
    1, 0xB2, 0x82,
    1, 0xB3, 0x80,
    1, 0xB5, 0x42,
    1, 0xB7, 0x85,
    1, 0xB8, 0x20,
    1, 0xC0, 0x09,
    1, 0xC1, 0x78,
    1, 0xC2, 0x78,
    1, 0xD0, 0x88,
    1, 0xEE, 0x42,
    3, 0xE0, 0x00, 0x00, 0x02,
    11, 0xE1, 0x04, 0xA0, 0x06, 0xA0, 0x05, 0xA0, 0x07, 0xA0, 0x00, 0x44, 0x44,
    12, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    4, 0xE3, 0x00, 0x00, 0x22, 0x22,
    2, 0xE4, 0x44, 0x44,
    16, 0xE5, 0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0, 0xA0,
                0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0,
    4, 0xE6, 0x00, 0x00, 0x22, 0x22,
    2, 0xE7, 0x44, 0x44,
    16, 0xE8, 0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0, 0xA0,
                0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0,
    7, 0xEB, 0x00, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40,
    16, 0xED, 0xFF, 0xF5, 0x47, 0x6F, 0x0B, 0xA1, 0xAB, 0xFF,
                0xFF, 0xBA, 0x1A, 0xB0, 0xF6, 0x74, 0x5F, 0xFF,
    6, 0xEF, 0x08, 0x08, 0x08, 0x40, 0x3F, 0x64,
    /* back to bank 0, then bank 13 for two more of the vendor's own */
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00,
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13,
    2, 0xE6, 0x16, 0x7C,
    2, 0xE8, 0x00, 0x0E,
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00,
    /* sleep out, then the vendor's two-step release of the gate driver */
    0, 0x11,
    ST_DELAY, 200,
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x13,
    2, 0xE8, 0x00, 0x0C,
    ST_DELAY, 150,
    2, 0xE8, 0x00, 0x00,
    5, 0xFF, 0x77, 0x01, 0x00, 0x00, 0x00,
    1, 0x35, 0x00,                  /* tearing-effect line on */
    /* The standard tail every driver appends after a vendor list: scan
     * direction, pixel format, display on. If the colour channels come out
     * swapped or the image is monochrome, the pixel format is the one of these
     * three to question — the panel is in RGB mode, where 0x3A describes an
     * interface it is not using. */
    1, 0x36, 0x00,                  /* MADCTL: normal scan, RGB order */
    1, 0x3A, 0x50,                  /* COLMOD: 16 bit/pixel */
    0, 0x29,                        /* display on */
};

static void panelInit(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BOARD_LCD_SDA_PIN) | (1ULL << BOARD_LCD_SCL_PIN);
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)BOARD_LCD_SCL_PIN, 1);   /* idle high (mode 0) */

    for (size_t i = 0; i < sizeof(kSt7701Init); ) {
        uint8_t n = kSt7701Init[i++];
        if (n == ST_DELAY) { vTaskDelay(pdMS_TO_TICKS(kSt7701Init[i++])); continue; }
        uint8_t cmd = kSt7701Init[i++];
        lcdCmd(cmd, &kSt7701Init[i], n);
        i += n;
    }
    info("ST7701S configured\n");
}

/* =========================================================================
 * 3. The microSD chip-select
 *
 * The card's select is expander line EXIO_SD_CS, which means it cannot be
 * toggled per transaction — an SPI transfer would spend more time on I2C than
 * on data. So it is asserted ONCE, here, and left asserted: the card is the
 * only device left on that bus (the panel has just finished with it and will
 * never speak again), and a permanently selected card on an otherwise idle bus
 * is exactly the wiring a single-device SPI slot has. spangap-core is told the
 * matching truth with CONFIG_SPANGAP_SDCARD_SPI_PIN_CS=-1, which makes its
 * sdspi host drive no select line at all.
 *
 * Asserted LAST, after the panel: while the panel was being configured these
 * were its clock and data, and a card listening to that would have seen a long
 * burst of nonsense addressed to somebody else.
 * ========================================================================= */

static void sdSelect(void)
{
    exioSet(EXIO_SD_CS, false);
}

/* =========================================================================
 * 4. Battery monitor — VBAT via the GPIO4 divider (see BOARD_BAT_ADC).
 *
 * Always compiled (no UI dependency): a once-a-minute esp_timer samples the ADC
 * and publishes two ephemerals the rest of the system reacts to —
 *   battery.millivolt  — true VBAT in mV (pin reading × divider)
 *   battery.percent    — 0..100, via the open-circuit-voltage curve below
 * spangap-lcd's status bar subscribes to battery.percent for its icon; spangap-
 * core's `bat` CLI command prints both. No dedicated task — the periodic timer
 * callback does the read on the esp_timer task.
 *
 * The charger on this board is a fixed-function part with no register
 * interface: there is no state of charge to ask for and no charging/discharging
 * flag to read, so this divider is the whole of what the device knows about its
 * battery.
 * ========================================================================= */

namespace {

/* Divider ratio 3/1 — Waveshare's 200k/100k pair. Trim NUM/DEN if a multimeter
 * disagrees. */
constexpr uint32_t BAT_DIV_NUM   = 3, BAT_DIV_DEN = 1;
constexpr int      BAT_SAMPLES   = 16;             /* averaged per read — kills ADC jitter */
constexpr int64_t  BAT_PERIOD_US = 60LL * 1000000; /* re-sample cadence: every minute */

/* Open-circuit voltage at 100 %, 90 %, … 0 % for a single lithium cell.
 * Linear interpolation between the decade points; input jitter is smoothed by
 * the per-read averaging + EMA below. */
const uint16_t s_ocvMv[11] = {
    4160, 4020, 3940, 3870, 3810, 3760, 3740, 3720, 3680, 3620, 2990,
};

adc_oneshot_unit_handle_t s_adc      = nullptr;
adc_cali_handle_t         s_adcCali  = nullptr;
adc_unit_t                s_adcUnit  = ADC_UNIT_1;
adc_channel_t             s_adcChan  = ADC_CHANNEL_0;   /* GPIO4; confirmed at init */
bool                      s_adcReady = false;
uint32_t                  s_mvEma    = 0;               /* smoothed VBAT, mV (0 = unset) */

uint8_t batteryPercent(uint16_t mv) {
    if (mv >= s_ocvMv[0])  return 100;
    if (mv <= s_ocvMv[10]) return 0;
    for (int i = 1; i <= 10; i++) {
        if (mv >= s_ocvMv[i]) {
            uint16_t hi = s_ocvMv[i - 1], lo = s_ocvMv[i];
            int pctLo = 100 - i * 10;
            return (uint8_t)(pctLo + (uint32_t)(mv - lo) * 10 / (hi - lo));
        }
    }
    return 0;
}

/* Sample, smooth, publish. Runs on the esp_timer task (and once at init). */
void batteryRead(void*) {
    if (!s_adcReady) return;
    int acc = 0, ok = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_adcChan, &raw) == ESP_OK) { acc += raw; ok++; }
    }
    if (!ok) return;
    int raw = acc / ok;
    int pinMv;
    if (!(s_adcCali && adc_cali_raw_to_voltage(s_adcCali, raw, &pinMv) == ESP_OK))
        pinMv = (int)((int64_t)raw * 3100 / 4095);      /* nominal 12-bit @ 12 dB */
    uint32_t mv = (uint32_t)pinMv * BAT_DIV_NUM / BAT_DIV_DEN;
    /* Light EMA across reads (~3-4 min at the 1/min cadence) so the icon and
     * percent don't wobble on noise; first reading seeds it directly (no lag). */
    s_mvEma = s_mvEma ? (s_mvEma * 3 + mv) / 4 : mv;
    uint16_t outMv = (uint16_t)s_mvEma;
    storageBegin();                                     /* one commit -> subscribers see both */
    storageSet("battery.millivolt", (int)outMv);
    storageSet("battery.percent",   (int)batteryPercent(outMv));
    storageEnd();
}

}  // namespace

/* onInit — ADC bring-up, an initial reading, then the once-a-minute timer.
 * Runs after spangapInit() so storage is up for the ephemeral writes. */
void Waveshare28bBattery::onInit() {
    if (adc_oneshot_io_to_channel(BOARD_BAT_ADC, &s_adcUnit, &s_adcChan) != ESP_OK) {
        warn("battery: GPIO%d is not an ADC pin\n", BOARD_BAT_ADC);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = s_adcUnit;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        warn("battery: adc unit init failed\n");
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten    = ADC_ATTEN_DB_12;        /* ~0..3.1 V pin range; VBAT/3 maxes ~1.4 V */
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(s_adc, s_adcChan, &ccfg) != ESP_OK) {
        warn("battery: adc channel config failed\n");
        return;
    }
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = s_adcUnit;
    cal.chan     = s_adcChan;
    cal.atten    = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_adcCali) != ESP_OK) {
        s_adcCali = nullptr;                /* fall back to nominal raw->mV scaling */
        warn("battery: adc calibration unavailable, using nominal scale\n");
    }
    s_adcReady = true;

    batteryRead(nullptr);                   /* publish an initial reading now */

    const esp_timer_create_args_t targs = { .callback = batteryRead, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "battery", .skip_unhandled_events = true };
    esp_timer_handle_t th = nullptr;
    if (esp_timer_create(&targs, &th) == ESP_OK)
        esp_timer_start_periodic(th, BAT_PERIOD_US);
    else
        warn("battery: timer create failed\n");
}

void Waveshare28bBoard::onStart() {
    if (!boardBusInit()) return;    /* nothing below can work without the bus */
    exioInit();
    panelInit();
    sdSelect();
}

/* onInit — the board says what it is, once storage exists to say it into. Every
 * surface that names the hardware (the Hardware section of Settings, on both
 * the display and the browser) reads this key, so a board is identified in one
 * place rather than by each surface knowing which board it is running on. */
void Waveshare28bBoard::onInit() {
    storageSet("sys.board", BOARD_NAME);
}
