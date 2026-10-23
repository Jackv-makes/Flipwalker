/* Rendering the walker's four-shade frame on the Flipper's one-bit panel. */
#include <string.h>

#include "flip_port.h"

/* The walker has four shades; the Flipper's LCD has one bit. Two ways to
 * show the two grays:
 *   solid: light gray -> white, dark gray -> black, crisp 1-bit look;
 *   gray:  2x2 textures (light = sparse dots, dark = checkerboard) shifted by
 *          one pixel on alternate frames, so each gray pixel is lit for the
 *          right fraction of the time and the slow LCD averages it. */

/* Background mask: white pixels connected to the area outside the sprites.
 * White enclosed inside a sprite (belly, legs) is not background, so shading
 * patches sitting on it must not grow an outline. Propagated by alternating
 * sweeps until stable; a few passes suffice for sprite shapes. */
static uint8_t background[FLIP_XBM_BYTES];

static inline bool bg(unsigned x, unsigned y) {
    return (background[y * (FLIP_SCREEN_W / 8) + x / 8] >> (x & 7)) & 1;
}

static void find_background(const uint8_t* pixels) {
    for(unsigned i = 0; i < FLIP_SCREEN_W * FLIP_SCREEN_H; ++i) {
        const bool white_outside = (pixels[i] & 3) == 0 && !(pixels[i] & (FLIP_TAG_SPRITE << 2));
        if(white_outside) background[i / 8] |= (uint8_t)(1u << (i & 7));
        else background[i / 8] &= (uint8_t)~(1u << (i & 7));
    }
    for(unsigned pass = 0; pass < 24; ++pass) {
        bool changed = false;
        for(unsigned n = 0; n < FLIP_SCREEN_W * FLIP_SCREEN_H; ++n) {
            const unsigned i = (pass & 1) ? FLIP_SCREEN_W * FLIP_SCREEN_H - 1 - n : n;
            const unsigned x = i % FLIP_SCREEN_W, y = i / FLIP_SCREEN_W;
            if((pixels[i] & (3 | (FLIP_TAG_SPRITE << 2))) != (FLIP_TAG_SPRITE << 2)) continue; /* sprite white, not yet known */
            if(bg(x, y)) continue;
            if((x > 0 && bg(x - 1, y)) || (x + 1 < FLIP_SCREEN_W && bg(x + 1, y)) ||
               (y > 0 && bg(x, y - 1)) || (y + 1 < FLIP_SCREEN_H && bg(x, y + 1)) ||
               x == 0 || y == 0 || x + 1 == FLIP_SCREEN_W || y + 1 == FLIP_SCREEN_H) {
                background[i / 8] |= (uint8_t)(1u << (i & 7));
                changed = true;
            }
        }
        if(!changed) break;
    }
}

void flip_render_xbm(const uint8_t* pixels, uint8_t* xbm, FlipDither mode, bool phase) {
    memset(xbm, 0, FLIP_XBM_BYTES);
    find_background(pixels);
    for(unsigned y = 0; y < FLIP_SCREEN_H; ++y) {
        for(unsigned x = 0; x < FLIP_SCREEN_W; ++x) {
            const uint8_t shade = pixels[y * FLIP_SCREEN_W + x] & 3;
            const bool sprite = (pixels[y * FLIP_SCREEN_W + x] & (FLIP_TAG_SPRITE << 2)) != 0;
            const bool scene = (pixels[y * FLIP_SCREEN_W + x] & (FLIP_TAG_SCENE << 2)) != 0;
            bool on;
            /* Sprite outlines are partly gray. A gray sprite pixel next to the
             * background is outline: keep it solid in both modes, or the
             * silhouette breaks into dots or disappears. */
            if(sprite && (shade == 1 || shade == 2)) {
                const bool edge = (x > 0 && bg(x - 1, y)) ||
                                  (x + 1 < FLIP_SCREEN_W && bg(x + 1, y)) ||
                                  (y > 0 && bg(x, y - 1)) ||
                                  (y + 1 < FLIP_SCREEN_H && bg(x, y + 1));
                if(edge) {
                    xbm[y * (FLIP_SCREEN_W / 8) + x / 8] |= (uint8_t)(1u << (x & 7));
                    continue;
                }
            }
            if(mode == FlipDitherGray)
                on = shade == 3 || (shade == 2 && ((x + y + phase) & 1) == 0) ||
                     (shade == 1 && ((x + phase) & 1) == 0 && ((y + phase) & 1) == 0);
            else if(scene)
                /* Route scenes (caves, night) are drawn mostly in gray; solid
                 * thresholding turns them into a block, so texture them. */
                on = shade == 3 || (shade == 2 && ((x + y) & 1) == 0) ||
                     (shade == 1 && (x & 1) == 0 && (y & 1) == 0);
            else
                on = shade >= 2;
            if(on) xbm[y * (FLIP_SCREEN_W / 8) + x / 8] |= (uint8_t)(1u << (x & 7));
        }
    }
}
