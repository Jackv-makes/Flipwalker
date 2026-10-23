/* NT7508 emulation, ported from stickwalker/stick/display_bus.cpp. The game
 * streams panel commands and two bit-planes per column; presenting reads the
 * selected 96x64 window back out as four-shade pixels. */
#include <furi.h>
#include <string.h>

#include "flip_port.h"

#include "pw_begin.h"
#include "display_bus.h"
#include "display_panel.h"

StickDisplayIo IO;
StickDisplaySsu SSU;

#define COLUMNS 96
#define PAGES   16

static uint8_t ram[PAGES][COLUMNS][2];
static uint8_t tag[PAGES][COLUMNS]; /* FLIP_TAG_* of the blit that wrote the column */
static uint8_t tagging;
uint8_t flip_eeprom_buffer_kind(const void* ptr);
static uint8_t page, column, plane, start_line;
static bool start_line_parameter, contrast_parameter;
static bool screen_enabled = true;

static uint8_t presented[FLIP_SCREEN_W * FLIP_SCREEN_H];
static FuriMutex* presented_mutex;
static FlipFrameCallback frame_callback;
static void* frame_context;

static void command(uint8_t value) {
    if(start_line_parameter) {
        start_line = value & 0x7f;
        start_line_parameter = false;
        return;
    }
    if(contrast_parameter) {
        contrast_parameter = false;
        return;
    }
    if(value == 0x40) {
        start_line_parameter = true;
    } else if(value == 0x81) {
        contrast_parameter = true;
    } else if(value >= 0xb0 && value <= 0xbf) {
        page = value & 0x0f;
        plane = 0;
    } else if(value >= 0x10 && value <= 0x17) {
        column = (uint8_t)((column & 0x0f) | ((value & 7u) << 4));
        plane = 0;
    } else if(value <= 0x0f) {
        column = (uint8_t)((column & 0xf0) | value);
        plane = 0;
    } else if(value == 0xa9) {
        screen_enabled = false;
    } else if(value == 0xe1 || value == 0xaf) {
        screen_enabled = true;
    }
}

static void data(uint8_t value) {
    if(page < PAGES && column < COLUMNS) {
        ram[page][column][plane] = value;
        tag[page][column] = tagging;
    }
    plane ^= 1;
    if(!plane) ++column;
}

void StickDisplayBlitBegin(const u8* raster) {
    tagging = flip_eeprom_buffer_kind(raster);
}

void StickDisplayBlitEnd(void) {
    tagging = 0;
}

void StickDisplayBusInit(void) {
    memset(ram, 0, sizeof(ram));
    memset(tag, 0, sizeof(tag));
    tagging = 0;
    memset(&IO, 0, sizeof(IO));
    memset(&SSU, 0, sizeof(SSU));
    SSU.SSSR.BIT.TDRE = 1;
    SSU.SSSR.BIT.TEND = 1;
    page = column = plane = start_line = 0;
    start_line_parameter = contrast_parameter = false;
    screen_enabled = true;
}

void flip_display_reset(void) {
    StickDisplayBusInit();
    if(!presented_mutex) presented_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    memset(presented, 0, sizeof(presented));
}

int StickDisplayIsPowered(void) {
    return screen_enabled;
}

void StickDisplayWrite(u8 value) {
    if(IO.PDR1.BIT.B1)
        data(value);
    else
        command(value);
}

void StickDisplayFrame(u8* pixels, u16 byteCount) {
    if(!pixels || byteCount < COLUMNS * 64) return;
    for(unsigned y = 0; y < 64; ++y) {
        const unsigned physical_row = (start_line + y) & 127u;
        const unsigned row_page = physical_row >> 3;
        const uint8_t mask = (uint8_t)(1u << (physical_row & 7u));
        for(unsigned x = 0; x < COLUMNS; ++x) {
            const uint8_t hi = ram[row_page][x][0] & mask;
            const uint8_t lo = ram[row_page][x][1] & mask;
            pixels[y * COLUMNS + x] =
                screen_enabled ? (uint8_t)((hi ? 2u : 0u) | (lo ? 1u : 0u) |
                                           ((unsigned)tag[row_page][x] << 2)) : 0;
        }
    }
}

/* Contrast is a panel property with no Flipper equivalent. */
void StickDisplayPanelSetContrastDelta(unsigned delta) {
    (void)delta;
}

void flip_display_set_callback(FlipFrameCallback callback, void* context) {
    frame_callback = callback;
    frame_context = context;
}

bool flip_display_present(void) {
    furi_mutex_acquire(presented_mutex, FuriWaitForever);
    StickDisplayFrame(presented, sizeof(presented));
    furi_mutex_release(presented_mutex);
    if(frame_callback) frame_callback(frame_context);
    return true;
}

void flip_display_copy(uint8_t* pixels) {
    furi_mutex_acquire(presented_mutex, FuriWaitForever);
    memcpy(pixels, presented, sizeof(presented));
    furi_mutex_release(presented_mutex);
}

/* pw_home.c presents the frame between slow steps of a long transfer. */
int StickDisplayPresent(void) {
    return flip_display_present();
}
