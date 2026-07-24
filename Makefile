# rPod top-level build
#
# Targets:
#   make sim          - build and run the desktop LVGL/SDL UI simulator
#   make build         - cross-compile the on-device binary for the Pi (aarch64)
#   make deploy         - rsync the built binary + system files to rpod.local
#   make deploy-run      - deploy, then restart the rpod systemd service
#
# See docs/PLAN.md for the full spec.

PI_HOST     ?= rpod.local
PI_USER     ?= rpod
CROSS       ?= aarch64-linux-gnu-
CC_CROSS    := $(CROSS)gcc

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
                src/app.c \
                src/ui/fonts/lv_font_montserrat_10.c \
                src/ui/fonts/lv_font_montserrat_12.c \
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

# RPOD_BOARD selects the UI form factor + window size (default: the classic
# 320x240 landscape build). `RPOD_BOARD=hat144 make sim` opens a 128x128
# window with the square-panel profile, standing in for the Waveshare 1.44"
# LCD HAT. See src/platform/board.h and tools/sim/sim_main.c.
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

APP_CFLAGS  := -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -I src -I src/ui -I $(LVGL_DIR) \
               $(shell pkg-config --cflags libmpdclient libcurl)
# -lgpiod: the on-device joystick/button backend (src/input/gpio_buttons.c)
# reads /dev/gpiochip0 via libgpiod (package libgpiod-dev). Linked directly
# rather than via pkg-config, same as the wheel daemon's -lpigpio -- it's an
# on-device-only dependency (see docs/PLAN.md's multi-board notes).
APP_LDFLAGS := $(shell pkg-config --libs libmpdclient libcurl) -lgpiod -lm -lpthread -lz

.PHONY: build
build: $(BUILD_DIR)/rpod

$(BUILD_DIR)/rpod: $(APP_OBJS)
	$(CC_CROSS) $(APP_OBJS) -o $@ $(APP_LDFLAGS)

# Header-dependency tracking -- see the note on the sim rule above.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_CROSS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

-include $(APP_OBJS:.o=.d)

# --- Deploy -------------------------------------------------------------

.PHONY: deploy
deploy: build
	rsync -avz --progress \
		$(BUILD_DIR)/rpod \
		system/systemd/ \
		$(PI_USER)@$(PI_HOST):/tmp/rpod-deploy/

.PHONY: deploy-run
deploy-run: deploy
	ssh $(PI_USER)@$(PI_HOST) ' \
		sudo install -m 755 /tmp/rpod-deploy/rpod /usr/local/bin/rpod && \
		sudo install -m 644 /tmp/rpod-deploy/rpod.service /etc/systemd/system/rpod.service && \
		sudo systemctl daemon-reload && \
		sudo systemctl restart rpod'

# --- Hardware tools (cross-compiled, require pigpio on-device) -------------

.PHONY: wheel-sniff
wheel-sniff: $(BUILD_DIR)/wheel-sniff

$(BUILD_DIR)/wheel-sniff: tools/wheel-sniff.c
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g $< -o $@ -lpigpio -lpthread -lrt

.PHONY: fb-test
fb-test: $(BUILD_DIR)/fb-test

$(BUILD_DIR)/fb-test: tools/fb-test.c
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_POSIX_C_SOURCE=200809L $< -o $@

# --- panel-mipi-dbi firmware blob (Waveshare 1.44" HAT, RPOD_BOARD=waveshare-144)
# Compiles the ST7735S init sequence into /lib/firmware/panel.bin's format for
# the panel-mipi-dbi DRM driver. Install with:
#   sudo install -m 644 build/panel.bin /lib/firmware/panel.bin
# See system/panel-mipi-dbi/README.md.
.PHONY: panel-blob
panel-blob: $(BUILD_DIR)/panel.bin

$(BUILD_DIR)/panel.bin: system/panel-mipi-dbi/st7735s-waveshare144.txt system/panel-mipi-dbi/mipi-dbi-cmd
	@mkdir -p $(BUILD_DIR)
	system/panel-mipi-dbi/mipi-dbi-cmd $@ $<

# --- Click wheel daemon (cross-compiled, requires pigpio on-device) --------
#
# Won't build until daemon/wheel_bits.h has real bit positions derived from
# hardware — see docs/PLAN.md §4.3 and docs/clickwheel-protocol.md.

.PHONY: wheel
wheel: $(BUILD_DIR)/rpod-wheel

$(BUILD_DIR)/rpod-wheel: daemon/rpod-wheel.c daemon/wheel_protocol.h daemon/wheel_bits.h
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE daemon/rpod-wheel.c -o $@ -lpigpio -lpthread -lrt

.PHONY: wheel-test-client
wheel-test-client: $(BUILD_DIR)/wheel-test-client

$(BUILD_DIR)/wheel-test-client: tools/wheel-test-client.c daemon/wheel_protocol.h
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE tools/wheel-test-client.c -o $@

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
