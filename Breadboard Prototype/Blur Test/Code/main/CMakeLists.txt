idf_component_register(SRCS "main.c" "capture.c" "servo.c" "storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp_video esp_driver_jpeg esp_driver_ledc esp_driver_gpio
             esp_tinyusb esp_hw_support esp_timer fatfs sdmmc esp_driver_sdmmc)
