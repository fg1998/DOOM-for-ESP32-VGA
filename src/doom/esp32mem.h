#ifndef DOOM_ESP32MEM_H
#define DOOM_ESP32MEM_H

#ifdef ESP32
#include "esp_heap_caps.h"

/* Arduino's sections.ld puts *(.ext_ram.bss) into dram0.bss, so large
 * tables cannot use EXT_RAM_ATTR. Allocate them from PSRAM at runtime. */
#define DOOM_PSRAM_CALLOC(n, sz) \
    heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void doom_esp32_copy_info_tables(void);
void doom_esp32_copy_sound_tables(void);
void doom_esp32_config_to_psram(void);
void doom_esp32_alloc_game(void);
#else
#define DOOM_PSRAM_CALLOC(n, sz) calloc((n), (sz))
#endif

#endif
