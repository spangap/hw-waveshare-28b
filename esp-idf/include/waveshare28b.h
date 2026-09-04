/**
 * waveshare28b.h — Waveshare ESP32-S3-Touch-LCD-2.8B board HAL.
 *
 * What this module provides:
 *   - Compile-time hardware constants for the board's bespoke parts: the one
 *     I2C bus everything hangs off, the PCA9554 IO expander and what each of
 *     its eight lines does, the ST7701S's 3-wire init channel, and the battery
 *     sense. Consumed by waveshare28b.cpp and by detect.cpp.
 *   - The board bring-up Services (Waveshare28bBoard / Waveshare28bBattery).
 *
 * NOT here, because they are other straddles' from pins this board supplies in
 * straddle.yaml: the RGB panel and its backlight (spangap-lcd's
 * CONFIG_LCD_BUS_RGB block), the GT911 touch (CONFIG_LCD_TOUCH_*), the
 * PCF85063 clock (spangap-rtc's CONFIG_RTC_*), the QMI8658 (spangap/imu's
 * CONFIG_IMU_*), and the microSD (spangap-core's CONFIG_SPANGAP_SDCARD_*).
 * What this board DOES own is everything those parts need to exist before they
 * are reached: the bus, the expander, and the panel's own register sequence.
 */
#pragma once

#include "service.h"

/** The board's display name, published as sys.board. */
#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-2.8B"

/* ── the one I2C bus ─────────────────────────────────────────────────────────
 * SDA 15 / SCL 7 carry the IO expander, the GT911 touch controller, the
 * QMI8658 and the PCF85063, and they are the two pins Waveshare brings out on
 * the board's I2C header. Every RGB data line and both sync pins are spoken
 * for, so there is no second bus to move anything onto: the board creates this
 * one in onStart and the three straddles adopt it by port number. */
#define BOARD_I2C_PORT      0
#define BOARD_I2C_SDA_PIN   15
#define BOARD_I2C_SCL_PIN   7

/* ── PCA9554 IO expander (0x20) ──────────────────────────────────────────────
 * Eight lines, all used, none brought out. Waveshare's documentation numbers
 * them EXIO1..EXIO8; the chip's own registers number the same lines P0..P7, and
 * these are the chip's numbers. The three resets and the two chip-selects on it
 * are the reason this board needs bring-up code at all — none of them can be
 * reached by a driver that only knows about GPIOs. */
#define BOARD_EXIO_ADDR     0x20
#define EXIO_LCD_RST        0    /* ST7701S reset */
#define EXIO_TP_RST         1    /* GT911 reset */
#define EXIO_LCD_CS         2    /* ST7701S 3-wire chip-select */
#define EXIO_SD_CS          3    /* microSD chip-select — see waveshare28b.cpp */
#define EXIO_IMU_INT1       4    /* QMI8658 INT1 (input, unread) */
#define EXIO_IMU_INT2       5    /* QMI8658 INT2 (input, unread) */
#define EXIO_RTC_INT        6    /* PCF85063 interrupt (input, unread) */
#define EXIO_BUZZER         7    /* buzzer drive — the platform has no engine for one */

/* PCA9554 registers. Configuration is 1 = input, 0 = output. */
#define EXIO_REG_INPUT      0x00
#define EXIO_REG_OUTPUT     0x01
#define EXIO_REG_POLARITY   0x02
#define EXIO_REG_CONFIG     0x03

/* ── ST7701S init channel ────────────────────────────────────────────────────
 * The panel controller's registers are written over a 3-wire SPI — clock, data
 * and a chip-select, no data/command pin, so every frame is NINE bits with the
 * D/C bit first. Its two wires are the microSD's clock and MOSI: the board can
 * do that because the panel is configured exactly once, at onStart, and never
 * addressed again, while the card is not mounted until spangapInit() runs after
 * it. Nothing may touch these pins between those two points. */
#define BOARD_LCD_SDA_PIN   1    /* also the microSD's MOSI */
#define BOARD_LCD_SCL_PIN   2    /* also the microSD's SCK */

/* ── battery sense ───────────────────────────────────────────────────────────
 * VBAT through a 200k/100k divider (÷3) into GPIO 4 (ADC1). The divider has no
 * enable gate, so the pin always carries the divided VBAT and needs no power-up
 * step. Trim BAT_DIV_NUM/DEN in waveshare28b.cpp if a multimeter disagrees. */
#define BOARD_BAT_ADC       4

/**
 * Board bring-up (onStart): the I2C bus, the IO expander and its five driven
 * lines, the ST7701S register sequence, and the microSD chip-select. Runs
 * BEFORE spangapInit(), which is what the SD mount and every adopting straddle
 * depend on. Its onInit publishes sys.board, which needs storage.
 */
class Waveshare28bBoard : public Service {
public:
    void onStart() override;
    void onInit() override;
};

/**
 * Battery monitor bring-up (onInit): configures the GPIO4 ADC, publishes an
 * initial battery.millivolt / battery.percent, and arms a once-a-minute
 * esp_timer to keep them fresh. init band (needs storage up). No task of its
 * own — the periodic timer callback does the sampling.
 */
class Waveshare28bBattery : public Service {
public:
    void onInit() override;
};
