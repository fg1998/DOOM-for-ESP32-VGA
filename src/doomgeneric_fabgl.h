#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool fabgl_hw_begin(void);
bool fabgl_sd_begin(void);
void fabgl_input_begin(void);
void fabgl_show_splash(const char *status_line, const char *detail_line);
void fabgl_show_error(const char *title, const char *detail);
int  fabgl_find_iwad(char *out_path, int out_size);
void fabgl_poll_input(void);

#ifdef __cplusplus
}
#endif
