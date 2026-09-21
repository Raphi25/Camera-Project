idf_component_register(
    SRCS "rtc.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver fatfs sdmmc esp_timer esp_hw_support esp_driver_i2c esp_driver_sdmmc esp_driver_usb_serial_jtag
)
