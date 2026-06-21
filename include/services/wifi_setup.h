#pragma once

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Clear only WiFi credentials (keep location/units), then reboot to setup AP. */
void wifiEraseCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** True when the Wi-Fi manager portal/web server is currently active. */
bool wifiPortalActive();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Latched double-click (two taps within 0.75 seconds). */
bool bootButtonConsumeDoubleClick();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();
