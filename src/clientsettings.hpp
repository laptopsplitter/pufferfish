#pragma once
// clientsettings.hpp - Roblox's client settings, as the Android app's Java
// side gets them at each start: downloaded (clientsettingscdn.roblox.com,
// AndroidApp), with scripts/settings-overrides.json merged over them, into
// data/settings/ (client-settings.json, AndroidApp.json, preload-overrides.json).
// Without them the sign-in and captcha flows break. Offline, the last ones
// are kept (or the overrides alone are used).

namespace tonic {

void clientsettings_start();  // early: the download, in the background
void clientsettings_wait();   // before Roblox starts: the files are written

}  // namespace tonic
