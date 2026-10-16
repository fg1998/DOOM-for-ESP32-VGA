#pragma once

// LILYGO TTGO VGA32 v1.4 / FabGL VGA32 pinout
// Same mapping used by fg1998/ESP32-Basic-vga

#define VGA_PIN_R1        GPIO_NUM_22
#define VGA_PIN_R0        GPIO_NUM_21
#define VGA_PIN_G1        GPIO_NUM_19
#define VGA_PIN_G0        GPIO_NUM_18
#define VGA_PIN_B1        GPIO_NUM_5
#define VGA_PIN_B0        GPIO_NUM_4
#define VGA_PIN_HSYNC     GPIO_NUM_23
#define VGA_PIN_VSYNC     GPIO_NUM_15

#define SD_PIN_SCK        14
#define SD_PIN_MISO       2
#define SD_PIN_MISO_FALLBACK 35
#define SD_PIN_MOSI       12
#define SD_PIN_CS         13

#define AUDIO_PIN         25
#define USER_BUTTON_PIN   36

/* FabGL KeyboardPort0 / porta PS/2 roxa da VGA32 */
#define PS2_PIN_CLK_DEFAULT GPIO_NUM_33
#define PS2_PIN_DAT_DEFAULT GPIO_NUM_32

#define SD_MOUNT_PATH     "/sd"
#define SD_SPI_HZ         20000000

#define DOOM_TASK_STACK   32768
#define DOOM_TASK_CORE    0
#define DOOM_TASK_PRIO    1

#define SERIAL_BAUD       115200
