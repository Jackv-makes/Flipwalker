#ifndef PW_STICK_FOREGROUND_BRIDGE_H
#define PW_STICK_FOREGROUND_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

void StickForegroundQuarterSecond(void);
void StickForegroundSecond(void);
int StickForegroundIsIr(void);
int StickForegroundIsMain(void);
int StickForegroundIsBeep(void);
int StickForegroundIsInactive(void);
unsigned StickForegroundUiFrame(void);
void StickForegroundRun(void);
void StickForegroundRequestIr(void);
void StickForegroundDeviceMenuClosed(void);
void StickForegroundWakeDisplay(void);
void StickForegroundCenterWake(void);
unsigned StickForegroundIrResult(void);
void StickForegroundIrDiagnostic(unsigned *phase, unsigned *received,
                                 unsigned *reference);

#ifdef __cplusplus
}
#endif

#endif
