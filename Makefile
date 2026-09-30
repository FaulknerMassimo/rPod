# rPod top-level build
#
# Targets:
#   make sim          - build and run the desktop LVGL/SDL UI simulator
#   make sysroot       - copy the Pi's headers + libraries into ./sysroot (once,
#                        and again after installing new -dev packages on the Pi)
#   make build         - cross-compile the on-device binary for the Pi (aarch64)
#   make deploy         - rsync the built binary + system files to rpod.local
#   make deploy-run      - deploy, then restart the rpod systemd service
#   make deploy-wheel    - build + install the click wheel daemon (rpod-wheel)
#                          and its test tools on rpod.local; needs pigpio there
#   make bluetooth-setup - install BlueZ + system-wide PipeWire on rpod.local
#
# See docs/PLAN.md for the full spec.

PI_HOST     ?= rpod.local
PI_USER     ?= rpod
# Override when mDNS is flaky, keeping the known host key, e.g.
#   make deploy-run PI_HOST=192.168.1.113 SSH="ssh -o HostKeyAlias=rpod.local"
SSH         ?= ssh

# Cross toolchain: clang + lld targeting aarch64 (host packages: clang, lld),
# compiling and linking against a sysroot copied off the Pi itself (`make
# sysroot`) -- so the binary links against the Pi's own glibc, libmpdclient
# and libcurl, whatever Debian release it runs. To build natively on the Pi
# instead: make build CC_CROSS=gcc PKG_CONFIG_CROSS=pkg-config
SYSROOT     ?= $(CURDIR)/sysroot
CC_CROSS    ?= clang --target=aarch64-linux-gnu --sysroot=$(SYSROOT) -fuse-ld=lld -Qunused-arguments
PKG_CONFIG_CROSS ?= PKG_CONFIG_SYSROOT_DIR=$(SYSROOT) \
                    PKG_CONFIG_LIBDIR=$(SYSROOT)/usr/lib/aarch64-linux-gnu/pkgconfig:$(SYSROOT)/usr/share/pkgconfig \
                    pkg-config

LVGL_DIR    := third_party/lvgl
BUILD_DIR   := build
SIM_BUILD_DIR := build-sim

# --- Desktop simulator ------------------------------------------------------

LVGL_SRCS   := $(shell find $(LVGL_DIR)/src -name '*.c')
RPOD_UI_SRCS := src/ui/theme.c \
                src/ui/metrics.c \
                src/ui/status_bar.c \
                src/ui/cover_art.c \
                src/ui/heart_icon.c \
                src/ui/playlist_membership.c \
                src/input/encoder.c \
                src/input/wheel_input.c \
                src/app.c \
                src/ui/fonts/lv_font_montserrat_14.c \
                src/ui/fonts/lv_font_montserrat_16.c \
                src/ui/fonts/lv_font_montserrat_20.c \
                src/ui/fonts/lv_font_montserrat_24.c \
                src/ui/screens/screen_stack.c \
                src/ui/screens/list_screen.c \
                src/ui/screens/music_screens.c \
                src/ui/screens/search_screen.c \
                src/ui/screens/playlist_edit_screens.c \
                src/ui/screens/playlist_picker.c \
                src/ui/screens/now_playing.c \
                src/ui/screens/settings_screens.c \
                src/ui/screens/main_menu.c \
                src/audio/mpd_client.c \
                src/audio/visualizer.c \
                src/audio/listenbrainz.c \
                src/audio/scrobbler.c
SIM_SRCS    := tools/sim/sim_main.c tools/sim/sim_input.c $(RPOD_UI_SRCS) $(LVGL_SRCS)
SIM_OBJS    := $(patsubst %.c,$(SIM_BUILD_DIR)/%.o,$(SIM_SRCS))

SIM_CFLAGS  := -std=c17 -Wall -Wextra -O0 -g -D_DEFAULT_SOURCE \
               -I tools/sim -I src -I $(LVGL_DIR) \
               $(shell pkg-config --cflags sdl2 libmpdclient libcurl)
SIM_LDFLAGS := $(shell pkg-config --libs sdl2 libmpdclient libcurl) -lm -lpthread -lz

.PHONY: sim
sim: $(SIM_BUILD_DIR)/rpod-sim
	$(SIM_BUILD_DIR)/rpod-sim

$(SIM_BUILD_DIR)/rpod-sim: $(SIM_OBJS)
	$(CC) $(SIM_OBJS) -o $@ $(SIM_LDFLAGS)

# -MMD -MP emits a .d beside each .o listing the headers it included; the
# -include below then makes every object depend on those headers, so editing
# a header (e.g. adding a field to a struct in list_screen.h) recompiles all
# its users. Without this, an incremental build silently links objects built
# against a stale struct layout -- an ABI mismatch that crashes at runtime.
$(SIM_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(SIM_CFLAGS) -MMD -MP -c $< -o $@

-include $(SIM_OBJS:.o=.d)

# --- On-device (cross) build -------------------------------------------------

APP_SRCS    := $(shell find src -name '*.c') $(LVGL_SRCS)
APP_OBJS    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SRCS))

APP_CFLAGS  = -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -I src -I src/ui -I $(LVGL_DIR) \
               $(shell $(PKG_CONFIG_CROSS) --cflags libmpdclient libcurl)
APP_LDFLAGS = $(shell $(PKG_CONFIG_CROSS) --libs libmpdclient libcurl) -lm -lpthread -lz

.PHONY: build
build: $(BUILD_DIR)/rpod

$(BUILD_DIR)/rpod: $(APP_OBJS)
	$(CC_CROSS) $(APP_OBJS) -o $@ $(APP_LDFLAGS)

# Header-dependency tracking -- see the note on the sim rule above.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_CROSS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

-include $(APP_OBJS:.o=.d)

# --- Sysroot (the Pi's headers + libraries, for cross builds) --------------
#
# Only what compiling/linking needs: headers, GCC's crt/libgcc bits, and the
# .so/.a/.o/.pc files from the multiarch lib dir (big runtime-only blobs like
# Mesa/LLVM are skipped by size). Symlinks leaving a copied tree are
# dereferenced (e.g. the kernel headers' asm/*.h point into /usr/lib/linux/),
# so nothing dangles or points into the host's own /usr; lib -> usr/lib
# mirrors the Pi's merged-/usr layout, which glibc's libc.so linker script
# relies on.
PI_SSH := $(PI_USER)@$(PI_HOST)

.PHONY: sysroot
sysroot:
	@mkdir -p $(SYSROOT)/usr/lib/gcc $(SYSROOT)/usr/share $(SYSROOT)/usr/local
	rsync -e "$(SSH)" -a --delete --copy-unsafe-links $(PI_SSH):/usr/include/ $(SYSROOT)/usr/include/
	rsync -e "$(SSH)" -a --delete $(PI_SSH):/usr/lib/gcc/aarch64-linux-gnu/ $(SYSROOT)/usr/lib/gcc/aarch64-linux-gnu/
	rsync -e "$(SSH)" -a --delete --copy-unsafe-links --prune-empty-dirs --max-size=8M \
		--include='*/' --include='*.so' --include='*.so.*' --include='*.a' --include='*.o' \
		--include='pkgconfig/*' --exclude='*' \
		$(PI_SSH):/usr/lib/aarch64-linux-gnu/ $(SYSROOT)/usr/lib/aarch64-linux-gnu/
	rsync -e "$(SSH)" -a --copy-unsafe-links $(PI_SSH):/usr/lib/ld-linux-aarch64.so.1 $(SYSROOT)/usr/lib/
	rsync -e "$(SSH)" -a --delete $(PI_SSH):/usr/share/pkgconfig/ $(SYSROOT)/usr/share/pkgconfig/
	@# /usr/local: pigpio, built from source on the Pi (docs/PLAN.md §4.5).
	rsync -e "$(SSH)" -a --delete --copy-unsafe-links $(PI_SSH):/usr/local/include/ $(SYSROOT)/usr/local/include/
	rsync -e "$(SSH)" -a --delete --copy-unsafe-links --prune-empty-dirs \
		--include='*/' --include='*.so' --include='*.so.*' --include='*.a' \
		--include='pkgconfig/*' --exclude='*' \
		$(PI_SSH):/usr/local/lib/ $(SYSROOT)/usr/local/lib/
	ln -sfn usr/lib $(SYSROOT)/lib

# --- Deploy -------------------------------------------------------------

.PHONY: deploy
deploy: build
	rsync -avz --progress -e "$(SSH)" \
		$(BUILD_DIR)/rpod \
		system/systemd/ \
		$(PI_USER)@$(PI_HOST):/tmp/rpod-deploy/

.PHONY: deploy-run
deploy-run: deploy
	$(SSH) $(PI_USER)@$(PI_HOST) ' \
		sudo install -m 755 /tmp/rpod-deploy/rpod /usr/local/bin/rpod && \
		sudo install -m 644 /tmp/rpod-deploy/rpod.service /etc/systemd/system/rpod.service && \
		sudo systemctl daemon-reload && \
		sudo systemctl enable rpod && \
		sudo systemctl restart rpod'

# --- Bluetooth audio (docs/PLAN.md §6.3) -------------------------------------
#
# One-time (re-runnable) Pi setup: BlueZ + a system-wide PipeWire/WirePlumber
# for MPD's "Bluetooth" output. Also installs system/mpd/mpd.conf.

.PHONY: bluetooth-setup
bluetooth-setup:
	rsync -az --delete -e "$(SSH)" system/ $(PI_SSH):/tmp/rpod-system/
	$(SSH) $(PI_SSH) 'sudo sh /tmp/rpod-system/bluetooth/setup.sh'

# --- Hardware tools (cross-compiled, require pigpio on-device) -------------
#
# pigpio lives in the Pi's /usr/local (built from source there, docs/PLAN.md
# §4.5), which `make sysroot` copies too -- re-run it after installing pigpio.
# clang already searches <sysroot>/usr/local/include; the lib dir needs -L.

PIGPIO_LIBS = -L$(SYSROOT)/usr/local/lib -lpigpio -lpthread -lrt

.PHONY: wheel-sniff
wheel-sniff: $(BUILD_DIR)/wheel-sniff

$(BUILD_DIR)/wheel-sniff: tools/wheel-sniff.c
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE $< -o $@ $(PIGPIO_LIBS)

.PHONY: fb-test
fb-test: $(BUILD_DIR)/fb-test

$(BUILD_DIR)/fb-test: tools/fb-test.c
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L $< -o $@

# --- Click wheel daemon (cross-compiled, requires pigpio on-device) --------
#
# Won't build until daemon/wheel_bits.h has real bit positions derived from
# hardware — see docs/PLAN.md §4.3 and docs/clickwheel-protocol.md.

.PHONY: wheel
wheel: $(BUILD_DIR)/rpod-wheel

$(BUILD_DIR)/rpod-wheel: daemon/rpod-wheel.c daemon/wheel_protocol.h daemon/wheel_bits.h
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE daemon/rpod-wheel.c -o $@ $(PIGPIO_LIBS)

.PHONY: wheel-test-client
wheel-test-client: $(BUILD_DIR)/wheel-test-client

$(BUILD_DIR)/wheel-test-client: tools/wheel-test-client.c daemon/wheel_protocol.h
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE tools/wheel-test-client.c -o $@

# The daemon runs as its own unit (rpod.service Wants= it), so it deploys
# separately from the UI -- `make deploy-run` keeps working without pigpio.
# The sniffer and test client land in /usr/local/bin alongside it; stop
# rpod-wheel before running wheel-sniff (one pigpio process at a time).
.PHONY: deploy-wheel
deploy-wheel: $(BUILD_DIR)/rpod-wheel $(BUILD_DIR)/wheel-sniff $(BUILD_DIR)/wheel-test-client
	rsync -avz -e "$(SSH)" $^ system/systemd/rpod-wheel.service $(PI_SSH):/tmp/rpod-deploy/
	$(SSH) $(PI_SSH) ' \
		cd /tmp/rpod-deploy && \
		sudo install -m 755 rpod-wheel wheel-sniff wheel-test-client /usr/local/bin/ && \
		sudo install -m 644 rpod-wheel.service /etc/systemd/system/rpod-wheel.service && \
		sudo systemctl daemon-reload && \
		sudo systemctl enable rpod-wheel && \
		sudo systemctl restart rpod-wheel'

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(SIM_BUILD_DIR)

# --- Dev MPD instance for `make sim` (docs/PLAN.md §5.4) -------------------
#
# Separate from the on-device mpd.conf in docs/PLAN.md §6.2 — this one plays
# through the dev machine's normal audio so the sim's Music screens have a
# real library and real playback to exercise.

SIM_MUSIC_DIR      ?= $(HOME)/Music
RPOD_MPD_STATE_DIR ?= $(HOME)/.local/state/rpod-sim/mpd
SIM_MPD_CONF       := tools/sim/.mpd-dev.conf

.PHONY: mpd-dev-conf
mpd-dev-conf:
	@mkdir -p $(RPOD_MPD_STATE_DIR)/playlists
	sed -e 's|@SIM_MUSIC_DIR@|$(SIM_MUSIC_DIR)|g' \
	    -e 's|@RPOD_MPD_STATE_DIR@|$(RPOD_MPD_STATE_DIR)|g' \
	    tools/sim/mpd-dev.conf.in > $(SIM_MPD_CONF)

.PHONY: mpd-dev
mpd-dev: mpd-dev-conf
	mpd --no-daemon $(SIM_MPD_CONF)
