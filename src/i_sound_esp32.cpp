#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_config.h"

extern "C" {
#include "deh_str.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"
}

/* VGA32 jack is a built-in DAC. LilyGO v1.4 often wires GPIO26;
 * some boards and FabGL examples use GPIO25. Drive both. */

#define MIX_RATE       11025
#define MIX_CHUNK      128
#define MIX_CHANNELS   8
#define MIX_TASK_PRIO  5
#define MIX_TASK_STACK 4096
#define MUS_VOICES     10
#define MUS_RATE       140
#define MUS_PERC_CHAN  15

enum {
    WAV_SQUARE = 0,
    WAV_PULSE  = 1,
    WAV_TRI    = 2,
    WAV_NOISE  = 3
};

extern "C" {

typedef struct {
    uint8_t *pcm;
    unsigned int len;
    unsigned int rate;
} cached_sfx_t;

typedef struct {
    const uint8_t *pcm;
    unsigned int len;
    uint32_t pos;
    uint32_t step;
    int vol;
    volatile int active;
} mix_chan_t;

typedef struct {
    const uint8_t *data;
    int len;
} mus_song_t;

typedef struct {
    int on;
    int chan;
    int note;
    int wave;
    int vol;
    int life;
    uint32_t phase;
    uint32_t step;
} mus_voice_t;

static boolean s_usePrefix;
static boolean s_ready;
static TaskHandle_t s_mixTask;
static mix_chan_t s_ch[MIX_CHANNELS];

static snddevice_t s_devices[] = {
    SNDDEVICE_SB,
};

static mus_song_t s_song;
static const uint8_t *s_mus;
static int s_musLen;
static int s_musOff;
static int s_musStart;
static int s_musDelay;
static uint32_t s_musTickFrac;
static volatile int s_musPlaying;
static volatile int s_musPaused;
static int s_musLoop;
static volatile int s_musicVol = 64;
static uint8_t s_chanVol[16];
static uint8_t s_chanPatch[16];
static mus_voice_t s_voice[MUS_VOICES];
static uint32_t s_noteStep[128];
static uint32_t s_lfsr = 0xACE1u;
static const uint32_t s_musTickInc = (MUS_RATE << 16) / MIX_RATE;

static int GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[9];

    if (sfxinfo == NULL) {
        return -1;
    }
    if (sfxinfo->link) {
        sfxinfo = sfxinfo->link;
    }

    if (s_usePrefix) {
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfxinfo->name));
    } else {
        M_StringCopy(namebuf, DEH_String(sfxinfo->name), sizeof(namebuf));
    }

    return W_CheckNumForName(namebuf);
}

static cached_sfx_t *ensureCached(sfxinfo_t *sfxinfo)
{
    if (sfxinfo == NULL) {
        return NULL;
    }
    if (sfxinfo->link) {
        sfxinfo = sfxinfo->link;
    }
    if (sfxinfo->driver_data != NULL) {
        return (cached_sfx_t *)sfxinfo->driver_data;
    }

    const int lump = GetSfxLumpNum(sfxinfo);
    if (lump < 0) {
        return NULL;
    }

    const int lumplen = W_LumpLength((unsigned int)lump);
    if (lumplen < 8) {
        return NULL;
    }

    byte *lumpdata = (byte *)W_CacheLumpNum(lump, PU_CACHE);
    if (lumpdata == NULL) {
        return NULL;
    }

    /* DMX: uint16 format=3, uint16 rate, uint32 count, then unsigned PCM. */
    if (lumpdata[0] != 0x03 || lumpdata[1] != 0x00) {
        W_ReleaseLumpNum(lump);
        return NULL;
    }

    const unsigned int rate = (unsigned int)(lumpdata[2] | (lumpdata[3] << 8));
    unsigned int length = (unsigned int)(lumpdata[4] | (lumpdata[5] << 8) |
                                         (lumpdata[6] << 16) | (lumpdata[7] << 24));
    if (length > (unsigned int)(lumplen - 8) || length <= 48) {
        W_ReleaseLumpNum(lump);
        return NULL;
    }

    const byte *pcm_src = lumpdata + 8 + 16;
    length -= 32;

    uint8_t *pcm = (uint8_t *)heap_caps_malloc(
        length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cached_sfx_t *cached = (cached_sfx_t *)heap_caps_malloc(
        sizeof(cached_sfx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL || cached == NULL) {
        if (pcm) {
            heap_caps_free(pcm);
        }
        if (cached) {
            heap_caps_free(cached);
        }
        W_ReleaseLumpNum(lump);
        return NULL;
    }

    memcpy(pcm, pcm_src, length);
    W_ReleaseLumpNum(lump);

    cached->pcm = pcm;
    cached->len = length;
    cached->rate = rate ? rate : MIX_RATE;
    sfxinfo->driver_data = cached;
    return cached;
}

static int noiseSample(void)
{
    s_lfsr ^= s_lfsr << 13;
    s_lfsr ^= s_lfsr >> 17;
    s_lfsr ^= s_lfsr << 5;
    return (int)(s_lfsr & 255) - 128;
}

static int waveForPatch(int patch)
{
    if (patch >= 32 && patch < 40) {
        return WAV_SQUARE; /* bass */
    }
    if (patch >= 16 && patch < 24) {
        return WAV_SQUARE; /* organ */
    }
    if (patch >= 24 && patch < 32) {
        return WAV_PULSE; /* guitar */
    }
    if (patch >= 40 && patch < 56) {
        return WAV_TRI; /* strings / ensemble */
    }
    return WAV_TRI;
}

static void voicesOff(int chan)
{
    for (int i = 0; i < MUS_VOICES; ++i) {
        if (chan < 0 || s_voice[i].chan == chan) {
            s_voice[i].on = 0;
        }
    }
}

static void noteOff(int chan, int note)
{
    for (int i = 0; i < MUS_VOICES; ++i) {
        if (s_voice[i].on && s_voice[i].chan == chan && s_voice[i].note == note) {
            s_voice[i].on = 0;
        }
    }
}

static mus_voice_t *allocVoice(int chan, int note)
{
    for (int i = 0; i < MUS_VOICES; ++i) {
        if (s_voice[i].on && s_voice[i].chan == chan && s_voice[i].note == note) {
            return &s_voice[i];
        }
    }
    for (int i = 0; i < MUS_VOICES; ++i) {
        if (!s_voice[i].on) {
            return &s_voice[i];
        }
    }
    int quietest = 0;
    for (int i = 1; i < MUS_VOICES; ++i) {
        if (s_voice[i].vol < s_voice[quietest].vol) {
            quietest = i;
        }
    }
    return &s_voice[quietest];
}

static void noteOn(int chan, int note, int vel)
{
    if (note < 0 || note > 127) {
        return;
    }
    mus_voice_t *v = allocVoice(chan, note);
    v->chan = chan;
    v->note = note;
    v->vol = vel;
    v->phase = 0;
    v->on = 1;
    if (chan == MUS_PERC_CHAN) {
        v->life = MIX_RATE / 12;
        if (note == 35 || note == 36) {
            v->wave = WAV_SQUARE;
            v->step = s_noteStep[32];
            v->life = MIX_RATE / 8;
            v->vol = vel + (vel >> 1);
            if (v->vol > 127) {
                v->vol = 127;
            }
        } else if (note == 42 || note == 44 || note == 46) {
            v->wave = WAV_NOISE;
            v->step = 0;
            v->life = MIX_RATE / 24;
        } else if (note == 49 || note == 51 || note == 57) {
            v->wave = WAV_NOISE;
            v->step = 0;
            v->life = MIX_RATE / 4;
        } else {
            v->wave = WAV_NOISE;
            v->step = 0;
        }
    } else {
        v->wave = waveForPatch(s_chanPatch[chan]);
        v->step = s_noteStep[note];
        v->life = -1;
    }
}

static int musGet(void)
{
    if (s_mus == NULL || s_musOff >= s_musLen) {
        return -1;
    }
    return s_mus[s_musOff++];
}

static void musRestart(void)
{
    voicesOff(-1);
    s_musOff = s_musStart;
    s_musDelay = 0;
}

static void musProcessEvent(void)
{
    const int b = musGet();
    if (b < 0) {
        if (s_musLoop) {
            musRestart();
        } else {
            s_musPlaying = 0;
        }
        return;
    }

    const int chan = b & 0x0f;
    const int ev = (b >> 4) & 7;
    const int last = b & 0x80;

    switch (ev) {
    case 0: { /* release note */
        const int n = musGet();
        if (n >= 0) {
            noteOff(chan, n & 0x7f);
        }
        break;
    }
    case 1: { /* play note */
        int n = musGet();
        if (n < 0) {
            break;
        }
        if (n & 0x80) {
            const int vol = musGet();
            if (vol >= 0) {
                s_chanVol[chan] = (uint8_t)(vol & 0x7f);
            }
            n &= 0x7f;
        }
        noteOn(chan, n, s_chanVol[chan]);
        break;
    }
    case 2: /* pitch wheel */
        (void)musGet();
        break;
    case 3: { /* system */
        const int sys = musGet();
        if (sys == 10 || sys == 11 || sys == 12 || sys == 14) {
            voicesOff(chan);
        }
        break;
    }
    case 4: { /* controller */
        const int ctrl = musGet();
        const int val = musGet();
        if (ctrl < 0 || val < 0) {
            break;
        }
        if (ctrl == 0) {
            s_chanPatch[chan] = (uint8_t)(val & 0x7f);
        } else if (ctrl == 3 || ctrl == 5) {
            s_chanVol[chan] = (uint8_t)(val & 0x7f);
        } else if (ctrl == 14) {
            voicesOff(chan);
        }
        break;
    }
    case 6: /* score end */
        if (s_musLoop) {
            musRestart();
        } else {
            s_musPlaying = 0;
            voicesOff(-1);
        }
        return;
    default:
        break;
    }

    if (last) {
        int delay = 0;
        int t;
        do {
            t = musGet();
            if (t < 0) {
                break;
            }
            delay = (delay << 7) + (t & 127);
        } while (t & 0x80);
        s_musDelay = delay;
    }
}

static void musAdvanceSample(void)
{
    if (!s_musPlaying || s_musPaused || s_mus == NULL) {
        return;
    }
    s_musTickFrac += s_musTickInc;
    while (s_musTickFrac >= 0x10000u) {
        s_musTickFrac -= 0x10000u;
        if (s_musDelay > 0) {
            s_musDelay--;
        }
        int evlimit = 0;
        while (s_musPlaying && s_musDelay == 0 && evlimit < 32) {
            const int off = s_musOff;
            musProcessEvent();
            evlimit++;
            if (s_musOff == off && s_musDelay == 0) {
                break;
            }
        }
    }
}

static int mixMusic(void)
{
    int sum = 0;
    for (int i = 0; i < MUS_VOICES; ++i) {
        mus_voice_t *v = &s_voice[i];
        if (!v->on) {
            continue;
        }
        if (v->life == 0) {
            v->on = 0;
            continue;
        }
        if (v->life > 0) {
            v->life--;
        }

        int s = 0;
        const int idx = (int)(v->phase >> 24);
        v->phase += v->step;
        switch (v->wave) {
        case WAV_SQUARE:
            s = (idx < 128) ? 36 : -36;
            break;
        case WAV_PULSE:
            s = (idx < 64) ? 36 : -36;
            break;
        case WAV_TRI:
            s = (idx < 128) ? (idx - 64) : (191 - idx);
            s >>= 1;
            break;
        default:
            s = noiseSample() >> 2;
            break;
        }
        sum += (s * v->vol) >> 6;
    }
    return (sum * s_musicVol) >> 3;
}

static void mixChunk(uint16_t *stereo, int n)
{
    for (int i = 0; i < n; ++i) {
        int acc = 0;
        for (int c = 0; c < MIX_CHANNELS; ++c) {
            mix_chan_t *ch = &s_ch[c];
            if (!ch->active || ch->pcm == NULL) {
                continue;
            }
            const unsigned int idx = ch->pos >> 16;
            if (idx >= ch->len) {
                ch->active = 0;
                continue;
            }
            acc += ((int)ch->pcm[idx] - 128) * ch->vol;
            ch->pos += ch->step;
        }

        musAdvanceSample();
        acc += mixMusic();

        acc >>= 6;
        if (acc < -128) {
            acc = -128;
        } else if (acc > 127) {
            acc = 127;
        }
        const uint16_t s = (uint16_t)((acc + 128) << 8);
        stereo[i * 2] = s;
        stereo[i * 2 + 1] = s;
    }
}

static void mixTask(void * /*arg*/)
{
    uint16_t buf[MIX_CHUNK * 2];
    for (;;) {
        mixChunk(buf, MIX_CHUNK);
        size_t written = 0;
        i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY);
    }
}

static void initNoteTable(void)
{
    for (int n = 0; n < 128; ++n) {
        const double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        double step = freq * 4294967296.0 / (double)MIX_RATE;
        if (step < 1.0) {
            step = 1.0;
        }
        if (step > 4294967295.0) {
            step = 4294967295.0;
        }
        s_noteStep[n] = (uint32_t)step;
    }
    for (int i = 0; i < 16; ++i) {
        s_chanVol[i] = 100;
        s_chanPatch[i] = 0;
    }
}

static boolean InitSound(boolean use_sfx_prefix)
{
    s_usePrefix = use_sfx_prefix;
    memset(s_ch, 0, sizeof(s_ch));
    initNoteTable();

    i2s_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    cfg.sample_rate = MIX_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = MIX_CHUNK;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        printf("Som: i2s_driver_install falhou (%d)\n", (int)err);
        return false;
    }

    err = i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
    if (err != ESP_OK) {
        printf("Som: i2s_set_dac_mode falhou (%d)\n", (int)err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2s_set_sample_rates(I2S_NUM_0, MIX_RATE);
    i2s_zero_dma_buffer(I2S_NUM_0);

    BaseType_t ok = xTaskCreatePinnedToCore(
        mixTask, "doom-sfx", MIX_TASK_STACK, NULL, MIX_TASK_PRIO, &s_mixTask,
        DOOM_TASK_CORE);
    if (ok != pdPASS) {
        printf("Som: falha ao criar task de mixagem\n");
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    s_ready = true;
    printf("Som: I2S0 DAC GPIO25+26 @ %d Hz (SFX+MUS)\n", MIX_RATE);
    return true;
}

static void ShutdownSound(void)
{
    s_musPlaying = 0;
    if (s_mixTask) {
        vTaskDelete(s_mixTask);
        s_mixTask = NULL;
    }
    if (s_ready) {
        i2s_driver_uninstall(I2S_NUM_0);
        s_ready = false;
    }
}

static void UpdateSound(void)
{
}

static void UpdateSoundParams(int handle, int vol, int sep)
{
    (void)sep;
    if (handle < 0 || handle >= MIX_CHANNELS) {
        return;
    }
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 127) {
        vol = 127;
    }
    s_ch[handle].vol = vol;
}

static int StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    (void)sep;
    if (!s_ready || sfxinfo == NULL || channel < 0 || channel >= MIX_CHANNELS) {
        return -1;
    }

    cached_sfx_t *cached = ensureCached(sfxinfo);
    if (cached == NULL || cached->pcm == NULL || cached->len == 0) {
        return -1;
    }

    if (vol < 0) {
        vol = 0;
    }
    if (vol > 127) {
        vol = 127;
    }

    uint32_t step = ((uint32_t)cached->rate << 16) / MIX_RATE;
    if (step == 0) {
        step = 1u << 16;
    }

    s_ch[channel].active = 0;
    s_ch[channel].pcm = cached->pcm;
    s_ch[channel].len = cached->len;
    s_ch[channel].pos = 0;
    s_ch[channel].step = step;
    s_ch[channel].vol = vol;
    s_ch[channel].active = 1;
    return channel;
}

static void StopSound(int handle)
{
    if (handle < 0 || handle >= MIX_CHANNELS) {
        return;
    }
    s_ch[handle].active = 0;
}

static boolean SoundIsPlaying(int handle)
{
    if (handle < 0 || handle >= MIX_CHANNELS) {
        return false;
    }
    return s_ch[handle].active ? true : false;
}

static void CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    int ok = 0;
    int bytes = 0;
    for (int i = 1; i < num_sounds; ++i) {
        cached_sfx_t *c = ensureCached(&sounds[i]);
        if (c != NULL) {
            ok++;
            bytes += (int)c->len;
        }
    }
    printf("Som: %d efeitos em cache (%d bytes PSRAM)\n", ok, bytes);
}

sound_module_t ESP32_sound_module = {
    s_devices,
    1,
    InitSound,
    ShutdownSound,
    GetSfxLumpNum,
    UpdateSound,
    UpdateSoundParams,
    StartSound,
    StopSound,
    SoundIsPlaying,
    CacheSounds,
};

static boolean MusicInit(void)
{
    if (s_noteStep[69] == 0) {
        initNoteTable();
    }
    printf("Musica: sintetizador MUS (chiptune, nao AdLib)\n");
    return true;
}

static void MusicShutdown(void)
{
    s_musPlaying = 0;
    s_mus = NULL;
}

static void MusicSetVolume(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 127) {
        volume = 127;
    }
    s_musicVol = volume;
}

static void MusicPause(void)
{
    s_musPaused = 1;
}

static void MusicResume(void)
{
    s_musPaused = 0;
}

static void *MusicRegisterSong(void *data, int len)
{
    if (data == NULL || len < 16) {
        return NULL;
    }
    const uint8_t *p = (const uint8_t *)data;
    if (p[0] != 'M' || p[1] != 'U' || p[2] != 'S' || p[3] != 0x1a) {
        printf("Musica: lump nao e MUS\n");
        return NULL;
    }
    s_song.data = p;
    s_song.len = len;
    return &s_song;
}

static void MusicUnRegisterSong(void *handle)
{
    (void)handle;
    s_musPlaying = 0;
    s_mus = NULL;
}

static void MusicPlay(void *handle, boolean looping)
{
    mus_song_t *song = (mus_song_t *)handle;
    if (song == NULL || song->data == NULL || song->len < 16) {
        return;
    }
    const uint8_t *p = song->data;
    const int start = p[6] | (p[7] << 8);
    if (start < 16 || start >= song->len) {
        printf("Musica: scoreStart invalido (%d)\n", start);
        return;
    }

    s_musPlaying = 0;
    voicesOff(-1);
    s_mus = p;
    s_musLen = song->len;
    s_musStart = start;
    s_musOff = start;
    s_musDelay = 0;
    s_musTickFrac = 0;
    s_musLoop = looping ? 1 : 0;
    s_musPaused = 0;
    for (int i = 0; i < 16; ++i) {
        s_chanVol[i] = 100;
        s_chanPatch[i] = 0;
    }
    s_musPlaying = 1;
    printf("Musica: tocando MUS (%d bytes, loop=%d)\n", song->len, s_musLoop);
}

static void MusicStop(void)
{
    s_musPlaying = 0;
    s_mus = NULL;
    voicesOff(-1);
}

static boolean MusicIsPlaying(void)
{
    return s_musPlaying ? true : false;
}

static void MusicPoll(void)
{
}

music_module_t ESP32_music_module = {
    s_devices,
    1,
    MusicInit,
    MusicShutdown,
    MusicSetVolume,
    MusicPause,
    MusicResume,
    MusicRegisterSong,
    MusicUnRegisterSong,
    MusicPlay,
    MusicStop,
    MusicIsPlaying,
    MusicPoll,
};

} /* extern "C" */
