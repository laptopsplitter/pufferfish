# Pufferfish

## (THIS IS VIBECODED!!!)

Play Roblox natively on **OpenBSD**, FreeBSD, and Linux desktops: no Wine, no emulator, no virtual machine. Pufferfish runs Roblox's official x86-64 Android build as a native desktop application with mouse, keyboard, controllers, Vulkan or OpenGL graphics, and native OpenBSD `sndio` audio.

## Install on OpenBSD (or FreeBSD / Linux)

Open a terminal as your normal user and run:

```sh
git clone https://github.com/YOUR_USERNAME/pufferfish.git
cd pufferfish
./installer.sh
```

`installer.sh`:
1. Works out which version of OpenBSD you are on (`7.5`, `7.6`, `7.7`, `-release`, `-stable`, or `-current`),
2. Checks which dependencies you already have and lists **only** the missing packages first (asking `[Y/n]` before running `doas pkg_add`),
3. Verifies `/usr/local` has `wxallowed` enabled and builds `pufferfish` and `pufferfish-webview` with `-Wl,-z,wxneeded`,
4. Installs Pufferfish to `/usr/local` (`/usr/local/bin/pufferfish`, `/usr/local/libexec/pufferfish/`, `/usr/local/share/applications/pufferfish.desktop`, and `/usr/local/share/icons/hicolor/512x512/apps/pufferfish.png`) so it shows up in your desktop app menu under **Games**.
