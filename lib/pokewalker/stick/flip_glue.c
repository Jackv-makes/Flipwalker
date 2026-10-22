/* Flipper-side helpers that need the game's H8-packed records. They are built
 * with the reconstructed sources so struct layout and the 16-bit `uint` match. */
#include "project.h"
#include "flags.h"
#include "eeprom_map.h"
#include "records.h"
#include "application/pw_eeprom_m95512.h"
#include "foreground_bridge.h"
#include "flip_glue.h"
#include <string.h>

void RtcSecondInterrupt(void);
void RtcDispatch(void);

#define FLIP_RTC_PENDING_MASK 7

void FlipGlueCatchUp(unsigned long seconds)
{
  /* Replay the RTC interrupt for every second the app was closed. Minute,
   * hour and day work is dispatched as it would have been on a walker that
   * was left alone, so the diary, step history and Pokemon minutes advance. */
  while (seconds--) {
    RtcSecondInterrupt();
    if (g_state.time.pendingUpdates & FLIP_RTC_PENDING_MASK)
      RtcDispatch();
  }
}

void FlipGlueGetSteps(unsigned long *daily, unsigned *hourly)
{
  *daily = g_state.dailySteps;
  *hourly = g_state.hourSteps;
}

void FlipGlueSetSteps(unsigned long daily, unsigned hourly)
{
  g_state.dailySteps = daily;
  g_state.hourSteps = (u16)hourly;
}

void FlipGluePersistSave(void)
{
  /* The walker saves hourly; write the current counters before the app exits
   * so steps, watts and the clock survive closing the app. */
  EepromMirrorWrite(EEPROM_SAVE_PRIMARY, EEPROM_SAVE_BACKUP,
                    (u8 *)&g_state.save, sizeof(SaveData));
}

/* Device identity repair, equivalent to StickIdentityRepair but performed
 * through the EEPROM cache instead of a 64 KiB RAM image. */
static u8 flip_checksum(const u8 *bytes, unsigned count)
{
  u8 sum = 1;
  while (count--)
    sum += *bytes++;
  return sum;
}

static int flip_usable_id(const u8 *id)
{
  unsigned i;
  u8 any = 0, all = 0xff;
  for (i = 0; i < DEVICE_ID_BYTES; ++i) {
    any |= id[i];
    all &= id[i];
  }
  return any != 0 && all != 0xff;
}

int FlipGlueIdentityRepair(const unsigned char uid[6])
{
  const u16 masters[2] = {EEPROM_ID_PRIMARY, EEPROM_ID_BACKUP};
  const u16 statuses[2] = {EEPROM_STATUS_PRIMARY, EEPROM_STATUS_BACKUP};
  u8 id[DEVICE_ID_BYTES + 1];
  u8 record[DEVICE_ID_BYTES + 1];
  u8 status[sizeof(DeviceStatus) + 1];
  unsigned i;
  int found = 0, changed = 0, registered = 0;

  for (i = 0; i < 2 && !found; ++i) {
    EepromRead(masters[i], record, sizeof(record));
    if (flip_checksum(record, DEVICE_ID_BYTES) == record[DEVICE_ID_BYTES] &&
        flip_usable_id(record)) {
      memcpy(id, record, DEVICE_ID_BYTES);
      found = 1;
    }
  }
  for (i = 0; i < 2 && !found; ++i) {
    EepromRead(statuses[i], status, sizeof(status));
    if (flip_checksum(status, sizeof(DeviceStatus)) ==
            status[sizeof(DeviceStatus)] &&
        flip_usable_id(status + offsetof(DeviceStatus, deviceId))) {
      memcpy(id, status + offsetof(DeviceStatus, deviceId), DEVICE_ID_BYTES);
      found = 1;
    }
  }
  if (!found) {
    memset(id, 0, DEVICE_ID_BYTES);
    memcpy(id, "Flipwalker v1", sizeof("Flipwalker v1") - 1);
    memcpy(id + DEVICE_ID_BYTES - 6, uid, 6);
  }
  id[DEVICE_ID_BYTES] = flip_checksum(id, DEVICE_ID_BYTES);
  for (i = 0; i < 2; ++i) {
    EepromRead(masters[i], record, sizeof(record));
    if (memcmp(record, id, sizeof(id)) != 0) {
      EepromWrite(masters[i], id, sizeof(id));
      changed = 1;
    }
  }
  for (i = 0; i < 2; ++i) {
    EepromRead(statuses[i], status, sizeof(status));
    if (flip_checksum(status, sizeof(DeviceStatus)) !=
        status[sizeof(DeviceStatus)])
      continue;
    registered |= status[STATUS_FLAGS_OFFSET] & 1;
    if (memcmp(status + offsetof(DeviceStatus, deviceId), id,
               DEVICE_ID_BYTES) != 0) {
      memcpy(status + offsetof(DeviceStatus, deviceId), id, DEVICE_ID_BYTES);
      status[sizeof(DeviceStatus)] =
          flip_checksum(status, sizeof(DeviceStatus));
      EepromWrite(statuses[i], status, sizeof(status));
      changed = 1;
    }
  }
  if (registered) {
    u16 own = EEPROM_OWN_RECORDS + offsetof(PeerRecords, deviceId);
    EepromRead(own, record, DEVICE_ID_BYTES);
    if (memcmp(record, id, DEVICE_ID_BYTES) != 0) {
      EepromWrite(own, id, DEVICE_ID_BYTES);
      changed = 1;
    }
  }
  return changed;
}
