cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# Workaround: arduino-esp32 v3.x does not yet declare IDF 6.0 support.
# Fake the IDF version for the component-manager dependency solver and skip
# the CMake-level version gate inside the arduino component.
# Remove these two lines once arduino-esp32 ships a release for IDF 6.x.
# ---------------------------------------------------------------------------
set(ENV{CI_TESTING_IDF_VERSION} "5.5.0")
set(ENV{ARDUINO_SKIP_IDF_VERSION_CHECK} "1")

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/punchmeter/idf_component"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bapthit)
