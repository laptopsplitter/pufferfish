# Makefile for Pufferfish (OpenBSD, FreeBSD, and Linux)
UNAME   := $(shell uname -s)
VERSION := $(shell cat VERSION 2>/dev/null || echo 1.0-openbsd)

ifeq ($(UNAME),OpenBSD)
# OpenBSD: clang++ in base (/usr/bin/c++); Xenocara X11/Mesa in /usr/X11R6;
# packages in /usr/local; epoll/timerfd/eventfd from epoll-shim.
# CRITICAL: -Wl,-z,wxneeded marks the ELF PT_OPENBSD_WXNEEDED phdr so OpenBSD's
# kernel allows mmap(PROT_WRITE|PROT_EXEC) needed by libroblox.so and Luau.
ifeq ($(origin CXX),default)
CXX := c++
endif
ifeq ($(origin CC),default)
CC  := cc
endif
export PKG_CONFIG_PATH := /usr/local/lib/pkgconfig:/usr/X11R6/lib/pkgconfig:$(PKG_CONFIG_PATH)
OS_CFLAGS := -I/usr/local/include -I/usr/X11R6/include $(shell pkg-config --cflags epoll-shim 2>/dev/null) -D__OpenBSD_Pufferfish__=1
OS_LIBS   := -L/usr/local/lib -L/usr/X11R6/lib $(shell pkg-config --libs epoll-shim 2>/dev/null) -pthread -Wl,-z,wxneeded
DL_LIBS   :=
OS_SRC    := src/openbsd_abi.cpp src/openbsd_sys.cpp
OS_HDR    := src/bsdabi.hpp src/bsdsys.hpp
WIN_CFLAGS := $(shell pkg-config --cflags x11)
WIN_LIBS   := $(shell pkg-config --libs x11)
WEBKIT_PC  := $(shell pkg-config --exists webkit2gtk-4.1 && echo webkit2gtk-4.1 || echo webkit2gtk-4.0)
else ifeq ($(UNAME),FreeBSD)
ifeq ($(origin CXX),default)
CXX := c++
endif
OS_CFLAGS := -I/usr/local/include $(shell pkg-config --cflags epoll-shim 2>/dev/null)
OS_LIBS   := -L/usr/local/lib $(shell pkg-config --libs epoll-shim 2>/dev/null) -pthread
DL_LIBS   :=
OS_SRC    := src/bsdabi.cpp src/bsdsys.cpp
OS_HDR    := src/bsdabi.hpp src/bsdsys.hpp
WIN_CFLAGS := $(shell pkg-config --cflags wayland-client x11)
WIN_LIBS   := $(shell pkg-config --libs wayland-client x11)
WEBKIT_PC  := webkit2gtk-4.1
else
CXX       ?= g++
OS_CFLAGS :=
OS_LIBS   :=
DL_LIBS   := -ldl
OS_SRC    :=
OS_HDR    :=
WIN_CFLAGS := $(shell pkg-config --cflags wayland-client x11)
WIN_LIBS   := $(shell pkg-config --libs wayland-client x11)
WEBKIT_PC  := webkit2gtk-4.1
endif

CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2
ifeq ($(UNAME),OpenBSD)
LDFLAGS  ?= -Wl,-z,relro,-z,now -Wl,-z,wxneeded
else
LDFLAGS  ?= -Wl,-z,relro,-z,now -Wl,-z,noexecstack
endif

SRC := src/main.cpp src/elfimg.cpp src/shims.cpp src/fakejni.cpp src/script.cpp \
       src/window.cpp src/assets.cpp src/input.cpp src/webview.cpp src/mainloop.cpp \
       src/game.cpp src/audio.cpp src/cookies.cpp src/textoverlay.cpp \
       src/usersettings.cpp src/looper.cpp src/config.cpp src/recent.cpp \
       src/menu.cpp src/apk.cpp src/apksig.cpp src/apkfiles.cpp src/panel.cpp \
       src/mods.cpp src/profile.cpp src/vkshim.cpp src/gamemode.cpp \
       src/closescreen.cpp src/overlaywin.cpp src/weblogin.cpp src/spawn.cpp \
       src/links.cpp src/device.cpp src/haptics.cpp src/account.cpp \
       src/upscale.cpp src/framegen.cpp src/privacy.cpp src/netping.cpp \
       src/clientsettings.cpp src/platform.cpp src/appicon.cpp $(OS_SRC)

SDL_CFLAGS    := $(shell pkg-config --cflags sdl2)
SDL_LIBS      := $(shell pkg-config --libs sdl2)
FT_CFLAGS     := $(shell pkg-config --cflags freetype2)
FT_LIBS       := $(shell pkg-config --libs freetype2)
PNG_CFLAGS    := $(shell pkg-config --cflags libpng)
PNG_LIBS      := $(shell pkg-config --libs libpng)
CRYPTO_CFLAGS := $(shell pkg-config --cflags libcrypto 2>/dev/null)
CRYPTO_LIBS   := $(or $(shell pkg-config --libs libcrypto 2>/dev/null),-lcrypto)
ZLIB_CFLAGS   := $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS     := $(or $(shell pkg-config --libs zlib 2>/dev/null),-lz)

.DEFAULT_GOAL := pufferfish

OBJ := $(SRC:src/%.cpp=build/%.o)
ALL_CFLAGS = $(CXXFLAGS) -DPUFFERFISH_VERSION='"$(VERSION)"' $(OS_CFLAGS) $(SDL_CFLAGS) $(FT_CFLAGS) $(PNG_CFLAGS) $(WIN_CFLAGS) $(CRYPTO_CFLAGS) $(ZLIB_CFLAGS)

build/.vulkan-ok:
	@mkdir -p build
	@echo '#include <vulkan/vulkan_core.h>' | $(CXX) $(OS_CFLAGS) -E -x c++ - >/dev/null 2>&1 || { echo "Vulkan headers missing: run ./installer.sh (OpenBSD: doas pkg_add vulkan-headers)"; exit 1; }
	@touch $@

build/%.o: src/%.cpp build/.vulkan-ok
	@echo "  CXX $<"
	@$(CXX) $(ALL_CFLAGS) -c -o $@ $<

pufferfish: $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(DL_LIBS) $(SDL_LIBS) $(FT_LIBS) $(PNG_LIBS) $(WIN_LIBS) $(CRYPTO_LIBS) $(ZLIB_LIBS) $(OS_LIBS)

webview: pufferfish-webview
pufferfish-webview: src/webview_helper.c
	$(CC) -O2 -g -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 $(LDFLAGS) -o $@ $< $$(pkg-config --cflags --libs $(WEBKIT_PC))

PREFIX     ?= /usr/local
DESTDIR    ?=
BINDIR     := $(PREFIX)/bin
LIBEXECDIR := $(PREFIX)/libexec/pufferfish
SHAREDIR   := $(PREFIX)/share/pufferfish
APPSDIR    := $(PREFIX)/share/applications
ICONDIR    := $(PREFIX)/share/icons/hicolor/512x512/apps

SUBST = sed -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' -e 's|@SHAREDIR@|$(SHAREDIR)|g'

install: pufferfish pufferfish-webview
	mkdir -p "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBEXECDIR)" "$(DESTDIR)$(SHAREDIR)/assets" \
	  "$(DESTDIR)$(SHAREDIR)/scripts" "$(DESTDIR)$(APPSDIR)" "$(DESTDIR)$(ICONDIR)"
	cp pufferfish pufferfish-webview "$(DESTDIR)$(LIBEXECDIR)/"
	chmod 755 "$(DESTDIR)$(LIBEXECDIR)/pufferfish" "$(DESTDIR)$(LIBEXECDIR)/pufferfish-webview"
	cp assets/icon.png assets/logo.png "$(DESTDIR)$(SHAREDIR)/assets/"
	cp scripts/startup.script scripts/roblox.values scripts/settings-overrides.json "$(DESTDIR)$(SHAREDIR)/scripts/"
	$(SUBST) packaging/pufferfish.sh.in > "$(DESTDIR)$(BINDIR)/pufferfish"
	chmod 755 "$(DESTDIR)$(BINDIR)/pufferfish"
	$(SUBST) packaging/pufferfish.desktop.in > "$(DESTDIR)$(APPSDIR)/pufferfish.desktop"
	chmod 644 "$(DESTDIR)$(APPSDIR)/pufferfish.desktop"
	cp assets/icon.png "$(DESTDIR)$(ICONDIR)/pufferfish.png"
	-if [ -z "$(DESTDIR)" ]; then \
	  update-desktop-database "$(APPSDIR)" 2>/dev/null; \
	  gtk-update-icon-cache -q -t "$(PREFIX)/share/icons/hicolor" 2>/dev/null; \
	fi; true

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/pufferfish" "$(DESTDIR)$(APPSDIR)/pufferfish.desktop" "$(DESTDIR)$(ICONDIR)/pufferfish.png"
	rm -rf "$(DESTDIR)$(LIBEXECDIR)" "$(DESTDIR)$(SHAREDIR)"
	-if [ -z "$(DESTDIR)" ]; then update-desktop-database "$(APPSDIR)" 2>/dev/null; fi; true

clean:
	rm -rf build pufferfish pufferfish-webview

.PHONY: install uninstall webview clean
