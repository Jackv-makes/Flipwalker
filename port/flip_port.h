#pragma once

/* Interface between the app shell (flipwalker.c) and the adapters that stand
 * in for the Stick S3 hardware layer. Only native types
 * are used here; the game's H8-packed headers stay out of this file. */
#include <stdbool.h>
#include <stdint.h>

#define FLIP_SCREEN_W  96
#define FLIP_SCREEN_H  64
#define FLIP_XBM_BYTES (FLIP_SCREEN_W / 8 * FLIP_SCREEN_H)

/* Render 0..3 shade pixels into a 96x64 one-bit XBM (LSB first). */
typedef enum {
    FlipDitherSolid, /* light gray -> white, dark gray -> black (default) */
    FlipDitherGray,  /* two textures alternated every frame; the LCD blurs them */
    FlipDitherCount,
} FlipDither;
/* phase alternates each frame in Gray mode. */
void flip_render_xbm(const uint8_t* pixels, uint8_t* xbm, FlipDither mode, bool phase);

/* Monotonic microseconds from the Cortex cycle counter. Main thread only. */
uint64_t flip_now_us(void);
/* Convert a raw DWT->CYCCNT stamp (taken in an ISR) to flip_now_us time. */
uint64_t flip_cycles_to_us(uint32_t cycles);

/* Display: the game draws into an emulated NT7508; present copies it out. */
typedef void (*FlipFrameCallback)(void* context);
void flip_display_set_callback(FlipFrameCallback callback, void* context);
/* Copy the last presented frame as 0..3 shade indexes (0 = white). */
void flip_display_copy(uint8_t* pixels);
bool flip_display_present(void);
void flip_display_reset(void);

/* Input: Flipper key transitions in BUTTON_* bit form (center 2, left 4,
 * right 8). Each transition is held for one native scan. */
void flip_input_transition(uint8_t button_bit, bool pressed);
bool flip_input_active(void);

/* Motion: external I2C accelerometer on pins 15 (SDA) and 16 (SCL). */
void flip_motion_init(void);
void flip_motion_service(void);
const char* flip_motion_name(void);
void flip_motion_set_simulate(bool simulate);
bool flip_motion_simulating(void);

/* Sound through the Flipper speaker. */
void flip_sound_service(void);
bool flip_sound_busy(void);
void flip_sound_set_muted(bool muted);
bool flip_sound_muted(void);
void flip_sound_shutdown(void);

/* EEPROM image on the SD card with a write-back block cache. */
bool flip_eeprom_open(void);
void flip_eeprom_service(void);
void flip_eeprom_close(void);
bool flip_eeprom_failed(void);

/* IrDA SIR on USART1: pin 13 TX (IR LED), pin 14 RX (photodiode). */
void flip_ir_shutdown(void);
const char* flip_ir_status(void);
/* Looks for a transceiver by whether anything drives RXD. Safe only outside
 * a session. */
void flip_ir_probe(void);
const char* flip_ir_name(void);
/* Link health while a session is open: 0 no session, 1 clean, 2 trouble
 * (line errors, broken bursts, or the DS has gone quiet). */
typedef enum { FlipIrLinkNone, FlipIrLinkGood, FlipIrLinkPoor } FlipIrLink;
FlipIrLink flip_ir_link(void);
