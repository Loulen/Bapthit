---
name: esp-idf-project-scaffold
description: Use when creating a new ESP-IDF project, adding components, setting up partition tables, or structuring an ESP32 firmware codebase from scratch
---

# ESP-IDF Project Scaffold

## Overview

ESP-IDF uses a CMake-based build system with a component architecture. Projects need a specific directory structure, proper CMakeLists.txt files, and partition table configuration.

## Standard Project Structure

```
project_name/
  CMakeLists.txt              # Top-level (required)
  sdkconfig.defaults          # Git-tracked config overrides
  sdkconfig.defaults.esp32    # Target-specific overrides (optional)
  partitions.csv              # Custom partition table
  main/
    CMakeLists.txt            # Main component build file
    idf_component.yml         # Component registry dependencies
    main.c                    # app_main() entry point
    Kconfig.projbuild         # Project-wide menuconfig options (optional)
  components/
    my_component/
      CMakeLists.txt
      include/
        my_component.h        # Public API
      src/
        my_component.c
      Kconfig                 # Component menuconfig options
      idf_component.yml       # Dependencies
  .gitignore
  .vscode/
    settings.json
```

## Scaffolding Commands

```bash
# Create new project from scratch
idf.py create-project project_name

# Create a new component inside a project
idf.py create-component -C components my_component

# Add a dependency from component registry
idf.py add-dependency "espressif/button^3.0"

# Set target chip (run before first build)
idf.py set-target esp32
```

## Essential File Templates

### Top-level CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(project_name)
```

### main/CMakeLists.txt
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
)
```

### Component CMakeLists.txt
```cmake
idf_component_register(
    SRCS "src/my_component.c"
    INCLUDE_DIRS "include"
    REQUIRES driver              # Public deps (propagated)
    PRIV_REQUIRES nvs_flash     # Private deps (not propagated)
)
```

### Minimal main.c
```c
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Application started");
}
```

### .gitignore
```
build/
managed_components/
sdkconfig
sdkconfig.old
```

## sdkconfig Management

**Never commit `sdkconfig`** -- it contains thousands of lines of resolved defaults that vary across IDF versions.

**Commit `sdkconfig.defaults`** -- only your intentional overrides:

```ini
# Flash
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y

# Partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Logging
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# FreeRTOS
CONFIG_FREERTOS_HZ=1000
```

**Target-specific**: Create `sdkconfig.defaults.esp32s3` for ESP32-S3-only settings -- auto-merged when target matches.

## Partition Tables

### Default (no OTA) -- good for development
```csv
# Name,    Type, SubType,  Offset,   Size,     Flags
nvs,       data, nvs,      0x9000,   0x6000,
phy_init,  data, phy,      0xf000,   0x1000,
factory,   app,  factory,  0x10000,  1M,
```

### OTA-capable (production)
```csv
# Name,    Type, SubType,  Offset,   Size,     Flags
nvs,       data, nvs,      0x9000,   0x6000,
otadata,   data, ota,      0xf000,   0x2000,
phy_init,  data, phy,      0x11000,  0x1000,
factory,   app,  factory,  0x20000,  0x1C0000,
ota_0,     app,  ota_0,    0x1E0000, 0x1C0000,
ota_1,     app,  ota_1,    0x3A0000, 0x1C0000,
coredump,  data, coredump, 0x560000, 0x10000,
```

**Rules:**
- `ota_0` and `ota_1` must be same size
- App partitions aligned to 0x10000 (64KB), data to 0x1000 (4KB)
- Total must fit flash chip (4MB = 0x400000)
- Always include `nvs` -- required by Wi-Fi, BLE, many components
- Add `coredump` partition for post-mortem debugging

## Component Architecture Best Practices

- **Keep `main/` thin** -- just `app_main()` init and task creation
- **One component = one responsibility** -- separate drivers, protocols, app logic
- **Use `REQUIRES` sparingly** -- exposes headers transitively; prefer `PRIV_REQUIRES`
- **Use `idf_component.yml`** for external dependencies from the component registry
- **Use `Kconfig`** in components for configurable parameters exposed in menuconfig
