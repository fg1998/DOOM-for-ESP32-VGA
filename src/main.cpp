#include <Arduino.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"

#include "board_config.h"
#include "doomgeneric_fabgl.h"

extern "C" {
#include "doomgeneric.h"
}

static char iwadPath[64];
static char *doomArgv[8];
static int doomArgc = 0;

static void disableWatchdogs(void)
{
    /* Core 1 WDT is disabled from setup() (that core). This task is core 0. */
    disableCore0WDT();
}

static void buildArgs(const char *iwad)
{
    static char arg0[] = "doom";
    static char argIwad[] = "-iwad";
    static char argMb[] = "-mb";
    static char argMbVal[] = "3";
    static char argNogui[] = "-nogui";

    doomArgc = 0;
    doomArgv[doomArgc++] = arg0;
    doomArgv[doomArgc++] = argIwad;
    doomArgv[doomArgc++] = (char *)iwad;
    doomArgv[doomArgc++] = argMb;
    doomArgv[doomArgc++] = argMbVal;
    doomArgv[doomArgc++] = argNogui;
}

static void doomTask(void * /*param*/)
{
    Serial.printf("DOOM task on core %d\n", xPortGetCoreID());
    Serial.printf("SRAM livre=%u  PSRAM livre=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    if (!fabgl_hw_begin()) {
        Serial.println("Falha ao iniciar VGA");
        for (;;) {
            delay(1000);
        }
    }

    fabgl_show_splash("Montando cartao SD...", "Tire o SD na hora do upload");

    bool sdOk = false;
    for (int attempt = 0; attempt < 4 && !sdOk; ++attempt) {
        sdOk = fabgl_sd_begin();
        if (!sdOk) {
            delay(250);
        }
    }
    /* bootl.rc (PS/2 pins) has to be read before the ULP starts. */
    fabgl_input_begin();
    if (!sdOk) {
        fabgl_show_splash("SD nao encontrado", "Insira um cartao e aguarde");
    }

    unsigned long lastScan = 0;
    while (true) {
        unsigned long now = millis();
        if (now - lastScan > 1500) {
            lastScan = now;
            if (!sdOk) {
                sdOk = fabgl_sd_begin();
            }
            if (sdOk && fabgl_find_iwad(iwadPath, sizeof(iwadPath))) {
                break;
            }
            fabgl_show_splash(sdOk ? "Coloque doom1.wad na raiz do SD" : "SD nao encontrado",
                              "Shareware ou Freedoom");
            Serial.println("Aguardando IWAD em /sd/doom1.wad ...");
        }
        delay(50);
    }

    fabgl_show_splash("Carregando", iwadPath);
    buildArgs(iwadPath);
    disableWatchdogs();

    Serial.printf("SRAM livre=%u  PSRAM livre=%u (antes do Create)\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    doomgeneric_Create(doomArgc, doomArgv);

    for (;;) {
        doomgeneric_Tick();
    }
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(200);
    /* setup() runs on core 1 (VGA). Disable its WDT here, not from the DOOM task. */
    disableCore1WDT();
    disableLoopWDT();
    /* ESP32_Bootloader: next reset returns to the SD menu, not this app. */
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        esp_partition_erase_range(otadata, 0, otadata->size);
    }
    Serial.println();
    Serial.println("=== DOOM ESP32 VGA32 ===");
    Serial.printf("CPU %u MHz  PSRAM %u bytes\n",
                  (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getFreePsram());

    if (ESP.getFreePsram() < 2 * 1024 * 1024) {
        Serial.println("AVISO: esta placa precisa de PSRAM (modulo WROVER / VGA32).");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        doomTask, "doom", DOOM_TASK_STACK, nullptr, DOOM_TASK_PRIO, nullptr, DOOM_TASK_CORE);

    if (ok != pdPASS) {
        Serial.println("xTaskCreatePinnedToCore falhou, tentando stack menor");
        ok = xTaskCreatePinnedToCore(
            doomTask, "doom", 24576, nullptr, DOOM_TASK_PRIO, nullptr, DOOM_TASK_CORE);
    }
    if (ok != pdPASS) {
        Serial.println("ERRO: nao foi possivel criar a task do DOOM");
    }
}

void loop()
{
    /* Arduino loopTask lives on core 1 with the VGA ISR. delay(1000) woke it
     * once a second and caused a short flicker. Park it forever. */
    vTaskDelete(NULL);
}
