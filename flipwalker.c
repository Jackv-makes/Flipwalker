/* The Pokewalker on a Flipper Zero.
 *
 * The game is lumirth's binary-matching reconstruction of the Pokewalker
 * firmware, built through stickwalker's PW_STICK_S3 hardware seam; the
 * adapters in port/ stand in for the Stick S3. This file is the Flipper app
 * shell: screen, keys, backlight and a small settings menu.
 *
 * Keys: Left/OK/Right are the walker's three buttons. Back opens the
 * Flipwalker menu; hold Back to save and exit. */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "port/flip_port.h"
#include "port/flip_runtime.h"

#define BUTTON_CENTER 0x02
#define BUTTON_LEFT   0x04
#define BUTTON_RIGHT  0x08

typedef enum {
    MenuBacklight,
    MenuShading,
    MenuAccel,
    MenuIr,
    MenuExit,
    MenuCount,
} MenuRow;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    NotificationApp* notifications;
    uint8_t pixels[FLIP_SCREEN_W * FLIP_SCREEN_H];
    uint8_t xbm[FLIP_XBM_BYTES];
    volatile bool menu_open;
    volatile uint8_t menu_row;
    volatile bool screen_on;
    /* Backlight stays off even while the walker's screen is on; the LCD is
     * readable in daylight and the backlight is a large share of the drain. */
    volatile bool backlight_off;
    volatile uint32_t last_key_tick; /* saver mode keeps the light on briefly after a press */
    volatile FlipDither dither;
    volatile bool gray_phase;
    const char* volatile fatal;
    bool running;
    uint8_t swallowed_keys;
    /* Gray mode draws straight to the LCD so the two patterns alternate
     * strictly, frame for frame, instead of through the GUI thread. */
    Canvas* direct;
    FuriPubSub* input_events;
    FuriPubSubSubscription* direct_sub;
    uint32_t direct_period_ms;
    uint32_t direct_last_cycles;
    /* Gray mode: both dither phases rendered once per picture change, so a
     * frame is just a commit. */
    uint8_t phase_xbm[2][FLIP_XBM_BYTES];
    volatile bool phase_valid;
} Flipwalker;

static void draw_game(Canvas* canvas, Flipwalker* app) {
    flip_display_copy(app->pixels);
    if(app->dither == FlipDitherGray) app->gray_phase = !app->gray_phase;
    flip_render_xbm(app->pixels, app->xbm, app->dither, app->gray_phase);
    canvas_draw_xbm(canvas, 16, 0, FLIP_SCREEN_W, FLIP_SCREEN_H, app->xbm);
    /* Bezel lines either side of the walker's panel. */
    canvas_draw_line(canvas, 14, 0, 14, 63);
    canvas_draw_line(canvas, 113, 0, 113, 63);
}

static void draw_menu(Canvas* canvas, Flipwalker* app) {
    char line[40];
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Flipwalker");
    canvas_set_font(canvas, FontSecondary);
    for(unsigned row = 0; row < MenuCount; ++row) {
        switch(row) {
        case MenuBacklight:
            snprintf(
                line, sizeof(line), "Backlight: %s", app->backlight_off ? "off (saver)" : "auto");
            break;
        case MenuShading: {
            static const char* const names[FlipDitherCount] = {"solid", "gray"};
            snprintf(line, sizeof(line), "Shading: %s", names[app->dither]);
            break;
        }
        case MenuAccel:
            snprintf(line, sizeof(line), "Accel: %s (OK: rescan)", flip_motion_name());
            break;
        case MenuIr:
            if(flip_ir_link() != FlipIrLinkNone)
                snprintf(line, sizeof(line), "IR: %s", flip_ir_status());
            else
                snprintf(line, sizeof(line), "IR: %s (OK: rescan)", flip_ir_name());
            break;
        default:
            snprintf(line, sizeof(line), "Save & exit");
            break;
        }
        const int y = 21 + (int)row * 10;
        if(row == app->menu_row) {
            canvas_draw_box(canvas, 0, y - 8, 128, 10);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 3, y, line);
        canvas_set_color(canvas, ColorBlack);
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    Flipwalker* app = context;
    canvas_clear(canvas);
    if(app->fatal) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Flipwalker");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 28, app->fatal);
        canvas_draw_str(canvas, 2, 60, "Press Back to exit");
    } else if(app->menu_open) {
        draw_menu(canvas, app);
    } else if(app->screen_on) {
        draw_game(canvas, app);
    }
}

static void queue_input(Flipwalker* app, const InputEvent* event) {
    app->last_key_tick = furi_get_tick();
    furi_message_queue_put(app->input_queue, event, 0);
}

/* View port input. The GUI keeps routing keys here during direct draw once
 * a press has made this view port the "ongoing input" owner, and that owner
 * is never cleared, so without this check every key would arrive twice in
 * gray mode (once from here, once from the raw subscription) and a Back
 * press would open the menu and close it again. */
static void input_callback(InputEvent* event, void* context) {
    Flipwalker* app = context;
    if(app->direct) return;
    queue_input(app, event);
}

/* IR status LED: green while packets flow cleanly, red when the link is
 * struggling, off outside a session. */
static void ir_led_service(Flipwalker* app) {
    static FlipIrLink shown = FlipIrLinkNone;
    const FlipIrLink link = flip_ir_link();
    if(link == shown) return;
    shown = link;
    notification_message(app->notifications,
                         link == FlipIrLinkGood ? &sequence_set_only_green_255 :
                         link == FlipIrLinkPoor ? &sequence_set_only_red_255 :
                                                  &sequence_reset_rgb);
}

static void frame_callback(void* context) {
    Flipwalker* app = context;
    app->phase_valid = false;
    /* The GUI cannot redraw while we hold the display; asking it to only
     * logs a lockup warning. */
    if(!app->direct) view_port_update(app->view_port);
}

/* While the canvas is ours the GUI delivers no view port input, so take the
 * raw events instead. */
static void direct_input(const void* value, void* context) {
    queue_input(context, value);
}

static void direct_frame(Flipwalker* app) {
    if(!app->phase_valid) {
        flip_display_copy(app->pixels);
        flip_render_xbm(app->pixels, app->phase_xbm[0], FlipDitherGray, false);
        flip_render_xbm(app->pixels, app->phase_xbm[1], FlipDitherGray, true);
        app->phase_valid = true;
    }
    app->gray_phase = !app->gray_phase;
    canvas_reset(app->direct);
    canvas_draw_xbm(app->direct, 16, 0, FLIP_SCREEN_W, FLIP_SCREEN_H, app->phase_xbm[app->gray_phase]);
    canvas_draw_line(app->direct, 14, 0, 14, 63);
    canvas_draw_line(app->direct, 113, 0, 113, 63);
    canvas_commit(app->direct);
}

static void direct_enter(Flipwalker* app) {
    app->direct = gui_direct_draw_acquire(app->gui);
    app->phase_valid = false;
    app->direct_last_cycles = DWT->CYCCNT;
    app->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    app->direct_sub = furi_pubsub_subscribe(app->input_events, direct_input, app);
    /* Find out how fast the panel really takes frames, then run just under
     * that so the alternation stays locked to the refresh. Measured once;
     * the game is not stepped while this runs, so keep it to a few frames. */
    if(!app->direct_period_ms) {
        const uint32_t start = DWT->CYCCNT;
        for(unsigned i = 0; i < 4; ++i) direct_frame(app);
        const uint32_t per_frame_us = (DWT->CYCCNT - start) / 4 / furi_hal_cortex_instructions_per_microsecond();
        uint32_t fps = per_frame_us ? 1000000u / per_frame_us : 1000;
        FURI_LOG_I("flipwalker", "direct draw: %lu us per commit, %lu fps max", (unsigned long)per_frame_us, (unsigned long)fps);
        if(fps > 60) fps = 60;
        if(fps < 10) fps = 10;
        app->direct_period_ms = 1000u / fps;
    }
}

static void direct_leave(Flipwalker* app) {
    if(!app->direct) return;
    /* Hand input back to the view port before the raw feed stops, so no
     * press falls between the two. */
    app->direct = NULL;
    furi_pubsub_unsubscribe(app->input_events, app->direct_sub);
    furi_record_close(RECORD_INPUT_EVENTS);
    gui_direct_draw_release(app->gui);
    view_port_update(app->view_port);
}

static uint8_t key_bit(InputKey key) {
    switch(key) {
    case InputKeyLeft:
        return BUTTON_LEFT;
    case InputKeyOk:
        return BUTTON_CENTER;
    case InputKeyRight:
        return BUTTON_RIGHT;
    default:
        return 0;
    }
}

static void menu_input(Flipwalker* app, const InputEvent* event) {
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return;
    switch(event->key) {
    case InputKeyUp:
        app->menu_row = (app->menu_row + MenuCount - 1) % MenuCount;
        break;
    case InputKeyDown:
        app->menu_row = (app->menu_row + 1) % MenuCount;
        break;
    case InputKeyBack:
        app->menu_open = false;
        flip_runtime_wake();
        if(app->dither == FlipDitherGray && app->screen_on) return; /* direct draw takes over */
        break;
    case InputKeyOk:
    case InputKeyLeft:
    case InputKeyRight:
        if(app->menu_row == MenuBacklight) {
            app->backlight_off = !app->backlight_off;
            app->screen_on = !app->screen_on; /* force update_backlight to reapply */
        } else if(app->menu_row == MenuShading)
            app->dither = (app->dither + 1) % FlipDitherCount;
        else if(app->menu_row == MenuAccel)
            flip_motion_init();
        else if(app->menu_row == MenuIr)
            flip_ir_probe();
        else if(app->menu_row == MenuExit && event->key == InputKeyOk)
            app->running = false;
        break;
    default:
        break;
    }
    view_port_update(app->view_port);
}

static void handle_input(Flipwalker* app, const InputEvent* event) {
    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        app->running = false;
        return;
    }
    if(app->fatal) {
        if(event->key == InputKeyBack && event->type == InputTypeShort) app->running = false;
        return;
    }
    if(app->menu_open) {
        menu_input(app, event);
        return;
    }
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        app->menu_open = true;
        app->menu_row = 0;
        flip_input_transition(BUTTON_LEFT | BUTTON_CENTER | BUTTON_RIGHT, false);
        view_port_update(app->view_port);
        return;
    }
    const uint8_t bit = key_bit(event->key);
    if(!bit) return;
    if(event->type == InputTypePress) {
        if(!flip_runtime_screen_powered()) {
            /* A dark walker wakes on any button; that press is not also
             * delivered to the game. */
            app->swallowed_keys |= bit;
            flip_runtime_wake();
            return;
        }
        flip_input_transition(bit, true);
    } else if(event->type == InputTypeRelease) {
        if(app->swallowed_keys & bit) {
            app->swallowed_keys &= (uint8_t)~bit;
            return;
        }
        flip_input_transition(bit, false);
    }
}

static void update_backlight(Flipwalker* app) {
    const bool on = flip_runtime_screen_powered() || app->menu_open || app->fatal;
    if(on == app->screen_on) return;
    app->screen_on = on;
    if(on && !app->backlight_off) {
        notification_message(app->notifications, &sequence_display_backlight_enforce_on);
    } else {
        notification_message(app->notifications, &sequence_reset_rgb);
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
        notification_message(app->notifications, &sequence_display_backlight_off);
    }
    view_port_update(app->view_port);
}

/* Backlight and shading choices, kept next to the save. */
#define SETTINGS_PATH  APP_DATA_PATH("settings.bin")
#define SETTINGS_MAGIC 0x46575331u /* FWS1 */
typedef struct {
    uint32_t magic;
    uint8_t backlight_off;
    uint8_t dither;
    uint8_t reserved[2];
} Settings;

static void settings_load(Flipwalker* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    Settings s;
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_read(file, &s, sizeof(s)) == sizeof(s) && s.magic == SETTINGS_MAGIC) {
        app->backlight_off = s.backlight_off != 0;
        app->dither = s.dither < FlipDitherCount ? (FlipDither)s.dither : FlipDitherSolid;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void settings_save(Flipwalker* app) {
    Settings s = {.magic = SETTINGS_MAGIC, .backlight_off = app->backlight_off, .dither = app->dither};
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS))
        storage_file_write(file, &s, sizeof(s));
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void process_input(Flipwalker* app, uint32_t wait_ms) {
    InputEvent event;
    if(furi_message_queue_get(app->input_queue, &event, furi_ms_to_ticks(wait_ms)) !=
       FuriStatusOk)
        return;
    do {
        handle_input(app, &event);
    } while(app->running && furi_message_queue_get(app->input_queue, &event, 0) == FuriStatusOk);
}

/* Saver mode: the notification service lights the backlight on every key
 * press and nothing can stop that, so leave it lit while keys are being
 * pressed and switch it off a few seconds after the last one. */
#define SAVER_HOLD_MS 1000
static void saver_backlight_service(Flipwalker* app) {
    static bool lit;
    if(!app->backlight_off || app->menu_open) {
        lit = false;
        return;
    }
    const uint32_t since = furi_get_tick() - app->last_key_tick;
    if(since < furi_ms_to_ticks(SAVER_HOLD_MS)) {
        lit = true;
    } else if(lit) {
        notification_message(app->notifications, &sequence_display_backlight_off);
        lit = false;
    }
}

int32_t flipwalker_app(void* p) {
    UNUSED(p);
    Flipwalker* app = malloc(sizeof(Flipwalker));
    memset(app, 0, sizeof(Flipwalker));
    app->running = true;
    app->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    flip_display_set_callback(frame_callback, app);
    settings_load(app);

    if(flip_runtime_boot(furi_hal_rtc_get_timestamp())) {
        flip_ir_probe();
        while(app->running) {
            uint32_t wait_ms = flip_runtime_step();
            update_backlight(app);
            saver_backlight_service(app);
            ir_led_service(app);
            if(flip_ir_link() != FlipIrLinkNone && wait_ms > 100) wait_ms = 100;
            if(app->backlight_off && wait_ms > 250) wait_ms = 250;
            const bool want_direct = app->dither == FlipDitherGray && app->screen_on && !app->menu_open;
            if(want_direct && !app->direct) direct_enter(app);
            if(!want_direct && app->direct) direct_leave(app);
            if(app->direct) {
                /* The commit itself takes most of a period; wait only for
                 * what is left of it, so the frame rate is what was measured. */
                direct_frame(app);
                const uint32_t now = DWT->CYCCNT;
                const uint32_t per_ms = furi_hal_cortex_instructions_per_microsecond() * 1000u;
                const uint32_t spent_ms = (now - app->direct_last_cycles) / per_ms;
                app->direct_last_cycles = now;
                uint32_t left = spent_ms < app->direct_period_ms ? app->direct_period_ms - spent_ms : 1;
                if(wait_ms > left) wait_ms = left;
            }
            process_input(app, wait_ms);
        }
        direct_leave(app);
        flip_runtime_shutdown(furi_hal_rtc_get_timestamp());
        settings_save(app);
    } else {
        app->fatal = flip_eeprom_failed() ? "Save file is damaged" : "SD card unavailable";
        update_backlight(app);
        view_port_update(app->view_port);
        while(app->running)
            process_input(app, 1000);
        flip_eeprom_close();
    }

    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    free(app);
    return 0;
}
