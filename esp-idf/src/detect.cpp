/**
 * detect.cpp — is the hardware under this firmware a Waveshare
 * ESP32-S3-Touch-LCD-2.8B?
 *
 * The board's one self-assertion. It answers about THIS board only: its own
 * name when the four parts on its I2C bus answer where they should, and NULL
 * otherwise. Nothing here enumerates other boards — that comparison belongs to
 * whoever calls it.
 *
 * Two callers, one body:
 *
 *   * spangap-core, at the top of spangapInit(), before any bus is claimed. A
 *     board straddle is staged because the image was built for that board, so a
 *     NULL here means the image is on the wrong hardware and the platform halts
 *     rather than driving someone else's pins for the rest of the boot.
 *   * flashmon's standalone detector, which carries a copy of this function
 *     renamed `detect_hw_waveshare_28b` and calls it alongside every other
 *     board's, to identify a chip whose firmware is unknown.
 *
 * The copy is manual and deliberately so — see detect_probe.h. Change this,
 * change flashmon/esp-idf/main/detect.c.
 *
 * This probe WRITES the expander, which is the one thing here that is not a
 * read: the touch controller's reset is one of its lines, and the chip comes
 * out of power-on with every line an input, so without releasing that reset the
 * GT911 has nothing to say. The write is exactly what the board's own onStart
 * does moments later, so it is a scratch write in the sense detect_probe.h
 * means: the real firmware overwrites it with the same value.
 *
 * No rails to drive: everything on this board is powered as soon as the board
 * is.
 */
#include "detect_probe.h"
#include "waveshare28b.h"

/* The RTC and the IMU live on the same two wires as the expander and the touch
 * controller. Their addresses are in straddle.yaml as CONFIG_RTC_* /
 * CONFIG_IMU_* — but those symbols only exist when those straddles are staged,
 * and this probe has to work in an image built without either, so they are
 * written out here. */
#define DETECT_RTC_ADDR   0x51   /* PCF85063 */
#define DETECT_IMU_ADDR   0x6B   /* QMI8658, SA0 high */
#define DETECT_IMU_WHOAMI 0x00
#define DETECT_IMU_ID     0x05

extern "C" const char* detect_hw(void)
{
    /* 16 MB flash or it is not a 2.8B — cheapest possible rejection, and it
     * touches no pin at all. */
    if (!detect_flash_mb(16)) return NULL;

    detect_i2c_t h;
    if (!detect_i2c_open(&h, BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN)) return NULL;

    /* Anchor: the IO expander. Nothing else in this workspace puts one at 0x20
     * on these two pins, and on this board every reset and both chip-selects
     * hang off it. */
    if (!detect_i2c_ack(&h, BOARD_EXIO_ADDR)) {
        detect_i2c_close(&h);
        detect_miss("no IO expander at 0x%02X — not a Waveshare 2.8B", BOARD_EXIO_ADDR);
        return NULL;
    }

    /* Release the two resets it holds, so the touch controller can answer
     * below. Outputs high, the three interrupt lines left as inputs. */
    uint8_t out = (uint8_t)((1u << EXIO_LCD_RST) | (1u << EXIO_TP_RST) |
                            (1u << EXIO_LCD_CS)  | (1u << EXIO_SD_CS));
    uint8_t cfg = (uint8_t)((1u << EXIO_IMU_INT1) | (1u << EXIO_IMU_INT2) |
                            (1u << EXIO_RTC_INT));
    detect_i2c_wr(&h, BOARD_EXIO_ADDR, EXIO_REG_OUTPUT, out);
    detect_i2c_wr(&h, BOARD_EXIO_ADDR, EXIO_REG_CONFIG, cfg);
    vTaskDelay(pdMS_TO_TICKS(60));      /* the GT911 boots its own firmware */

    /* The other two on these wires, together: an RTC that only ACKs, and an
     * accelerometer that names itself. */
    bool rtc = detect_i2c_ack(&h, DETECT_RTC_ADDR);
    uint8_t who = 0;
    bool imu = detect_i2c_rd(&h, DETECT_IMU_ADDR, DETECT_IMU_WHOAMI, &who, 1) &&
               who == DETECT_IMU_ID;
    detect_i2c_close(&h);

    if (!rtc || !imu) {
        detect_miss("expander answered but rtc=%d imu=%d — not a Waveshare 2.8B",
                    rtc, imu);
        return NULL;
    }

    /* Confirm with the glass: a GT911 that reports its "911" product ID on
     * these pins. This is the part that separates the touch board from the
     * bare-LCD variant of the same PCB. */
    if (!detect_gt911(BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN)) {
        detect_miss("no GT911 touch — not a Waveshare 2.8B (the -Touch- variant)");
        return NULL;
    }

    detect_found("hw_waveshare_28b");
    return "hw-waveshare-28b";
}
