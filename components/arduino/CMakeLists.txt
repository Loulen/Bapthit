# Thin alias component: makes the managed `espressif/arduino-esp32` component
# available under the plain name "arduino" so that downstream components
# (e.g. PunchMeter) can use `REQUIRES arduino` without knowing the registry name.

idf_component_register(REQUIRES espressif__arduino-esp32)
