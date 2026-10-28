#!/bin/sh
# Pufferfish's installer for OpenBSD desktops (including OpenBSD 8.0+).
#
#   ./installer.sh              install (or update, after git pull)
#   ./installer.sh --test       run dry-run dependency & ABI self-tests without installing
#   ./installer.sh --uninstall  remove Pufferfish (your data stays unless you say so)
#   ./installer.sh --yes        do not ask before each step
#   ./installer.sh --import DIR also bring over the data/ folder of a Pufferfish in DIR
#
# On OpenBSD (7.4 through 8.0+):
#   1. Works out which version/release of OpenBSD you are running (e.g. 8.0,
#      7.7, 7.6, -release, -stable, or -current snapshot) and verifies amd64 +
#      /usr/local wxallowed mount flags.
#   2. Checks each build & runtime requirement by what it provides (pkg-config,
#      binaries, headers, pkg_info), lists ONLY the missing packages first,
#      and asks before running `doas pkg_add`.
#   3. Builds pufferfish and pufferfish-webview with `-Wl,-z,wxneeded` and installs
#      them to /usr/local/bin/pufferfish, /usr/local/libexec/pufferfish, and
#      /usr/local/share/applications/pufferfish.desktop so Pufferfish appears in
#      your desktop application menu like any native OpenBSD desktop app.

set -eu

cd "$(dirname "$0")"
here=$(pwd)

mode=install
yes=0
import=
while [ $# -gt 0 ]; do
  case "$1" in
    --uninstall) mode=uninstall ;;
    --test|--dry-run) mode=test ;;
    --yes|-y) yes=1 ;;
    --import) shift; import=${1:-} ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) printf 'unknown option: %s (see --help)\n' "$1" >&2; exit 2 ;;
  esac
  shift
done

if [ -t 1 ]; then b=$(printf '\033[1m'); r=$(printf '\033[0m'); else b=; r=; fi
say() { printf '%s==>%s %s\n' "$b" "$r" "$*"; }
note() { printf '    %s\n' "$*"; }
die() { printf '%serror:%s %s\n' "$b" "$r" "$*" >&2; exit 1; }

ask() {
  [ "$yes" = 1 ] && return 0
  printf '%s [Y/n] ' "$1"
  answer=
  read -r answer </dev/tty || answer=
  case "$answer" in [nN]*) return 1 ;; *) return 0 ;; esac
}

# --- where we are ------------------------------------------------------------------

os=$(uname -s)
arch=$(uname -m)
case "$os" in
  OpenBSD|FreeBSD|Linux) ;;
  *) die "Pufferfish runs on OpenBSD, FreeBSD and Linux, not $os" ;;
esac
case "$arch" in
  x86_64|amd64) ;;
  *) die "Pufferfish needs an x86-64 / amd64 computer (this one is $arch)" ;;
esac

if [ "$(id -u)" = 0 ]; then
  sudo=
else
  if [ "$os" = OpenBSD ] && command -v doas >/dev/null 2>&1; then sudo=doas
  elif command -v sudo >/dev/null 2>&1; then sudo=sudo
  elif command -v doas >/dev/null 2>&1; then sudo=doas
  elif [ "$mode" != "test" ]; then
    die "needs doas or sudo to install packages and copy into /usr/local (or run as root)"
  else
    sudo=doas
  fi
fi
as_root() { if [ -n "$sudo" ]; then "$sudo" "$@"; else "$@"; fi; }

family=
obsd_ver=
obsd_flavor=
if [ "$os" = OpenBSD ]; then
  family=openbsd
  obsd_ver=$(uname -r)
  kern_ver=$(sysctl -n kern.version 2>/dev/null | head -n 1 || echo "OpenBSD $obsd_ver")
  case "$kern_ver" in
    *-current*) obsd_flavor="-current snapshot" ;;
    *-stable*)  obsd_flavor="-stable" ;;
    *-beta*)    obsd_flavor="-beta" ;;
    *)          obsd_flavor="-release" ;;
  esac
  export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/X11R6/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  export PATH="/usr/local/bin:/usr/X11R6/bin:$PATH"
elif [ "$os" = FreeBSD ]; then
  family=freebsd
else
  family=linux
fi

if [ "$os" = OpenBSD ] || [ "$os" = FreeBSD ]; then
  make="gmake"
else
  make="make"
fi

if [ "$os" = OpenBSD ]; then
  jobs=$(sysctl -n hw.ncpuonline 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
else
  jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
fi

prefix=/usr/local
data_home=${XDG_DATA_HOME:-$HOME/.local/share}

# --- uninstall ----------------------------------------------------------------------

if [ "$mode" = uninstall ]; then
  say "Uninstalling Pufferfish from $prefix"
  as_root rm -f "$prefix/bin/pufferfish" \
    "$prefix/share/applications/pufferfish.desktop" \
    "$prefix/share/icons/hicolor/512x512/apps/pufferfish.png"
  as_root rm -rf "$prefix/libexec/pufferfish" "$prefix/share/pufferfish"
  as_root update-desktop-database "$prefix/share/applications" 2>/dev/null || true
  as_root gtk-update-icon-cache -q -t "$prefix/share/icons/hicolor" 2>/dev/null || true
  rm -f "$data_home/applications/pufferfish-links.desktop"
  if [ -d "$data_home/pufferfish" ]; then
    if [ "$yes" = 0 ] && ask "Also delete your Pufferfish data (Roblox APK, sign-in, settings) in $data_home/pufferfish?"; then
      rm -rf "$data_home/pufferfish"
      note "deleted $data_home/pufferfish"
    else
      note "kept your data in $data_home/pufferfish"
    fi
  fi
  say "Done"
  exit 0
fi

# --- OpenBSD version & environment checks -------------------------------------------

if [ "$os" = OpenBSD ]; then
  say "Pufferfish on OpenBSD $obsd_ver ($obsd_flavor) [$arch]"
  major=$(printf '%s' "$obsd_ver" | cut -d. -f1)
  minor=$(printf '%s' "$obsd_ver" | cut -d. -f2)
  if [ "$major" -ge 8 ]; then
    note "release check: OpenBSD $obsd_ver ($obsd_flavor) — full OpenBSD 8.x support (Clang 19+, Xenocara Mesa/Vulkan 1.3, pinsyscalls v2 & native futex)"
  elif [ "$major" -eq 7 ] && [ "$minor" -ge 4 ]; then
    note "release check: OpenBSD $obsd_ver ($obsd_flavor) supports native futex(2), pinsyscalls trap, and Xenocara DRM/Vulkan"
  else
    note "warning: OpenBSD $obsd_ver is older than 7.4; OpenBSD 7.5–8.0+ is recommended for Vulkan & WebKitGTK"
  fi

  usr_local_mnt=$(df -P "$prefix" 2>/dev/null | awk 'NR==2 {print $6}')
  if [ -n "$usr_local_mnt" ]; then
    if ! mount | awk -v m="$usr_local_mnt" '$3 == m {print $0}' | grep -q "wxallowed"; then
      say "OpenBSD W^X check: $usr_local_mnt is not mounted with 'wxallowed'"
      note "Roblox's Android library (libroblox.so) maps executable pages at runtime, requiring wxallowed on $usr_local_mnt."
      if [ "$mode" != "test" ] && ask "Enable wxallowed on $usr_local_mnt now (updates /etc/fstab and remounts)?"; then
        as_root cp /etc/fstab /etc/fstab.pufferfish.bak
        as_root awk -v m="$usr_local_mnt" '$2 == m && $4 !~ /wxallowed/ { $4 = $4 ",wxallowed" } { print }' /etc/fstab.pufferfish.bak | as_root tee /etc/fstab >/dev/null
        as_root mount -u -o wxallowed "$usr_local_mnt" || note "remount after reboot to apply wxallowed"
        note "enabled wxallowed on $usr_local_mnt (backup at /etc/fstab.pufferfish.bak)"
      fi
    else
      note "W^X check: $usr_local_mnt is mounted with wxallowed (ok)"
    fi
  fi
else
  say "Pufferfish $(cat VERSION 2>/dev/null || echo 1.0-openbsd) on $os ($arch)"
fi

# --- dependencies -------------------------------------------------------------------

pcs="sdl2 freetype2 libpng libcrypto zlib x11 webkit2gtk-4.1"
if [ "$os" = OpenBSD ] || [ "$os" = FreeBSD ]; then
  pcs="$pcs epoll-shim"
fi

cmds="pkg-config curl unzip $make"
if [ "$os" = OpenBSD ]; then
  cmds="$cmds update-desktop-database"
fi

have_cxx() {
  command -v c++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1
}

have_vulkan_headers() {
  cxx="c++"
  command -v c++ >/dev/null 2>&1 || cxx=clang++
  command -v "$cxx" >/dev/null 2>&1 || cxx=g++
  echo '#include <vulkan/vulkan_core.h>' | "$cxx" -I/usr/local/include -I/usr/X11R6/include -E -x c++ - >/dev/null 2>&1
}

have_vulkan_loader() {
  if [ "$os" = OpenBSD ]; then
    [ -f /usr/local/lib/libvulkan.so ] || ls /usr/local/lib/libvulkan.so.* >/dev/null 2>&1
  else
    return 0
  fi
}

have_pkg_config_item() {
  p="$1"
  if [ "$os" = OpenBSD ]; then
    case "$p" in
      libcrypto) [ -f /usr/include/openssl/crypto.h ] && return 0 ;;
      zlib)      [ -f /usr/include/zlib.h ] && return 0 ;;
      webkit2gtk-4.1)
        pkg-config --exists webkit2gtk-4.1 2>/dev/null && return 0
        pkg-config --exists webkitgtk-6.0 2>/dev/null && return 0
        pkg-config --exists webkit2gtk-4.0 2>/dev/null && return 0
        return 1
        ;;
    esac
  fi
  command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$p" 2>/dev/null
}

missing() {
  miss=
  have_cxx || miss="$miss c++"
  for c in $cmds; do
    command -v "$c" >/dev/null 2>&1 || miss="$miss $c"
  done
  for p in $pcs; do
    have_pkg_config_item "$p" || miss="$miss $p"
  done
  have_vulkan_headers || miss="$miss vulkan-headers"
  have_vulkan_loader  || miss="$miss vulkan-loader"
  echo "$miss"
}

package_for() {
  case "$family:$1" in
    openbsd:c++|openbsd:libcrypto|openbsd:zlib|openbsd:x11|openbsd:freetype2|openbsd:pkg-config) echo "" ;;
    openbsd:gmake) echo gmake ;;
    openbsd:curl) echo curl ;;
    openbsd:unzip) echo "unzip--" ;;
    openbsd:sdl2) echo sdl2 ;;
    openbsd:libpng) echo png ;;
    openbsd:epoll-shim) echo epoll-shim ;;
    openbsd:vulkan-headers) echo vulkan-headers ;;
    openbsd:vulkan-loader) echo vulkan-loader ;;
    openbsd:webkit2gtk-4.1) echo webkitgtk41 ;;
    openbsd:update-desktop-database) echo desktop-file-utils ;;
    openbsd:*) echo "$1" ;;
    *) echo "$1" ;;
  esac
}

filter_uninstalled_openbsd_pkgs() {
  out=
  for pkg in "$@"; do
    stem=$(printf '%s' "$pkg" | sed 's/--$//')
    if pkg_info -q -e "${stem}-*" 2>/dev/null; then
      continue
    fi
    out="$out $pkg"
  done
  echo "$out"
}

install_packages() {
  if [ "$obsd_flavor" = "-current snapshot" ]; then
    as_root pkg_add -I -D snap "$@"
  else
    as_root pkg_add -I "$@"
  fi
}

need=$(missing)
if [ -n "$need" ]; then
  if [ "$os" = OpenBSD ] && ! have_cxx; then
    die "c++ compiler missing: make sure OpenBSD's comp${major}${minor}.tgz base set is installed"
  fi
  if [ "$os" = OpenBSD ] && [ ! -d /usr/X11R6/include/X11 ]; then
    die "Xenocara X11 headers missing: make sure OpenBSD's xbase${major}${minor}.tgz and xshare${major}${minor}.tgz sets are installed"
  fi

  pkgs=
  for item in $need; do
    pkgs="$pkgs $(package_for "$item")"
  done
  # shellcheck disable=SC2086
  pkgs=$(printf '%s\n' $pkgs | awk 'NF' | sort -u | tr '\n' ' ')
  if [ "$os" = OpenBSD ] && [ -n "$pkgs" ]; then
    # shellcheck disable=SC2086
    pkgs=$(filter_uninstalled_openbsd_pkgs $pkgs)
  fi

  if [ -n "$(printf '%s' "$pkgs" | tr -d ' ')" ]; then
    say "Missing capabilities:$need"
    note "OpenBSD packages to install (not yet on your system):"
    for p in $pkgs; do
      note "  - $p"
    done
    if [ "$mode" = "test" ]; then
      say "Dry-run test complete: installer.sh detected missing packages accurately."
      exit 0
    fi
    ask "Install these packages now with ${sudo:-root} pkg_add?" || die "cannot build Pufferfish without required packages"
    # shellcheck disable=SC2086
    install_packages $pkgs
  fi
else
  say "All required packages are already installed on your $os $obsd_ver system"
  if [ "$mode" = "test" ]; then
    say "Dry-run test complete: all dependencies satisfied."
    exit 0
  fi
fi

# --- build and install --------------------------------------------------------------

say "Building Pufferfish on $os $obsd_ver ($jobs parallel jobs)"
$make -j"$jobs"
$make webview

say "Installing Pufferfish to $prefix (uses ${sudo:-root} for $prefix)"
as_root $make install PREFIX="$prefix" >/dev/null

if [ -f "$data_home/applications/pufferfish.desktop" ] && grep -q "run.sh" "$data_home/applications/pufferfish.desktop"; then
  rm -f "$data_home/applications/pufferfish.desktop"
  note "removed old local folder menu entry so $prefix/share/applications/pufferfish.desktop takes precedence"
fi

if command -v update-desktop-database >/dev/null 2>&1; then
  as_root update-desktop-database "$prefix/share/applications" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  as_root gtk-update-icon-cache -q -t "$prefix/share/icons/hicolor" 2>/dev/null || true
fi

say "Installed Pufferfish successfully to $prefix"
note "Binary:        $prefix/bin/pufferfish (launches $prefix/libexec/pufferfish/pufferfish)"
note "Desktop entry: $prefix/share/applications/pufferfish.desktop (Category: Game)"
note "Icon:          $prefix/share/icons/hicolor/512x512/apps/pufferfish.png"
say "Open Pufferfish from your desktop app menu (under Games), or run: pufferfish"
