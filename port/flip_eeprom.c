/* The walker's 64 KiB M95512 EEPROM, stored as a file on the SD card.
 *
 * stickwalker keeps the whole image in RAM; a Flipper app cannot spare that,
 * so a 16 KiB write-back cache of 256-byte blocks sits in front of the file.
 * Dirty blocks are written back when the game finishes a mirrored update or
 * an IR session (the same points stickwalker commits), or on exit. The game's
 * own mirrored records, checksums and staged-walk marker cover a crash in
 * between, exactly as they did on the original EEPROM. */
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <string.h>

#include "flip_port.h"
#include "flip_glue.h"

#include "pw_begin.h"
#include "eeprom_backend.h"
#include "application/pw_eeprom_m95512.h"
#include "eeprom_map.h"
#include "resources.h"

void StickEepromSetError(u8 value);

#define IMAGE_BYTES 65536u
#define BLOCK_BYTES 256u
#define BLOCK_COUNT (IMAGE_BYTES / BLOCK_BYTES)
#define SLOT_COUNT  64u
#define PAGE_BYTES  128u
#define NO_BLOCK    0xffffu

#define EEPROM_PATH APP_DATA_PATH("eeprom.bin")

typedef struct {
    uint16_t block;
    bool dirty;
    uint32_t used;
    uint8_t data[BLOCK_BYTES];
} Slot;

static Storage* storage;
static File* file;
static Slot* slots;
static uint32_t use_clock;
static bool mounted;
static bool failed;
static bool deferred;
static unsigned batch_depth;
static bool any_dirty;

static void storage_error(void) {
    failed = true;
    StickEepromSetError(1);
}

static bool write_block(Slot* slot) {
    if(!storage_file_seek(file, slot->block * BLOCK_BYTES, true)) return false;
    if(storage_file_write(file, slot->data, BLOCK_BYTES) != BLOCK_BYTES) return false;
    slot->dirty = false;
    return true;
}

static Slot* load(uint16_t block) {
    Slot* victim = &slots[0];
    for(unsigned i = 0; i < SLOT_COUNT; ++i) {
        if(slots[i].block == block) {
            slots[i].used = ++use_clock;
            return &slots[i];
        }
        if(slots[i].block == NO_BLOCK) {
            victim = &slots[i];
        } else if(victim->block != NO_BLOCK && slots[i].used < victim->used) {
            victim = &slots[i];
        }
    }
    if(victim->block != NO_BLOCK && victim->dirty && !write_block(victim)) return NULL;
    victim->block = NO_BLOCK;
    if(!storage_file_seek(file, block * BLOCK_BYTES, true)) return NULL;
    if(storage_file_read(file, victim->data, BLOCK_BYTES) != BLOCK_BYTES) return NULL;
    victim->block = block;
    victim->dirty = false;
    victim->used = ++use_clock;
    return victim;
}

static bool range_ok(uint32_t address, uint32_t length) {
    return address <= IMAGE_BYTES && length <= IMAGE_BYTES - address;
}

static bool access_span(uint32_t address, uint8_t* bytes, uint32_t length, bool write, int fill) {
    while(length) {
        const uint16_t block = address / BLOCK_BYTES;
        const uint32_t offset = address % BLOCK_BYTES;
        uint32_t count = BLOCK_BYTES - offset;
        if(count > length) count = length;
        Slot* slot = load(block);
        if(!slot) return false;
        if(write) {
            if(fill >= 0)
                memset(slot->data + offset, fill, count);
            else
                memcpy(slot->data + offset, bytes, count);
            slot->dirty = true;
            any_dirty = true;
        } else {
            memcpy(bytes, slot->data + offset, count);
        }
        if(bytes) bytes += count;
        address += count;
        length -= count;
    }
    return true;
}

static bool create_blank(void) {
    uint8_t blank[BLOCK_BYTES];
    memset(blank, 0xff, sizeof(blank));
    if(!storage_file_open(file, EEPROM_PATH, FSAM_WRITE, FSOM_CREATE_NEW)) return false;
    bool ok = true;
    for(unsigned i = 0; i < BLOCK_COUNT && ok; ++i)
        ok = storage_file_write(file, blank, sizeof(blank)) == sizeof(blank);
    storage_file_close(file);
    return ok;
}

static bool flush(void) {
    if(!mounted || !any_dirty) return true;
    bool ok = true;
    for(unsigned i = 0; i < SLOT_COUNT; ++i)
        if(slots[i].block != NO_BLOCK && slots[i].dirty) ok = write_block(&slots[i]) && ok;
    ok = storage_file_sync(file) && ok;
    if(ok) any_dirty = false;
    return ok;
}

bool flip_eeprom_open(void) {
    if(mounted) return true;
    storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, APP_DATA_PATH(""));
    file = storage_file_alloc(storage);
    slots = malloc(sizeof(Slot) * SLOT_COUNT);
    for(unsigned i = 0; i < SLOT_COUNT; ++i) {
        slots[i].block = NO_BLOCK;
        slots[i].dirty = false;
        slots[i].used = 0;
    }
    FileInfo info;
    const bool exists = storage_common_stat(storage, EEPROM_PATH, &info) == FSE_OK;
    if(exists && info.size != IMAGE_BYTES) {
        /* Refuse to touch a damaged save; the user can restore or remove it. */
        failed = true;
        return false;
    }
    if(!exists && !create_blank()) {
        failed = true;
        return false;
    }
    if(!storage_file_open(file, EEPROM_PATH, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) {
        failed = true;
        return false;
    }
    mounted = true;

    /* Give a fresh image a unique device ID from the STM32's unique ID so
     * peer play can tell two Flipwalkers apart. */
    const uint8_t* uid = furi_hal_version_uid();
    const size_t uid_size = furi_hal_version_uid_size();
    uint8_t id[6] = {0};
    for(size_t i = 0; i < uid_size; ++i)
        id[i % 6] ^= uid[i];
    FlipGlueIdentityRepair(id);
    return flush();
}

void flip_eeprom_service(void) {
    if(!deferred && !batch_depth && any_dirty && !flush()) storage_error();
}

void flip_eeprom_close(void) {
    if(mounted) {
        flush();
        storage_file_close(file);
        mounted = false;
    }
    if(file) storage_file_free(file);
    file = NULL;
    if(storage) furi_record_close(RECORD_STORAGE);
    storage = NULL;
    free(slots);
    slots = NULL;
}

bool flip_eeprom_failed(void) {
    return failed;
}

/* Stick storage hooks called from the game. */
int StickEepromMount(void) {
    return flip_eeprom_open();
}

void StickEepromDefer(int enabled) {
    deferred = enabled != 0;
    if(!deferred && !batch_depth && !flush()) storage_error();
}

void StickEepromBatchBegin(void) {
    ++batch_depth;
}

int StickEepromBatchEnd(void) {
    if(!batch_depth) {
        storage_error();
        return 0;
    }
    --batch_depth;
    return 1;
}

int StickEepromCommit(void) {
    return flush();
}

/* Sprite tagging: the game reads Pokemon artwork from a few fixed EEPROM
 * areas into RAM and blits it from there. Remember those buffers so the
 * display emulation can mark the pixels they produce. */
typedef struct {
    const uint8_t* ptr;
    uint32_t len;
    uint8_t kind; /* FLIP_TAG_SPRITE or FLIP_TAG_SCENE */
} SpriteBuffer;
#define SPRITE_BUFFERS 8
static SpriteBuffer sprite_buffers[SPRITE_BUFFERS];
static unsigned sprite_next;

/* Which kind of picture lives at an EEPROM address: Pokemon sprites, or the
 * route's 32x24 scene behind them. Everything else (text, icons) is UI. */
static uint8_t image_kind(uint32_t address, uint32_t length) {
    static const struct { uint32_t start, len; uint8_t kind; } areas[] = {
        {EEPROM_COURSE + offsetof(CourseResources, pokemonImage), POKEMON_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_COURSE + offsetof(CourseResources, pokemonImageLarge), POKEMON_LARGE_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_COURSE + offsetof(CourseResources, encounterImages), COURSE_ENCOUNTERS * POKEMON_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_COURSE + offsetof(CourseResources, joiningPokemonImageLarge), POKEMON_LARGE_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_EVENT_POKEMON + offsetof(EventPokemon, pokemonImage), POKEMON_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_BONUS_COURSE + offsetof(BonusResources, pokemonImage), POKEMON_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_PEER_IMAGE, POKEMON_ANIMATION_BYTES, FLIP_TAG_SPRITE},
        {EEPROM_COURSE + offsetof(CourseResources, background), sizeof(((CourseResources*)0)->background), FLIP_TAG_SCENE},
        {EEPROM_BONUS_COURSE + offsetof(BonusResources, background), sizeof(((BonusResources*)0)->background), FLIP_TAG_SCENE},
    };
    for(unsigned i = 0; i < COUNT_OF(areas); ++i)
        if(address >= areas[i].start && address + length <= areas[i].start + areas[i].len) return areas[i].kind;
    return 0;
}

static void note_read(uint32_t address, const void* destination, uint32_t length) {
    /* A later read into the same RAM replaces whatever was there. */
    for(unsigned i = 0; i < SPRITE_BUFFERS; ++i) {
        SpriteBuffer* b = &sprite_buffers[i];
        if(b->len && (const uint8_t*)destination < b->ptr + b->len &&
           b->ptr < (const uint8_t*)destination + length)
            b->len = 0;
    }
    const uint8_t kind = image_kind(address, length);
    if(!kind) return;
    sprite_buffers[sprite_next] = (SpriteBuffer){destination, length, kind};
    sprite_next = (sprite_next + 1) % SPRITE_BUFFERS;
}

uint8_t flip_eeprom_buffer_kind(const void* ptr) {
    for(unsigned i = 0; i < SPRITE_BUFFERS; ++i) {
        const SpriteBuffer* b = &sprite_buffers[i];
        if(b->len && (const uint8_t*)ptr >= b->ptr && (const uint8_t*)ptr < b->ptr + b->len) return b->kind;
    }
    return 0;
}

/* M95512 transfers, as in stickwalker/stick/eeprom_backend.cpp. */
void EepromRead(u16 address, void* destination, u16 length) {
    StickEepromSetError(0);
    if(!destination) return;
    if(!mounted || !range_ok(address, length) ||
       !access_span(address, destination, length, false, -1)) {
        storage_error();
        memset(destination, 0xff, length);
        return;
    }
    note_read(address, destination, length);
}

void EepromWrite(u16 address, void* source, u16 length) {
    StickEepromSetError(0);
    if(!mounted || !source || !range_ok(address, length) ||
       !access_span(address, source, length, true, -1))
        storage_error();
}

u8 EepromReadByte(u16 address) {
    u8 value = 0xff;
    EepromRead(address, &value, 1);
    return value;
}

u8 StickEepromEventByte(void);

u8 EepromWriteByte(u16 address, u8 value) {
    EepromWrite(address, &value, 1);
    return StickEepromEventByte();
}

void EepromFillPage(u16 address, u8 value) {
    EepromFill(address, PAGE_BYTES, value);
}

void EepromFill(u16 address, u16 count, u8 value) {
    StickEepromSetError(0);
    if(!mounted || !range_ok(address, count) || !access_span(address, NULL, count, true, value))
        storage_error();
}

void EepromWritePage(uint address, u8* source) {
    EepromWrite(address, source, PAGE_BYTES);
}
