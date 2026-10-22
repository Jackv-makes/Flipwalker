#ifndef STICK_DEVICE_IDENTITY_H
#define STICK_DEVICE_IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Repair a complete EEPROM image before boot. Returns 1 if it changed.
 * The caller must checkpoint the image before allowing the game to start. */
int StickIdentityRepair(unsigned char *image,
                        const unsigned char factoryMac[6]);

/* A console transfer may carry a record made before identity repair. */
void StickIdentitySyncOwnRecord(void);

#ifdef __cplusplus
}
#endif
#endif
