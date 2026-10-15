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
#   make deploy-overlay  - compile + install the panel's device-tree overlay
#                          on rpod.local (takes effect next boot)
#   make bluetooth-setup - install BlueZ + system-wide PipeWire on rpod.local
#   make web             - cross-compile the web UI daemon (rpod-web)
#   make deploy-web      - install rpod-web and its page on rpod.local
#   make web-sim         - run the web UI on the dev machine against the sim
#   make test            - build and run the host-side unit tests
#   make ipod-assets     - fetch the iPod's firmware from Apple and extract
#                          its fonts and bitmaps for the Classic themes (once)
#
# See docs/PLAN.md for the full spec.

PI_HOST     ?= rpod.local
PI_USER     ?= rpod
# Override when mDNS is flaky, keeping the known host key, e.g.
#   make deploy-run PI_HOST=192.168.1.113 SSH="ssh -o HostKeyAlias=rpod.local"
SSH         ?= ssh

# Cross toolchain: clang + lld targeting aarch64 (host packages: clang, lld),
# compiling and linking against a sysroot copied off the Pi itself (`make
# sysroot`) -- so the binary links against the Pi's own glibc, libmpdclient,
# libcurl, libsystemd (sd-bus, for BlueZ) and libjpeg (cover art), whatever
# Debian release it runs; each needs its -dev package on the Pi
# (docs/PLAN.md §3). To build natively on the Pi instead:
#   make build CC_CROSS=gcc PKG_CONFIG_CROSS=pkg-config
SYSROOT     ?= $(CURDIR)/sysroot
CC_CROSS    ?= clang --target=aarch64-linux-gnu --sysroot=$(SYSROOT) -fuse-ld=lld -Qunused-arguments
PKG_CONFIG_CROSS ?= PKG_CONFIG_SYSROOT_DIR=$(SYSROOT) \
                    PKG_CONFIG_LIBDIR=$(SYSROOT)/usr/lib/aarch64-linux-gnu/pkgconfig:$(SYSROOT)/usr/share/pkgconfig \
                    pkg-config

LVGL_DIR    := third_party/lvgl
BUILD_DIR   := build
SIM_BUILD_DIR := build-sim

# --- The iPod's own fonts and bitmaps (optional) -----------------------------
#
# The Classic themes (src/ui/classic.h) draw with the 4th-gen iPod's Chicago
# and Pixo fonts and its bitmaps, extracted from its firmware by
# re/ipod4/tools/extract.py. They're Apple's, so they're never committed:
# `make ipod-assets` (below) downloads the firmware from Apple and generates
# them into re/ipod4/out/, once per checkout. When IPOD_ASSETS holds the
# extractor's out/lvgl both builds compile them in; otherwise the Classic
# themes keep their layout but draw with Montserrat and LVGL's symbols.
IPOD_ASSETS ?= re/ipod4/out/lvgl
ifneq ($(wildcard $(IPOD_ASSETS)/ipod_images.h),)
# Relative to IPOD_ASSETS, which can sit outside the tree: their objects go
# under $(BUILD_DIR)/ipod/ (see the rules below), not wherever patsubst lands.
IPOD_SRCS   := ipod_images_x2.c \
               fonts/ipod_font_chicago_12_x2.c \
               fonts/ipod_font_pixo_7_bold_x2.c
IPOD_CFLAGS := -DRPOD_IPOD_ASSETS=1 -I $(IPOD_ASSETS) -I $(IPOD_ASSETS)/fonts
else ifeq ($(filter ipod-assets,$(MAKECMDGOALS)),)
$(info Note: no iPod fonts/bitmaps in $(IPOD_ASSETS), so Classic draws with Montserrat -- run `make ipod-assets`.)
endif

# Whether the assets were found is baked into every object that includes
# classic.h, and make can't see a -D change. So $(call ipod_stamp,dir) keeps
# IPOD_CFLAGS in dir/ipod-assets, rewritten only when they change, and every
# object but LVGL's depends on it: adding, moving or losing the assets
# rebuilds them instead of linking objects that still have the old answer.
ipod_stamp = $(shell mkdir -p $(1) && { echo '$(IPOD_CFLAGS)' | cmp -s - $(1)/ipod-assets || \
                     echo '$(IPOD_CFLAGS)' > $(1)/ipod-assets; } && echo $(1)/ipod-assets)

# --- Desktop simulator ------------------------------------------------------

LVGL_SRCS   := $(shell find $(LVGL_DIR)/src -name '*.c')
RPOD_UI_SRCS := src/ui/theme.c \
                src/ui/metrics.c \
                src/ui/classic.c \
                src/ui/status_bar.c \
                src/ui/cover_art.c \
                src/ui/cover_cache.c \
                src/ui/heart_icon.c \
                src/ui/hud.c \
                src/ui/volume_control.c \
                src/ui/seek_control.c \
                src/ui/backlight.c \
                src/ui/sleep_timer.c \
                src/ui/alarm.c \
                src/ui/big_digits.c \
                src/ui/spinner.c \
                src/ui/airpods_art.c \
                src/ui/airpods_art_data.c \
                src/ui/airpods_notify.c \
                src/ui/playlist_membership.c \
                src/ui/alpha_sort.c \
                src/ui/scrub.c \
                src/input/encoder.c \
                src/input/gestures.c \
                src/input/input.c \
                src/input/wheel_input.c \
                src/input/wheel_tuning.c \
                src/ui/control.c \
                src/app.c \
                src/ui/fonts/lv_font_montserrat_14.c \
                src/ui/fonts/lv_font_montserrat_16.c \
                src/ui/fonts/lv_font_montserrat_20.c \
                src/ui/fonts/lv_font_montserrat_24.c \
                src/ui/fonts/rpod_font_letters_64.c \
                src/ui/fonts/rpod_font_digits_64.c \
                src/ui/screens/screen_stack.c \
                src/ui/screens/list_screen.c \
                src/ui/screens/music_screens.c \
                src/ui/screens/search_screen.c \
                src/ui/screens/playlist_edit_screens.c \
                src/ui/screens/playlist_picker.c \
                src/ui/screens/now_playing.c \
                src/ui/screens/settings_screens.c \
                src/ui/screens/live_list.c \
                src/ui/screens/bluetooth_screens.c \
                src/ui/screens/airpods_screens.c \
                src/ui/screens/main_menu.c \
                src/ui/screens/extras_screens.c \
                src/ui/screens/clock_screens.c \
                src/ui/screens/brick_screen.c \
                src/ui/screens/music_quiz.c \
                src/ui/screens/screen_lock.c \
                src/games/brick.c \
                src/games/quiz.c \
                src/games/scores.c \
                src/audio/mpd_client.c \
                src/audio/embedded_art.c \
                src/audio/bluetooth.c \
                src/audio/aap.c \
                src/audio/avrcp_volume.c \
                src/audio/airpods.c \
                src/audio/visualizer.c \
                src/audio/listenbrainz.c \
                src/audio/scrobbler.c \
                src/audio/volume_memory.c
SIM_SRCS    := tools/sim/sim_main.c tools/sim/sim_input.c $(RPOD_UI_SRCS) $(LVGL_SRCS)
SIM_OBJS    := $(patsubst %.c,$(SIM_BUILD_DIR)/%.o,$(SIM_SRCS)) \
               $(patsubst %.c,$(SIM_BUILD_DIR)/ipod/%.o,$(IPOD_SRCS))

SIM_CFLAGS  := -std=c17 -Wall -Wextra -O0 -g -D_DEFAULT_SOURCE \
               -I tools/sim -I src -I $(LVGL_DIR) $(IPOD_CFLAGS) \
               $(shell pkg-config --cflags sdl2 libmpdclient libcurl libsystemd)
SIM_LDFLAGS := $(shell pkg-config --libs sdl2 libmpdclient libcurl libsystemd) -lm -lpthread -lz -ljpeg

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

$(SIM_BUILD_DIR)/ipod/%.o: $(IPOD_ASSETS)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SIM_CFLAGS) -MMD -MP -c $< -o $@

$(filter-out $(SIM_BUILD_DIR)/$(LVGL_DIR)/%,$(SIM_OBJS)): $(call ipod_stamp,$(SIM_BUILD_DIR))

-include $(SIM_OBJS:.o=.d)

# --- On-device (cross) build -------------------------------------------------

APP_SRCS    := $(shell find src -name '*.c') $(LVGL_SRCS)
APP_OBJS    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SRCS)) \
               $(patsubst %.c,$(BUILD_DIR)/ipod/%.o,$(IPOD_SRCS))

APP_CFLAGS  = -std=c17 -Wall -Wextra -O2 -g -D_DEFAULT_SOURCE -I src -I src/ui -I $(LVGL_DIR) $(IPOD_CFLAGS) \
               $(shell $(PKG_CONFIG_CROSS) --cflags libmpdclient libcurl libsystemd)
APP_LDFLAGS = $(shell $(PKG_CONFIG_CROSS) --libs libmpdclient libcurl libsystemd) -lm -lpthread -lz -ljpeg

.PHONY: build
build: $(BUILD_DIR)/rpod

$(BUILD_DIR)/rpod: $(APP_OBJS)
	$(CC_CROSS) $(APP_OBJS) -o $@ $(APP_LDFLAGS)

# Header-dependency tracking -- see the note on the sim rule above.
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_CROSS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/ipod/%.o: $(IPOD_ASSETS)/%.c
	@mkdir -p $(dir $@)
	$(CC_CROSS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

$(filter-out $(BUILD_DIR)/$(LVGL_DIR)/%,$(APP_OBJS)): $(call ipod_stamp,$(BUILD_DIR))

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
		system/udev/ \
		$(PI_USER)@$(PI_HOST):/tmp/rpod-deploy/

.PHONY: deploy-run
deploy-run: deploy
	$(SSH) $(PI_USER)@$(PI_HOST) ' \
		sudo install -m 755 /tmp/rpod-deploy/rpod /usr/local/bin/rpod && \
		sudo install -m 644 /tmp/rpod-deploy/rpod.service /etc/systemd/system/rpod.service && \
		sudo install -m 644 /tmp/rpod-deploy/99-rpod-panel.rules /etc/udev/rules.d/ && \
		sudo udevadm control --reload && \
		sudo udevadm trigger --subsystem-match=backlight --subsystem-match=graphics && \
		sudo systemctl daemon-reload && \
		sudo systemctl enable rpod && \
		sudo systemctl restart rpod'

# --- Panel overlay (docs/PLAN.md §5.3) ---------------------------------------
#
# system/overlays/rpod-panel.dts, compiled with the Pi's own dtc into the
# firmware's overlay directory. config.txt loads it (dtoverlay=rpod-panel,
# system/config.txt.d/rpod.txt); like any boot config, it needs a reboot.

.PHONY: deploy-overlay
deploy-overlay:
	rsync -az -e "$(SSH)" system/overlays/rpod-panel.dts $(PI_SSH):/tmp/rpod-deploy/
	$(SSH) $(PI_SSH) ' \
		dtc -@ -q -I dts -O dtb -o /tmp/rpod-deploy/rpod-panel.dtbo /tmp/rpod-deploy/rpod-panel.dts && \
		sudo install -m 644 /tmp/rpod-deploy/rpod-panel.dtbo /boot/firmware/overlays/'

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

# --- Web UI (docs/PLAN.md §8.6) --------------------------------------------
#
# rpod-web is its own process, like the click wheel daemon: music uploads,
# the library and the settings the wheel can't reach, served from the device
# on port 80. It shares the MPD client and the FLAC cover reader with the UI,
# and talks to the UI itself over its settings socket (src/ui/control.h).
# The page is web/static/, served as plain files -- no build step. The
# iPod's font and icons for the Classic themes come from `make ipod-assets`
# (re/ipod4/out/web/), deployed beside it when they're there.
WEB_SRCS    := web/rpod_web.c web/http.c web/json.c web/api.c web/library.c web/control_client.c \
               src/audio/mpd_client.c src/audio/embedded_art.c
WEB_HDRS    := $(wildcard web/*.h) src/audio/mpd_client.h src/audio/embedded_art.h
WEB_VERSION := $(shell git describe --always --dirty 2>/dev/null || echo dev)
WEB_CFLAGS  := -std=c17 -Wall -Wextra -g -D_GNU_SOURCE -I src -DRPOD_VERSION='"$(WEB_VERSION)"'
WEB_IPOD    := $(IPOD_ASSETS)/../web

.PHONY: web
web: $(BUILD_DIR)/rpod-web

$(BUILD_DIR)/rpod-web: $(WEB_SRCS) $(WEB_HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CC_CROSS) $(WEB_CFLAGS) -O2 $(shell $(PKG_CONFIG_CROSS) --cflags libmpdclient libcurl) $(WEB_SRCS) -o $@ \
		$(shell $(PKG_CONFIG_CROSS) --libs libmpdclient libcurl) -lpthread

$(SIM_BUILD_DIR)/rpod-web: $(WEB_SRCS) $(WEB_HDRS)
	@mkdir -p $(SIM_BUILD_DIR)
	$(CC) $(WEB_CFLAGS) -O0 $(shell pkg-config --cflags libmpdclient libcurl) $(WEB_SRCS) -o $@ \
		$(shell pkg-config --libs libmpdclient libcurl) -lpthread

# The web UI on the dev machine, at http://localhost:8080/, against the
# sim's MPD (`make mpd-dev`) and settings (`make sim`, for those rPod owns).
SIM_STATE    ?= $(HOME)/.local/state/rpod-sim
WEB_SIM_PORT ?= 8080

.PHONY: web-sim
web-sim: $(SIM_BUILD_DIR)/rpod-web
	RPOD_WEB_PORT=$(WEB_SIM_PORT) RPOD_WEB_ROOT=web/static RPOD_WEB_IPOD=$(WEB_IPOD) \
	RPOD_MPD_SOCKET=$(RPOD_MPD_STATE_DIR)/socket RPOD_CONTROL_SOCK=$(SIM_STATE)/control.sock \
	RPOD_THEME_STATE=$(SIM_STATE)/theme RPOD_LISTENBRAINZ_QUEUE=$(SIM_STATE)/listenbrainz_queue.jsonl \
	$(SIM_BUILD_DIR)/rpod-web

# The daemon, its unit and the page (with the iPod's assets when they've
# been generated). Uploads need the music directory writable by the rpod
# user; this says so if it isn't.
.PHONY: deploy-web
deploy-web: $(BUILD_DIR)/rpod-web
	rsync -az --delete -e "$(SSH)" web/static/ $(PI_SSH):/tmp/rpod-deploy/web/
	$(if $(wildcard $(WEB_IPOD)/manifest.json),rsync -az --delete -e "$(SSH)" $(WEB_IPOD)/ $(PI_SSH):/tmp/rpod-deploy/web/ipod/)
	rsync -az -e "$(SSH)" $(BUILD_DIR)/rpod-web system/systemd/rpod-web.service $(PI_SSH):/tmp/rpod-deploy/
	$(SSH) $(PI_SSH) ' \
		sudo install -m 755 /tmp/rpod-deploy/rpod-web /usr/local/bin/rpod-web && \
		sudo mkdir -p /usr/local/share/rpod/web && \
		sudo rsync -a --delete /tmp/rpod-deploy/web/ /usr/local/share/rpod/web/ && \
		sudo install -m 644 /tmp/rpod-deploy/rpod-web.service /etc/systemd/system/rpod-web.service && \
		sudo systemctl daemon-reload && \
		sudo systemctl enable rpod-web && \
		sudo systemctl restart rpod-web && \
		{ sudo -u $(PI_USER) test -w /media/music || \
		  echo "Note: /media/music isn'"'"'t writable by $(PI_USER), so uploads will fail: sudo chown -R $(PI_USER): /media/music"; }'

# --- Host unit tests ------------------------------------------------------
#
# Pure-logic modules only (no LVGL, no hardware), built for the dev machine
# with sanitizers on.

TEST_CFLAGS := -std=c17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I src

.PHONY: test
test: $(BUILD_DIR)/test/test_aap $(BUILD_DIR)/test/test_avrcp_volume \
      $(BUILD_DIR)/test/test_gestures $(BUILD_DIR)/test/test_alpha_sort \
      $(BUILD_DIR)/test/test_embedded_art $(BUILD_DIR)/test/test_cover_art \
      $(BUILD_DIR)/test/test_brick $(BUILD_DIR)/test/test_quiz \
      $(BUILD_DIR)/test/test_web
	$(BUILD_DIR)/test/test_aap
	$(BUILD_DIR)/test/test_avrcp_volume
	$(BUILD_DIR)/test/test_gestures
	$(BUILD_DIR)/test/test_alpha_sort
	$(BUILD_DIR)/test/test_embedded_art
	$(BUILD_DIR)/test/test_cover_art
	$(BUILD_DIR)/test/test_brick
	$(BUILD_DIR)/test/test_quiz
	$(BUILD_DIR)/test/test_web

$(BUILD_DIR)/test/test_aap: tests/test_aap.c src/audio/aap.c src/audio/aap.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) tests/test_aap.c src/audio/aap.c -o $@

$(BUILD_DIR)/test/test_avrcp_volume: tests/test_avrcp_volume.c src/audio/avrcp_volume.c src/audio/avrcp_volume.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) tests/test_avrcp_volume.c src/audio/avrcp_volume.c -o $@

$(BUILD_DIR)/test/test_gestures: tests/test_gestures.c src/input/gestures.c src/input/gestures.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) tests/test_gestures.c src/input/gestures.c -o $@

$(BUILD_DIR)/test/test_alpha_sort: tests/test_alpha_sort.c src/ui/alpha_sort.c src/ui/alpha_sort.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) tests/test_alpha_sort.c src/ui/alpha_sort.c -o $@

$(BUILD_DIR)/test/test_embedded_art: tests/test_embedded_art.c src/audio/embedded_art.c src/audio/embedded_art.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -D_DEFAULT_SOURCE tests/test_embedded_art.c src/audio/embedded_art.c -o $@

$(BUILD_DIR)/test/test_cover_art: tests/test_cover_art.c src/ui/cover_art.c src/ui/cover_art.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) tests/test_cover_art.c src/ui/cover_art.c -o $@ -lz -ljpeg

$(BUILD_DIR)/test/test_brick: tests/test_brick.c src/games/brick.c src/games/brick.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) tests/test_brick.c src/games/brick.c -o $@ -lm

$(BUILD_DIR)/test/test_quiz: tests/test_quiz.c src/games/quiz.c src/games/quiz.h src/games/scores.c src/games/scores.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -D_DEFAULT_SOURCE tests/test_quiz.c src/games/quiz.c src/games/scores.c -o $@

$(BUILD_DIR)/test/test_web: tests/test_web.c web/http.c web/http.h web/json.c web/json.h web/library.c web/library.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -D_GNU_SOURCE -I web tests/test_web.c web/http.c web/json.c web/library.c -o $@

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(SIM_BUILD_DIR)

# --- The iPod's fonts and bitmaps, from Apple's own firmware ----------------
#
# The 4th-gen iPod's firmware 3.1.1, from the URL iTunes' update catalog still
# lists for it. Apple serves it over plain HTTP only, hence the pinned hash.
# The extractor needs nothing beyond python3 with --no-png (Pillow is only for
# the PNG previews; re/ipod4/README.md). Both land in ignored paths.
IPOD_FW        := re/ipod4/iPod_4.3.1.1.ipsw
IPOD_FW_URL    := http://appldnld.apple.com.edgesuite.net/content.info.apple.com/iPod/SBML/osx/bundles/061-2691.20060912.ipDcw/iPod_4.3.1.1.ipsw
IPOD_FW_SHA256 := 576e05def7800e28cfd9a28ec36518964ab7a356f3b035e3fc223b0816be9ed9

.PHONY: ipod-assets
ipod-assets: re/ipod4/out/lvgl/ipod_images.h

re/ipod4/out/lvgl/ipod_images.h: $(IPOD_FW) re/ipod4/tools/extract.py $(wildcard re/ipod4/tools/ipod4re/*.py)
	python3 -I re/ipod4/tools/extract.py --no-png $(IPOD_FW) re/ipod4/out

$(IPOD_FW):
	curl -fL --retry 3 -o $@.part $(IPOD_FW_URL)
	echo '$(IPOD_FW_SHA256)  $@.part' | sha256sum -c --quiet
	mv $@.part $@

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
