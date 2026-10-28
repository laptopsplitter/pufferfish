# tonic: developer notes

How tonic works inside, for people changing it. Using it: see the
[README](../README.md).

## Building from the folder

`./install.sh` installs the dependencies, builds and installs. While working
on tonic you can also run it straight from the source folder, without
installing:

    make -j$(nproc)             # FreeBSD: gmake -j$(sysctl -n hw.ncpu)
    make webview
    ./run.sh

Run that way, everything tonic keeps is in `data/` in the source folder;
installed, it is in `${XDG_DATA_HOME:-~/.local/share}/tonic/data`. Same
layout, so either can be copied to the other (`./install.sh --import DIR`).

`make install PREFIX=/usr/local DESTDIR=...` is what packages use: the
launcher in `bin/`, the programs in `libexec/tonic/`, assets and scripts in
`share/tonic/`, a desktop entry and icon. `make test` runs the tests with
stand-in libraries (no Roblox needed).

## Ground rules

These are fixed and not up for relaxing later:

- Runs the **official, unmodified** APK, downloaded by tonic and checked
  against Roblox's signature. No Roblox code or assets are ever shipped in
  this repo.
- **No injection, hooking or patching of game code.** No script execution.
- **No anti-cheat bypassing or spoofing.** If the official client refuses to run
  in this environment, that is the answer; we do not work around it.
- **No reimplementation of Roblox's network protocol.** Only the official
  client talks to Roblox's servers.
- Development and testing happen on a **throwaway account** with no Robux, no
  payment method and nothing of value. Unofficial and unsupported: it can break
  at any time and Roblox has not approved it.

## Only Roblox's own APK

The APK is installed only if it is Roblox's, unchanged (`src/apksig.cpp`):
its APK Signature Scheme v3 (or v2) signature is checked the way Android
checks an app before installing it (the signature over the whole file,
verified with the signer's key, using OpenSSL), and the signer's certificate
must be one of Roblox's, pinned by SHA-256. A changed byte, or any other
signer, and it is refused.

And at every start, before any of Roblox's code is loaded: the signed APKs
are kept with the installed version (`data/versions/VERSION/apk/`), their
signatures are checked again, and every installed file is compared, byte for
byte, with the APK's own copy (`src/apkfiles.cpp`, tonic's own zip reader,
the same one that unpacked it). A file changed, missing or added, and that
install is removed and a fresh one downloaded. Roblox's trust store for its
connections (`data/files/exe/cacert.pem`) is put back from the checked assets
each time. The check takes about a second. `./tonic --apk-check FILE` shows
what tonic sees in an APK. Should Roblox ever change its signing key before tonic is
updated, `TONIC_TRUSTED_CERT=<sha256>` adds one more certificate.

The download (`src/apk.cpp`) uses APKPure's app API as Mocktail and apkeep do:
one request for the x86_64 build; only links on APKPure's own hosts are
followed. `TONIC_NO_APK_DOWNLOAD=1`: the setup page does not download by
itself.

Everything tonic keeps is in `data/`: the installed APK, your sign-in
(0600), settings and Roblox's app storage.

## How it works

1. **Loader** (`src/elfimg.cpp`): maps the APK's x86_64 `libroblox.so` itself
   and resolves its imports against tonic's own symbol table first
   (`src/shims.cpp`: where bionic differs from glibc, e.g. sysconf numbers,
   `sigset_t`/`sigaction`, `jmp_buf`, `FILE`, `struct addrinfo`), then the
   host's libc. Its constructors run as on Android.
2. **Fake JVM / JNI** (`src/fakejni.cpp`): the Java side the library expects.
   One JNIEnv per thread, attached as on Android (the library wraps each
   env's function table in place). What "Java" returns is in
   `scripts/roblox.values` (`--values`); anything not listed is 0/null.
3. **Startup** (`scripts/startup.script`): the natives the app calls at
   launch, in its order, with typed arguments (format in `src/script.hpp`).
   The Lua app's message bus requests are answered there as Java would.
4. **Window and graphics** (`src/window.cpp`, `src/vkshim.cpp`): an SDL2
   window stands in for the Android Surface; EGL/GLES or Vulkan go to the
   host's Mesa or driver, with only the Android-specific calls translated.
5. **Input** (`src/input.cpp`): mouse, keyboard, text and gamepads reach the
   library through the same natives the Android app uses. TextBoxes work the
   Android way (Java's keyboard, `nativePassText`).
6. **Sign-in and WebView** (`src/weblogin.cpp`, `src/webview.cpp`,
   `tonic-webview`): roblox.com in a WebKitGTK window of tonic's own, and the
   pages the app opens (captchas and other challenges), passed both ways
   through the bridge Roblox's Android WebView offers. You solve challenges
   yourself; nothing is automated. The app's own password sign-in can ask for
   a device check (Google Play Integrity) that nothing on Linux passes; tonic
   does not fake one.
7. **Playing** (`src/game.cpp`, `src/mainloop.cpp`): Play publishes
   `Game.launch`; tonic answers as the Java side does, on the app's main loop.
8. **Audio** (`src/audio.cpp`): FMOD plays through Java's AudioTrack, which
   tonic implements on SDL audio (PipeWire/PulseAudio). `TONIC_NO_AUDIO=1`
   turns it off.

Client settings (`src/clientsettings.cpp`): at every start tonic downloads
the settings the Android app fetches (`clientsettingscdn.roblox.com`,
AndroidApp) and merges `scripts/settings-overrides.json` over them into
`data/settings/` (offline: the last ones it got). Server-side
rollouts matter: with defaults only, FIntLuaAppLoginRollout is 0 and the Lua
app hands login to the Android UI instead of showing its own.

## Roblox's UI and rumble

tonic plays the Android side, so what Roblox asks "the phone" is tonic's to
answer (src/device.cpp, src/haptics.cpp). Settings > Look:

- Roblox UI size: Android's display density (dpiScale). Roblox measures its
  UI in density units, so 180 % makes its menus, buttons and top bar 1.8x.
  Its icons come in 1x/2x/3x sheets and stretching one up shows its
  neighbours, so only 100, 160-200 and 260-300 % are offered (under 100 % Roblox's
  rounded corners come out square).
- Edge margin: told to Roblox as Android's safe-area insets, so its buttons
  and menus keep off the window's edges (Auto: 16 dp when the UI is bigger).
- Typed text follows the UI size (text box sizes are density units, as on
  Android).
- Roblox layout: phone or tablet (Roblox's tablet layouts).
- Roblox theme: the Roblox app takes its theme from your account, so tonic
  sets it there (accountsettings.roblox.com, as roblox.com's settings page
  does; src/account.cpp). The session goes to curl in a 0600 file, never on
  its command line.

Settings > Controls > Rumble: games that vibrate the phone (Roblox's
HapticProtocol) rumble your controllers instead, at a strength you pick.

Settings > Controls > Caffeinate, an anti-AFK (off by default): Roblox disconnects you after
20 minutes without input. With this on, after 5 minutes with no mouse, key or
gamepad input from you, tonic moves the mouse one pixel and straight back,
through the same path your mouse uses, and again every 5 minutes while you
stay away. The cursor and camera end up exactly where they were; no button or
key is ever pressed, and nothing happens while you play. Each nudge is logged
as `[input] caffeinate: ...`. `TONIC_AFK_SECS=N` makes it N seconds, to try it.

## Upscaling and frame generation (Vulkan)

Settings > Performance. Only the finished picture is used: nothing inside the
game is read or changed.

- FSR 1 upscaling (AMD FidelityFX Super Resolution 1) or NVIDIA Image Scaling
  (NIS): with a Resolution cap (General), Roblox draws fewer pixels and tonic
  enlarges each frame with one of these instead of a plain stretch: sharper,
  at almost no cost. One at a time; each has a sharpness setting. Any GPU.
  (src/upscale.cpp; shaders in shaders/, `make shaders` rebuilds
  src/upscale_spv.h after changing them; `make test-upscale` checks both.)
- NVIDIA Smooth Motion: the NVIDIA driver's frame generation (driver 575 or
  newer, GeForce RTX 40 series or newer). tonic checks the GPU and the driver's
  layer, switches to Vulkan and sets NVPRESENT_ENABLE_SMOOTH_MOTION=1 when you
  press Play (src/framegen.cpp). Roblox and its FPS counters still count the
  real frames; tonic's stats overlay says when about twice are shown.

## Stats overlay

Settings > General > Stats overlay, or F9 in a game: one line of text at the
top left, on a strip at 35 % opacity. With Vulkan there is no strip, just the
text and the line over the game with a thin dark outline (only their own
pixels are written into the frame, so nothing has to be read back from the GPU).

- FPS (green smooth, yellow uneven, red slow) and MS, the frame time.
- AVG: the average frame rate in this game, from 20 s after joining (its
  loading left out), only while tonic's window is in front, without stalls
  over half a second (a teleport's loading).
- PING: the round trip to the game server. tonic's libc layer notes where
  the game's traffic goes (the address only, never what is sent), and the
  system's `ping` measures the busiest server every 2 seconds, only while the
  overlay shows. A server that ignores ping gets a TCP connection attempt
  instead, timed until it answers (a connection or a refusal: either is one
  trip there and back). That is the network's part of Roblox's own ping,
  which adds the server's frame time. "no reply": the server answers neither.
- CPU: tonic's share of all your CPU cores.
- WORST: the slowest frame of the last quarter second.
- The renderer (OpenGL or Vulkan).

Under the text, the frame rate of each quarter second of the last minute as
a single line: a new point four times a second on the right, the rest moving
one step left (green 55 and up, yellow from 28, red below).

## Roblox links

Settings > General > "Open Roblox links in tonic" makes your desktop open
`roblox://` and `roblox-player:` links with tonic (a `tonic-links.desktop` in
`~/.local/share/applications`, set with `xdg-mime`; Off removes both). Then Play
on roblox.com in your browser starts tonic and joins that game, or, if tonic
is already open, the open one joins (in a game it leaves it first). In the
launcher, Ctrl+V joins a copied game link, private server links included.

Only the place, a user to follow and a private server link code are kept from
a link. A website Play link also carries a one-time sign-in ticket: tonic drops
it unread, and it never reaches the log (the launcher passes the link in
the environment, not on the command line).

## Security notes

- `data/` and each instance's folders are private (0700): the cookie file
  (0600), Roblox's app storage and the sign-in window's web data are in there.
- Helper programs (curl, xdg-open, xdg-mime) are started without a shell.
  curl only speaks HTTPS, redirects included.
- The browser window: only Roblox's own https pages can call into tonic; other
  sites' calls are dropped. `file:` pages, downloads, other apps' links and new
  windows are refused, as are camera, microphone, location and notification
  requests.
- "Block Roblox analytics" (Settings > Performance, off by default): Roblox's
  reporting hosts (analytics events, telemetry, tracing, crash and error
  reports) get "no such host", like a DNS blocklist; nothing else is blocked
  (src/privacy.cpp, `make test-privacy`). The Android app's third-party
  trackers (AppsFlyer, Firebase and others) are Java and never run in tonic.
- Built with stack protection, `_FORTIFY_SOURCE` and full RELRO.

## FreeBSD

Native (no Linuxulator). Build with GNU make:

    pkg install gmake pkgconf curl sdl2 freetype2 png wayland libX11 vulkan-headers vulkan-loader epoll-shim mesa-libs webkit2-gtk_41
    gmake -j$(sysctl -n hw.ncpu)
    gmake test-bsdabi && gmake test

Roblox's library is built for bionic on Linux. On Linux, `src/shims.cpp` only
has to cover where bionic and glibc differ, because both use the kernel's
numbers. FreeBSD's libc has its own numbers and layouts for errno, signals,
open/mmap/socket flags, `sockaddr`, `struct stat`, `dirent`, clock ids,
`pthread_once_t`, mutex types, `mbstate_t` and more, so `src/bsdabi.cpp`
(FreeBSD only; the Linux build does not include it) converts each call both
ways. It only translates. Anything the library asks about the system gets
FreeBSD's real answer (`uname` says FreeBSD), and anything with no FreeBSD
equivalent fails the way an older Linux kernel would, with one `[bsdabi] ...`
line in the log. `gmake test-bsdabi` calls the translations with Linux
numbers, the way the library does.

`/proc/meminfo`, `/proc/cpuinfo`, `/proc/self/{statm,maps,status,stat,auxv,
cmdline,comm}` and every process's `cmdline` are answered with FreeBSD's own
figures in Linux's format (as FreeBSD's linprocfs does), and `/proc/self/exe`
with tonic's own path. Where FreeBSD has no such thing (OOM score,
capabilities) the value is what Linux shows an ordinary process.
`/proc/self/fd` is FreeBSD's `/dev/fd`; mount fdescfs there
(`mount -t fdescfs fdesc /dev/fd`, or in /etc/fstab) or only 0-2 are listed.
The rest of `/proc` is "no such file", with one
`[bsdabi] /proc: the library looked at ...` line in the log for each. `statfs` on `/proc` reports Linux's proc file system
(0x9fa0) even when nothing is mounted there: Roblox checks it, and anything
else disconnects with Error 304 after about a minute. On file systems that cannot preallocate (ZFS),
`posix_fallocate` extends the file instead, as glibc does.

Raw system calls: a few of Roblox's checks skip libc and execute the
`syscall` instruction with Linux's numbers (ktrace shows them as FreeBSD's
`lio_listio` and `utimes`: Linux `openat` 257 and `fstatfs` 138). Inside a
native process those reach the wrong FreeBSD call, and Roblox disconnects with
Error 304 after about a minute. FreeBSD has no way for a program to catch them
itself (Linux has Syscall User Dispatch for this; Wine uses it), so tonic comes
with a small kernel module that adds it, `bsdkmod/tonicsud.c`. tonic tells it
where Roblox's code is; a `syscall` from there reaches tonic as a signal
instead of running, and tonic carries it out with the same translations as the
libc route (`src/bsdsys.cpp`), so it gets the same answer, `/proc` included.
Every other system call runs untouched at full speed. Nothing is traced and
the library is not changed. Each such call is logged once as `[bsdsys] ...`.

Setting it up (once, and again after a kernel update): the kernel sources must
be in `/usr/src` for your exact release. If they are not:

    fetch https://download.freebsd.org/releases/amd64/$(uname -r | cut -d- -f1,2)/src.txz
    sudo tar -C / -xzf src.txz

then `gmake kmod` builds `bsdkmod/tonicsud.ko`; `install.sh` (or `sudo gmake
install-kmod`) puts it in `/boot/modules`, so `kldload tonicsud` loads it
and `sysrc kld_list+=tonicsud` loads it at every boot. When it is not loaded,
tonic opens on a page with the two commands to run (`su -`, then the one that
builds it if need be and loads it), each with a Copy button, and goes on to
the launcher by itself once the module is there. "Continue without it" plays
anyway, but expect Error 304. `TONIC_SYSCALLS=off` leaves
the module unused.

Known gaps: no abstract unix sockets, no netlink.

## Layout

- `src/`: tonic (C++); `src/bsdabi.cpp`, `src/bsdsys.cpp`: FreeBSD only
- `bsdkmod/`: FreeBSD's kernel module (`gmake kmod`)
- `scripts/`: Roblox's startup sequence, what "Java" returns, settings overrides
- `shaders/`: the FSR 1 and NIS upscalers (`make shaders`)
- `tests/`: `make test` and the other `test-*` targets, with stand-in libraries
- `tools/`: helper scripts for reading a new Roblox version, all read-only:
  `apk_inspect.py` (the libraries and the symbols they need), `elf_probe.py`
  (what the loader must handle), `natives.py` and `dexnatives.py` (the
  native methods, their signatures, and the order Java calls them;
  `--script` writes startup lines), `where.py` (what is at an offset),
  `flagref.py` (the FFlag a function reads), `luau/` (the Lua app's
  compiled bytecode, read only)

## Credits

- [Mocktail](https://github.com/TheRealUwie/mocktail) (Apache-2.0), an
  open-source runtime for the Android Roblox client on Linux and FreeBSD, and
  its fork [Mocktail Plus](https://github.com/blackixxce12/mocktail): the idea
  of signing in on Roblox's website in a browser window of the client's own,
  instead of the app's sign-in screen, comes from Mocktail Plus. tonic's code
  for it (`src/weblogin.cpp`, `tonic-webview --login`) is its own.
- Mocktail also showed the way to the newest Roblox: APKPure's app API, and
  Roblox's signing certificates (its `config/roblox_signing_certificates.json`).
  tonic's downloader and signature check are its own code.
- [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  (MIT) and [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling)
  (MIT): their shader code, unchanged, in `shaders/third_party` with their
  licences.
- mcpelauncher and Sober, for showing that running the Android build of a
  game on desktop Linux works.
