#pragma once

namespace services::web {

/** Mount the LittleFS partition holding the web assets. */
bool begin();

/** Start/stop the STA-mode web server. */
void startStaServer();
void stopStaServer();
void handleClient();
bool serverActive();

}  // namespace services::web
