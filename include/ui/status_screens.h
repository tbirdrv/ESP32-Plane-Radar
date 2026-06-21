#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();

/** IP Address display screen with countdown timer. */
void statusScreenIPAddressBegin();
/** Returns true if screen should still be displayed, false if timeout expired. */
bool statusScreenIPAddressTick();

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
