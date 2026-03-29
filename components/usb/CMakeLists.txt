# Stub component: the "usb" host component was removed from ESP-IDF v6.0 and
# moved to the component registry (espressif/usb), but that registry component
# does not support the plain ESP32 target.  arduino-esp32 unconditionally lists
# "usb" in PRIV_REQUIRES, so we provide an empty interface library to satisfy
# the dependency until arduino-esp32 adds IDF 6.0 support.

idf_component_register()
