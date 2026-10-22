#ifndef PW_STICK_EEPROM_BACKEND_H
#define PW_STICK_EEPROM_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Load the newest valid image, replay its journal and repair the device
 * identity. Format LittleFS when the partition is blank. */
int StickEepromMount(void);
/* Buffer EEPROM writes in RAM during an infrared session. */
void StickEepromDefer(int enabled);
int StickEepromCommit(void);
/* Commit the writes in an EEPROM mirror update together. */
void StickEepromBatchBegin(void);
int StickEepromBatchEnd(void);

#ifdef __cplusplus
}
#endif

#endif
