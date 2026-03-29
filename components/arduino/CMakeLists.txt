idf_component_register(
    SRCS "Arduino.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_timer freertos
)
