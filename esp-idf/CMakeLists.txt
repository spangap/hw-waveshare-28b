# hw-waveshare-28b board HAL — an ordinary spangap component (not a buildable).
#
# Compiles the Waveshare ESP32-S3-Touch-LCD-2.8B board code: the shared I2C bus,
# the PCA9554 expander, the ST7701S register sequence and the battery monitor
# (waveshare28b.cpp). The RGB panel, the GT911 touch, the PCF85063, the QMI8658
# and the microSD are the generic straddles' — this board only supplies their
# pins, in straddle.yaml.
#
# app_main is NOT here — it's generated into the reticulous/reticulous buildable
# that adds this board with `--with`. This component just exports the board's
# Service classes that the generated dispatcher constructs and walks.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        "src/waveshare28b.cpp"
        # This board's one self-assertion (detect_hw), called by
        # spangapInit() before any bus is claimed.
        "src/detect.cpp"
        # The generated services trampoline rides this list, and so would any
        # conditional/<straddle>/ board code — this board has none of the
        # latter, but the former is not optional.
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_adc          # battery voltage ADC on GPIO 4
        esp_timer
)
