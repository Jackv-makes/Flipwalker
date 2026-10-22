#ifndef PW_STICK_DISPLAY_BUS_H
#define PW_STICK_DISPLAY_BUS_H

#include "pw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The NT7508 drawing code uses these H8 register fields for command/data
 * selection and ready polling. StickDisplayWrite sends the resulting bytes to
 * the emulated display. */
typedef struct {
  struct { u8 B0, B1; } BIT;
} StickPdr1;
typedef struct { StickPdr1 PDR1; } StickDisplayIo;
typedef struct {
  struct { u8 BYTE; } SSER;
  struct { struct { u8 TDRE, TEND; } BIT; } SSSR;
} StickDisplaySsu;

extern StickDisplayIo IO;
extern StickDisplaySsu SSU;
void StickDisplayWrite(u8 value);
/* Pixels written between these calls carry a sprite tag when the raster was
 * read from Pokemon artwork in EEPROM. StickDisplayFrame reports the tag in
 * bit 2 so a one-bit renderer can shade sprites and keep text solid. */
void StickDisplayBlitBegin(const u8 *raster);
void StickDisplayBlitEnd(void);
void StickDisplayBusInit(void);
int StickDisplayIsPowered(void);
/* Read the panel's selected 96x64 view as four-shade palette indexes. */
void StickDisplayFrame(u8 *pixels, u16 byteCount);

#ifdef __cplusplus
}
#endif

#endif
