---
name: esp-idf-debugging
description: Use when debugging ESP32 firmware crashes, decoding backtraces, setting up JTAG/OpenOCD, analyzing core dumps, or configuring ESP-IDF logging levels
---

# ESP-IDF Debugging

## Overview

ESP-IDF provides layered debugging: serial monitor with automatic crash decode, core dumps for post-mortem analysis, and JTAG for live debugging. Start with monitor, escalate as needed.

## Debugging Escalation

```dot
digraph debug_escalation {
    rankdir=LR;
    "Crash/bug" [shape=box];
    "Monitor output" [shape=box, label="idf.py monitor\n(auto backtrace decode)"];
    "Core dump" [shape=box, label="coredump-debug\n(post-mortem GDB)"];
    "JTAG + GDB" [shape=box, label="Live breakpoints\nstep-through"];

    "Crash/bug" -> "Monitor output" [label="first"];
    "Monitor output" -> "Core dump" [label="need more context"];
    "Core dump" -> "JTAG + GDB" [label="need live debug"];
}
```

## Logging (ESP_LOG)

```c
#include "esp_log.h"

static const char *TAG = "my_module";

ESP_LOGE(TAG, "Error: %s", esp_err_to_name(ret));   // Always shown
ESP_LOGW(TAG, "Warning: retry %d/%d", n, max);       // Important warnings
ESP_LOGI(TAG, "Connected to %s", ssid);               // Key events
ESP_LOGD(TAG, "Buffer: %d bytes", len);               // Dev details
ESP_LOGV(TAG, "Entering %s", __func__);               // Trace
```

**Runtime level control:**
```c
esp_log_level_set("wifi", ESP_LOG_WARN);         // Quiet noisy modules
esp_log_level_set("my_module", ESP_LOG_DEBUG);   // Verbose for yours
esp_log_level_set("*", ESP_LOG_INFO);            // Global default
```

**sdkconfig for logging:**
```ini
# Development
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y

# Production
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y    # Compile out DEBUG/VERBOSE
```

**Best practices:**
- Unique `TAG` per source file
- Always include `esp_err_to_name(ret)` in error logs
- Never log secrets, even at DEBUG level
- Use `ESP_LOG_BUFFER_HEX_LEVEL()` for binary data

## Error Handling Patterns

**Preferred pattern (ESP-IDF v5+):**
```c
#include "esp_check.h"

esp_err_t my_init(config_t *cfg)
{
    ESP_RETURN_ON_ERROR(validate(cfg), TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(cfg->timeout > 0, ESP_ERR_INVALID_ARG,
                        TAG, "Bad timeout: %d", cfg->timeout);
    return ESP_OK;
}
```

**With cleanup:**
```c
esp_err_t init(void)
{
    esp_err_t ret = ESP_OK;
    resource_t *r = NULL;

    r = allocate();
    ESP_GOTO_ON_FALSE(r, ESP_ERR_NO_MEM, cleanup, TAG, "Alloc failed");
    ESP_GOTO_ON_ERROR(configure(r), cleanup, TAG, "Config failed");
    return ESP_OK;

cleanup:
    free(r);
    return ret;
}
```

**`ESP_ERROR_CHECK()`** -- aborts on error. Use only for truly fatal init failures in `app_main()`.

## Monitor Crash Decoding

`idf.py monitor` automatically decodes panic output:

```
Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.
Core 0 register dump:
...
Backtrace: 0x400d1234:0x3ffb5e60 0x400d5678:0x3ffb5e80

Decoded:
  0x400d1234: my_function at /path/to/my_file.c:42
  0x400d5678: app_main at /path/to/main.c:18
```

**Monitor shortcuts:** `Ctrl+]` exit, `Ctrl+T Ctrl+R` reset, `Ctrl+T Ctrl+F` flash+restart.

## Core Dumps

### Setup

**sdkconfig.defaults:**
```ini
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=64
```

**Partition table -- add coredump partition:**
```csv
coredump, data, coredump, , 0x10000,
```

### Retrieve and Analyze

```bash
# Print crash summary
idf.py coredump-info

# Full GDB session with crash state
idf.py coredump-debug
```

In GDB you can inspect all task stacks, variables, and registers at the moment of crash.

### Production Use

For field devices, read core dump from flash over the network and upload to your backend. ESP Insights (`espressif/esp_insights` component) automates remote crash reporting.

## JTAG Debugging

### Hardware

| Chip | JTAG Option |
|------|-------------|
| ESP32 | ESP-PROG or FTDI adapter (external) |
| ESP32-S3 | Built-in USB-JTAG (no adapter needed) |
| ESP32-C3/C6/H2 | Built-in USB-JTAG |

### OpenOCD + GDB

```bash
# Terminal 1: Start OpenOCD
openocd -f board/esp32s3-builtin.cfg

# Terminal 2: Connect GDB
xtensa-esp32s3-elf-gdb build/project.elf
(gdb) target remote :3333
(gdb) mon reset halt
(gdb) thb app_main
(gdb) continue
```

### VS Code JTAG Debugging

1. `Ctrl+Shift+P` > `ESP-IDF: Select OpenOCD Board Configuration`
2. Press F5 to start debugging (extension handles OpenOCD + GDB)
3. Set breakpoints, inspect variables, step through code in the IDE

### Useful GDB Commands

| Command | Purpose |
|---------|---------|
| `info threads` | List all FreeRTOS tasks |
| `thread N` | Switch to task N |
| `bt` | Backtrace current task |
| `p variable` | Print variable value |
| `mon reset halt` | Reset and halt chip |
| `watch variable` | Break on variable change |

## Common Crash Types

| Panic | Likely Cause |
|-------|-------------|
| `LoadProhibited` / `StoreProhibited` | NULL pointer dereference or bad memory access |
| `InstrFetchProhibited` | Corrupted function pointer or stack overflow |
| `IllegalInstruction` | Stack overflow corrupting code, or wrong flash mode |
| `Stack canary watchpoint` | Stack overflow on a specific task -- increase stack size |
| `Task watchdog` | Task blocking too long without yielding |
| `Interrupt watchdog` | ISR taking too long, or critical section deadlock |
