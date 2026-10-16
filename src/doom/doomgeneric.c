#include <stdio.h>

#include "m_argv.h"

#include "doomgeneric.h"

#ifdef ESP32
#include "esp_heap_caps.h"
#include "esp32mem.h"
#endif

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

#ifdef ESP32
	DG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(pixel_t),
	                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(pixel_t));
#endif
	if (DG_ScreenBuffer == NULL)
	{
		fprintf(stderr, "Failed to allocate DG_ScreenBuffer\n");
		return;
	}

	DG_Init();

#ifdef ESP32
	doom_esp32_copy_info_tables();
	doom_esp32_copy_sound_tables();
	doom_esp32_config_to_psram();
	doom_esp32_alloc_game();
#endif

	D_DoomMain ();
}

