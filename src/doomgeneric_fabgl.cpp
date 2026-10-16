#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/rtc_io.h"
#include "esp_heap_caps.h"
#include "fabgl.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"

#include "board_config.h"
#include "doomgeneric_fabgl.h"

extern "C" {
#include "doomgeneric.h"
#include "doomkeys.h"
int  doom_palette_was_changed(void);
void doom_palette_ack_changed(void);
void doom_palette_rgb(int index, unsigned char *r, unsigned char *g, unsigned char *b);
}

/* 640x200 DoubleScan (same 70 Hz 400-line timing as the original
 * 320x200 mode). 2x horizontal only; vertical is hardware doublescan.
 * 640x400/480 unique lines made monitors stretch Doom on 4:3. */
static fabgl::VGADirectController VGAController(false);
static fabgl::PS2Controller PS2Controller;
static SPIClass spiSD(HSPI);

#define KEYQUEUE_SIZE 32
#define DOOM_W 320
#define DOOM_H 200
#define VGA_W  640
#define VGA_H  200
#define SCANLINES_PER_CB 2

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

/* Remainder dither LUT: 256 PLAYPAL × 16 Bayer thresholds. DRAM, ISR-safe. */
static uint8_t s_ditherLut[256][16];
static uint8_t s_bayer4[4][4] = {
    { 0,  8,  2, 10 },
    { 12, 4, 14,  6 },
    { 3, 11,  1,  9 },
    { 15, 7, 13,  5 }
};
#define FB_WORDS_PER_LINE (VGA_W / 4)
static uint32_t *s_row[VGA_H];
static bool s_fbReady = false;
static bool s_lutReady = false;
static bool s_buttonWasDown = false;
static int s_lastUnique = -1;
static int s_ps2Clk = PS2_PIN_CLK_DEFAULT;
static int s_ps2Dat = PS2_PIN_DAT_DEFAULT;
static int s_magicButton = USER_BUTTON_PIN;

static const char *kIwadNames[] = {
    "doom1.wad",
    "DOOM1.WAD",
    "doom.wad",
    "DOOM.WAD",
    "freedoom1.wad",
    "FREEDOOM1.WAD",
    "freedoom2.wad",
    "doom2.wad",
    "DOOM2.WAD",
    nullptr
};

static void addKeyToQueue(int pressed, unsigned char key)
{
    if (key == 0) {
        return;
    }
    unsigned short keyData = (unsigned short)((pressed << 8) | key);
    s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
    s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
}

static unsigned char convertVirtualKey(fabgl::VirtualKey vk)
{
    switch (vk) {
    case fabgl::VK_RETURN:
    case fabgl::VK_KP_ENTER:
        return KEY_ENTER;
    case fabgl::VK_ESCAPE:
        return KEY_ESCAPE;
    case fabgl::VK_LEFT:
    case fabgl::VK_a:
    case fabgl::VK_A:
        return KEY_LEFTARROW;
    case fabgl::VK_RIGHT:
    case fabgl::VK_d:
    case fabgl::VK_D:
        return KEY_RIGHTARROW;
    case fabgl::VK_UP:
    case fabgl::VK_w:
    case fabgl::VK_W:
        return KEY_UPARROW;
    case fabgl::VK_DOWN:
    case fabgl::VK_s:
    case fabgl::VK_S:
        return KEY_DOWNARROW;
    case fabgl::VK_LCTRL:
    case fabgl::VK_RCTRL:
    case fabgl::VK_z:
    case fabgl::VK_Z:
        return KEY_FIRE;
    case fabgl::VK_SPACE:
    case fabgl::VK_e:
    case fabgl::VK_E:
        return KEY_USE;
    case fabgl::VK_LSHIFT:
    case fabgl::VK_RSHIFT:
        return KEY_RSHIFT;
    case fabgl::VK_LALT:
    case fabgl::VK_RALT:
        return KEY_LALT;
    case fabgl::VK_TAB:
        return KEY_TAB;
    case fabgl::VK_BACKSPACE:
        return KEY_BACKSPACE;
    case fabgl::VK_PAUSE:
        return KEY_PAUSE;
    case fabgl::VK_F1:
        return KEY_F1;
    case fabgl::VK_F2:
        return KEY_F2;
    case fabgl::VK_F3:
        return KEY_F3;
    case fabgl::VK_F4:
        return KEY_F4;
    case fabgl::VK_F5:
        return KEY_F5;
    case fabgl::VK_F6:
        return KEY_F6;
    case fabgl::VK_F7:
        return KEY_F7;
    case fabgl::VK_F8:
        return KEY_F8;
    case fabgl::VK_F9:
        return KEY_F9;
    case fabgl::VK_F10:
        return KEY_F10;
    case fabgl::VK_F11:
        return KEY_F11;
    case fabgl::VK_F12:
        return KEY_F12;
    case fabgl::VK_EQUALS:
    case fabgl::VK_PLUS:
    case fabgl::VK_KP_PLUS:
        return KEY_EQUALS;
    case fabgl::VK_MINUS:
    case fabgl::VK_KP_MINUS:
        return KEY_MINUS;
    case fabgl::VK_COMMA:
        return ',';
    case fabgl::VK_PERIOD:
        return '.';
    case fabgl::VK_y:
    case fabgl::VK_Y:
        return 'y';
    case fabgl::VK_n:
    case fabgl::VK_N:
        return 'n';
    default:
        if (vk >= fabgl::VK_0 && vk <= fabgl::VK_9) {
            return (unsigned char)('0' + (vk - fabgl::VK_0));
        }
        return 0;
    }
}

static unsigned char convertSerialChar(int ch)
{
    switch (ch) {
    case 27:
        return KEY_ESCAPE;
    case '\r':
    case '\n':
        return KEY_ENTER;
    case 8:
    case 127:
        return KEY_BACKSPACE;
    case '\t':
        return KEY_TAB;
    case ' ':
        return KEY_USE;
    case 'w':
    case 'W':
        return KEY_UPARROW;
    case 's':
    case 'S':
        return KEY_DOWNARROW;
    case 'a':
    case 'A':
        return KEY_LEFTARROW;
    case 'd':
    case 'D':
        return KEY_RIGHTARROW;
    case 'z':
    case 'Z':
    case 'f':
    case 'F':
        return KEY_FIRE;
    case 'e':
    case 'E':
        return KEY_USE;
    default:
        if (ch >= 1 && ch < 127) {
            return (unsigned char)ch;
        }
        return 0;
    }
}

/* Mix only the remainder between RGB222 levels 0/85/170/255.
 * At 640x200 (doublescan) a 4x4 cell is 4 output pixels. */
static uint8_t quantRgb222(int v, int thr)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }
    int lo = v / 85;
    if (lo > 3) {
        lo = 3;
    }
    const int rem = v - lo * 85;
    if (lo < 3 && rem > thr) {
        return (uint8_t)(lo + 1);
    }
    return (uint8_t)lo;
}

static uint32_t * IRAM_ATTR fbLine(int y)
{
    return s_row[y];
}

static void IRAM_ATTR copyPackedLine(uint8_t *dest, const uint32_t *src)
{
    uint32_t *d = (uint32_t *)dest;
    for (int i = 0; i < FB_WORDS_PER_LINE; ++i) {
        d[i] = src[i];
    }
}

static void IRAM_ATTR drawScanline(void * /*arg*/, uint8_t *dest, int scanLine)
{
    const uint8_t blank = s_ditherLut[0][0];
    for (int i = 0; i < SCANLINES_PER_CB; ++i) {
        const int y = scanLine + i;
        if (!s_fbReady || y < 0 || y >= VGA_H || s_row[y] == nullptr) {
            for (int x = 0; x < VGA_W; ++x) {
                dest[x] = blank;
            }
            dest += VGA_W;
            continue;
        }
        copyPackedLine(dest, fbLine(y));
        dest += VGA_W;
    }
}

static void rebuildPaletteLut(void)
{
    bool used[64];
    for (int i = 0; i < 64; ++i) {
        used[i] = false;
    }
    int unique = 0;
    for (int i = 0; i < 256; ++i) {
        unsigned char srcR = 0, srcG = 0, srcB = 0;
        doom_palette_rgb(i, &srcR, &srcG, &srcB);
        for (int t = 0; t < 16; ++t) {
            const int thr = (t * 85) >> 4;
            const uint8_t r = quantRgb222(srcR, thr);
            const uint8_t g = quantRgb222(srcG, thr);
            const uint8_t b = quantRgb222(srcB, thr);
            const int packed = (r << 4) | (g << 2) | b;
            s_ditherLut[i][t] = VGAController.createRawPixel(fabgl::RGB222(r, g, b));
            if (!used[packed]) {
                used[packed] = true;
                unique++;
            }
        }
    }
    s_lutReady = true;
    doom_palette_ack_changed();
    if (unique != s_lastUnique) {
        s_lastUnique = unique;
        Serial.printf("Paleta PLAYPAL -> RGB222: %d de 64 (dither resto 640x200)\n", unique);
    }
}

static void fillLutIndex(int index, uint8_t raw)
{
    for (int t = 0; t < 16; ++t) {
        s_ditherLut[index][t] = raw;
    }
}

static void setSplashPalette(void)
{
    fillLutIndex(0, VGAController.createRawPixel(fabgl::RGB222(0, 0, 0)));
    fillLutIndex(1, VGAController.createRawPixel(fabgl::RGB222(3, 0, 0)));
    fillLutIndex(2, VGAController.createRawPixel(fabgl::RGB222(3, 3, 0)));
    fillLutIndex(3, VGAController.createRawPixel(fabgl::RGB222(3, 3, 3)));
    fillLutIndex(4, VGAController.createRawPixel(fabgl::RGB222(0, 3, 0)));
    fillLutIndex(5, VGAController.createRawPixel(fabgl::RGB222(0, 3, 3)));
    fillLutIndex(6, VGAController.createRawPixel(fabgl::RGB222(1, 3, 3)));
    const uint8_t white = s_ditherLut[3][0];
    for (int i = 7; i < 256; ++i) {
        fillLutIndex(i, white);
    }
}

static void expandLine(uint32_t *dstWords, const uint8_t *src, int y)
{
    uint8_t tmp[VGA_W];
    const uint8_t *bay = s_bayer4[y & 3];
    for (int x = 0; x < DOOM_W; ++x) {
        const uint8_t *cell = s_ditherLut[src[x]];
        const int x0 = x << 1;
        VGA_PIXELINROW(tmp, x0) = cell[bay[x0 & 3]];
        VGA_PIXELINROW(tmp, x0 + 1) = cell[bay[(x0 + 1) & 3]];
    }
    const uint32_t *s = (const uint32_t *)tmp;
    for (int i = 0; i < FB_WORDS_PER_LINE; ++i) {
        dstWords[i] = s[i];
    }
}

static void blitFrame(void)
{
    if (DG_ScreenBuffer == nullptr || !s_fbReady) {
        return;
    }
#ifdef CMAP256
    if (!s_lutReady || doom_palette_was_changed()) {
        rebuildPaletteLut();
    }
    const uint8_t *src = DG_ScreenBuffer;
    for (int y = 0; y < DOOM_H; ++y) {
        uint32_t *dst = fbLine(y);
        if (dst == nullptr) {
            continue;
        }
        expandLine(dst, src + y * DOOM_W, y);
    }
#else
    (void)0;
#endif
}

static void pollKeyboard(void)
{
    auto kb = PS2Controller.keyboard();
    if (kb != nullptr) {
        fabgl::VirtualKeyItem item;
        while (kb->virtualKeyAvailable() && kb->getNextVirtualKey(&item, 0)) {
            unsigned char key = convertVirtualKey(item.vk);
            addKeyToQueue(item.down ? 1 : 0, key);
        }
    }

    while (Serial.available() > 0) {
        int ch = Serial.read();
        unsigned char key = convertSerialChar(ch);
        if (key) {
            addKeyToQueue(1, key);
            addKeyToQueue(0, key);
        }
    }

    int buttonDown = digitalRead(s_magicButton) == LOW;
    if (buttonDown && !s_buttonWasDown) {
        addKeyToQueue(1, KEY_FIRE);
    } else if (!buttonDown && s_buttonWasDown) {
        addKeyToQueue(0, KEY_FIRE);
    }
    s_buttonWasDown = buttonDown;
}

static void storePackedByte(uint32_t *line, int x, uint8_t raw)
{
    const int i = x ^ 2;
    const int wi = i >> 2;
    const int sh = (i & 3) * 8;
    uint32_t w = line[wi];
    w &= ~(0xFFu << sh);
    w |= ((uint32_t)raw << sh);
    line[wi] = w;
}

static void putDoomPixel(int x, int y, uint8_t raw)
{
    if (!s_fbReady || x < 0 || x >= DOOM_W || y < 0 || y >= DOOM_H) {
        return;
    }
    uint32_t *line = fbLine(y);
    const int x0 = x << 1;
    storePackedByte(line, x0, raw);
    storePackedByte(line, x0 + 1, raw);
}

static void plotText(int x, int y, const char *s, uint8_t color)
{
    if (!s_fbReady || s == nullptr) {
        return;
    }
    const uint8_t raw = s_ditherLut[color][0];
    const uint8_t *font = fabgl::FONT_8x8.data;
    for (; *s; ++s, x += 8) {
        if (x < 0 || x + 8 > DOOM_W || y < 0 || y + 8 > DOOM_H) {
            continue;
        }
        const uint8_t *glyph = font + ((unsigned char)*s) * 8;
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; ++col) {
                if (bits & (uint8_t)(0x80 >> col)) {
                    putDoomPixel(x + col, y + row, raw);
                }
            }
        }
    }
}

static void clearPackedFb(uint8_t raw)
{
    if (!s_fbReady) {
        return;
    }
    const uint32_t word = (uint32_t)raw * 0x01010101u;
    for (int y = 0; y < VGA_H; ++y) {
        uint32_t *line = s_row[y];
        if (line == nullptr) {
            continue;
        }
        for (int i = 0; i < FB_WORDS_PER_LINE; ++i) {
            line[i] = word;
        }
    }
}

static void drawStatusScreen(const char *title, const char *line1, const char *line2, const char *line3)
{
    if (!s_fbReady) {
        return;
    }
    clearPackedFb(s_ditherLut[0][0]);
    plotText(8, 16, "DOOM", 1);
    plotText(8, 32, "LilyGO VGA32  640x200", 2);
    if (title) {
        plotText(8, 64, title, 3);
    }
    if (line1) {
        plotText(8, 88, line1, 4);
    }
    if (line2) {
        plotText(8, 104, line2, 5);
    }
    if (line3) {
        plotText(8, 120, line3, 5);
    }
    plotText(8, 144, "64 cores RGB222  2x dither", 6);
    plotText(8, 160, "PS/2: setas WASD Ctrl Espaco", 3);
    plotText(8, 176, "Serial 115200 tambem e teclado", 3);
}

static void trimInPlace(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static bool gpioIsRtcPs2(int gpio)
{
    return rtc_gpio_is_valid_gpio((gpio_num_t)gpio);
}

static void applyBootlTag(const char *key, const char *val)
{
    int n = atoi(val);
    if (n < 0 || n > 39) {
        Serial.printf("bootl.rc: GPIO invalido %s=%s\n", key, val);
        return;
    }
    if (strcasecmp(key, "kbddat") == 0) {
        if (!gpioIsRtcPs2(n)) {
            Serial.printf("bootl.rc: kbddat=%d nao e GPIO RTC (PS/2)\n", n);
            return;
        }
        s_ps2Dat = n;
        return;
    }
    if (strcasecmp(key, "kbdclk") == 0) {
        if (!gpioIsRtcPs2(n)) {
            Serial.printf("bootl.rc: kbdclk=%d nao e GPIO RTC (PS/2)\n", n);
            return;
        }
        s_ps2Clk = n;
        return;
    }
    if (strcasecmp(key, "magicb") == 0) {
        s_magicButton = n;
    }
}

static int loadBootlRc(void)
{
    const char *paths[] = {
        "/sd/bootl.rc",
        "/sd/BOOTL.RC",
        "/bootl.rc",
        "bootl.rc",
        nullptr
    };
    FILE *f = nullptr;
    const char *used = nullptr;
    for (int i = 0; paths[i] != nullptr; ++i) {
        f = fopen(paths[i], "r");
        if (f != nullptr) {
            used = paths[i];
            break;
        }
    }
    if (f == nullptr) {
        return 0;
    }
    Serial.printf("bootl.rc: lendo %s\n", used);
    char line[96];
    while (fgets(line, sizeof(line), f) != nullptr) {
        trimInPlace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trimInPlace(key);
        trimInPlace(val);
        if (key[0] == '\0' || val[0] == '\0') {
            continue;
        }
        applyBootlTag(key, val);
    }
    fclose(f);
    return 1;
}

bool fabgl_hw_begin(void)
{
    /* 640x200 packed as uint32 words (128000 bytes). ESP32 8-bit DRAM's
     * largest hole is ~110 KB, so one 128K or two 64K blocks fail. Carve
     * the 200 lines out of whatever holes exist (8-bit first, then
     * 32-bit-only). 32-bit-only DRAM is never byte-addressed. */
    const uint32_t cap8 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t cap32 = MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT;
    const size_t lineBytes = (size_t)VGA_W;
    const size_t heapPad = 32;

    for (int y = 0; y < VGA_H; ++y) {
        s_row[y] = nullptr;
    }
    s_fbReady = false;

    /* Hold an 8-bit hole for FabGL DMA until VGAController.begin(). */
    void *fabglPark = heap_caps_malloc(8192, cap8);
    if (fabglPark == nullptr) {
        fabglPark = heap_caps_malloc(4096, cap8);
    }

    void *stripes[VGA_H];
    int nStripes = 0;
    int filled = 0;
    int lines8 = 0;
    int lines32 = 0;

    auto takeFrom = [&](uint32_t cap, bool is8) -> bool {
        const int left = VGA_H - filled;
        size_t largest = heap_caps_get_largest_free_block(cap);
        if (largest <= heapPad + lineBytes) {
            return false;
        }
        int n = (int)((largest - heapPad) / lineBytes);
        if (n > left) {
            n = left;
        }
        while (n >= 1) {
            void *p = heap_caps_malloc((size_t)n * lineBytes, cap);
            if (p != nullptr) {
                stripes[nStripes++] = p;
                uint32_t *base = (uint32_t *)p;
                for (int i = 0; i < n; ++i) {
                    s_row[filled++] = base + i * FB_WORDS_PER_LINE;
                }
                if (is8) {
                    lines8 += n;
                } else {
                    lines32 += n;
                }
                return true;
            }
            n--;
        }
        return false;
    };

    while (filled < VGA_H) {
        if (takeFrom(cap8, true)) {
            continue;
        }
        if (takeFrom(cap32, false)) {
            continue;
        }
        break;
    }

    if (fabglPark != nullptr) {
        heap_caps_free(fabglPark);
        fabglPark = nullptr;
    }

    if (filled < VGA_H) {
        Serial.printf("Falha FB 640x200 linhas=%d/200 faixas=%d  8bit livre=%u maior=%u  32bit livre=%u maior=%u\n",
                      filled, nStripes,
                      (unsigned)heap_caps_get_free_size(cap8),
                      (unsigned)heap_caps_get_largest_free_block(cap8),
                      (unsigned)heap_caps_get_free_size(cap32),
                      (unsigned)heap_caps_get_largest_free_block(cap32));
        for (int i = 0; i < nStripes; ++i) {
            heap_caps_free(stripes[i]);
        }
        for (int y = 0; y < VGA_H; ++y) {
            s_row[y] = nullptr;
        }
        return false;
    }

    s_fbReady = true;
    clearPackedFb(0);
    Serial.printf("Framebuffer 640x200 packed (%d faixas, %d linhas 8bit + %d 32bit, ISR memcpy)\n",
                  nStripes, lines8, lines32);
    memset(s_ditherLut, 0, sizeof(s_ditherLut));

    Serial.printf("DRAM heap antes do VGA: %u (maior bloco %u)\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    VGAController.begin(VGA_PIN_R1, VGA_PIN_R0, VGA_PIN_G1, VGA_PIN_G0,
                        VGA_PIN_B1, VGA_PIN_B0, VGA_PIN_HSYNC, VGA_PIN_VSYNC);
    VGAController.setScanlinesPerCallBack(SCANLINES_PER_CB);
    VGAController.setDrawScanlineCallback(drawScanline);
    VGAController.setResolution(VGA_640x200_70Hz);
    VGAController.run();

    setSplashPalette();

    Serial.println("=== VGADirect 640x200 70Hz DS 64 CORES (2x horiz, dither) ===");
    Serial.printf("VGA %dx%d  colors=%d  bits/ch=%d  DRAM depois=%u  PSRAM=%d\n",
                  VGAController.getScreenWidth(),
                  VGAController.getScreenHeight(),
                  VGAController.colorsCount(),
                  VGAController.getBitsPerChannel(),
                  (unsigned)ESP.getFreeHeap(),
                  (int)ESP.getFreePsram());
    Serial.flush();

    if (VGAController.getScreenWidth() < VGA_W || VGAController.getScreenHeight() < VGA_H) {
        Serial.println("VGA nao abriu 640x200");
        return false;
    }
    if (VGAController.colorsCount() < 64) {
        Serial.println("AVISO: colorsCount < 64 — o controlador nao esta em RGB222.");
    }
    return true;
}

static int s_sdMiso = -1;

static bool sdMountWithMiso(int miso)
{
    SD.end();
    spiSD.end();
    spiSD.begin(SD_PIN_SCK, miso, SD_PIN_MOSI, SD_PIN_CS);
    if (!SD.begin(SD_PIN_CS, spiSD, SD_SPI_HZ)) {
        Serial.printf("SD.begin falhou (CS=%d SCK=%d MISO=%d MOSI=%d)\n",
                      SD_PIN_CS, SD_PIN_SCK, miso, SD_PIN_MOSI);
        SD.end();
        spiSD.end();
        return false;
    }
    s_sdMiso = miso;
    Serial.printf("SD montado em /sd (MISO=%d)\n", miso);
    return true;
}

bool fabgl_sd_begin(void)
{
    if (s_sdMiso >= 0 && SD.cardType() != CARD_NONE) {
        return true;
    }
    if (s_sdMiso >= 0 && sdMountWithMiso(s_sdMiso)) {
        return true;
    }
    if (sdMountWithMiso(SD_PIN_MISO)) {
        return true;
    }
    Serial.println("Tentando SD com MISO=35");
    return sdMountWithMiso(SD_PIN_MISO_FALLBACK);
}

void fabgl_input_begin(void)
{
    if (loadBootlRc()) {
        Serial.printf("bootl.rc: kbddat=%d kbdclk=%d magicb=%d\n",
                      s_ps2Dat, s_ps2Clk, s_magicButton);
    } else {
        Serial.printf("bootl.rc ausente, PS/2 LilyGO padrao DAT=%d CLK=%d magicb=%d\n",
                      s_ps2Dat, s_ps2Clk, s_magicButton);
    }

    pinMode(s_magicButton, INPUT);
    (new fabgl::Keyboard)->begin((gpio_num_t)s_ps2Clk, (gpio_num_t)s_ps2Dat, true, true);
}

void fabgl_show_splash(const char *status_line, const char *detail_line)
{
    drawStatusScreen("Aguardando IWAD no SD", status_line, detail_line,
                     "doom1.wad  doom.wad  freedoom1.wad");
}

void fabgl_show_error(const char *title, const char *detail)
{
    drawStatusScreen(title, detail, "Ver serial 115200", "Reset para tentar de novo");
}

int fabgl_find_iwad(char *out_path, int out_size)
{
    if (out_path == nullptr || out_size < 16) {
        return 0;
    }
    for (int i = 0; kIwadNames[i] != nullptr; ++i) {
        const char *prefixes[] = { "/sd/", "/", "" };
        for (int p = 0; p < 3; ++p) {
            char path[80];
            snprintf(path, sizeof(path), "%s%s", prefixes[p], kIwadNames[i]);
            FILE *f = fopen(path, "rb");
            if (f != nullptr) {
                fclose(f);
                snprintf(out_path, out_size, "%s", path);
                Serial.printf("IWAD: %s\n", out_path);
                return 1;
            }
            if (SD.exists(kIwadNames[i]) || SD.exists(path)) {
                snprintf(out_path, out_size, "/sd/%s", kIwadNames[i]);
                FILE *f2 = fopen(out_path, "rb");
                if (f2 != nullptr) {
                    fclose(f2);
                    Serial.printf("IWAD: %s\n", out_path);
                    return 1;
                }
            }
        }
    }
    return 0;
}

void fabgl_poll_input(void)
{
    pollKeyboard();
}

extern "C" void DG_Init(void)
{
    s_lutReady = false;
    Serial.println("DG_Init");
}

extern "C" void DG_DrawFrame(void)
{
    blitFrame();
    pollKeyboard();
}

extern "C" void DG_SleepMs(uint32_t ms)
{
    delay(ms);
}

extern "C" uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)millis();
}

extern "C" int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    pollKeyboard();
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
        return 0;
    }
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = (keyData >> 8) & 0xFF;
    *doomKey = (unsigned char)(keyData & 0xFF);
    return 1;
}

extern "C" void DG_SetWindowTitle(const char *title)
{
    if (title) {
        Serial.printf("DOOM: %s\n", title);
    }
}
