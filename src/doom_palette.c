#include "i_video.h"

int doom_palette_was_changed(void)
{
#ifdef CMAP256
    return palette_changed ? 1 : 0;
#else
    return 1;
#endif
}

void doom_palette_ack_changed(void)
{
#ifdef CMAP256
    palette_changed = false;
#endif
}

void doom_palette_rgb(int index, unsigned char *r, unsigned char *g, unsigned char *b)
{
#ifdef CMAP256
    *r = (unsigned char)colors[index].r;
    *g = (unsigned char)colors[index].g;
    *b = (unsigned char)colors[index].b;
#else
    *r = *g = *b = 0;
    (void)index;
#endif
}
