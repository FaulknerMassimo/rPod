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
PI_USER     ?= pi
CROSS       ?= aarch64-linux-gnu-
CC_CROSS    := $(CROSS)gcc

LVGL_DIR    := third_party/lvgl
BUILD_DIR   := build
SIM_BUILD_DIR := build-sim

# --- Desktop simulator ------------------------------------------------------

LVGL_SRCS   := $(shell find $(LVGL_DIR)/src -name '*.c')
SIM_SRCS    := tools/sim/sim_main.c $(LVGL_SRCS)
SIM_OBJS    := $(patsubst %.c,$(SIM_BUILD_DIR)/%.o,$(SIM_SRCS))

SIM_CFLAGS  := -std=c17 -Wall -Wextra -O0 -g \
               -I tools/sim -I $(LVGL_DIR) \
               $(shell pkg-config --cflags sdl2)
SIM_LDFLAGS := $(shell pkg-config --libs sdl2) -lm -lpthread

.PHONY: sim
sim: $(SIM_BUILD_DIR)/rpod-sim
	$(SIM_BUILD_DIR)/rpod-sim

$(SIM_BUILD_DIR)/rpod-sim: $(SIM_OBJS)
	$(CC) $(SIM_OBJS) -o $@ $(SIM_LDFLAGS)

$(SIM_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(SIM_CFLAGS) -c $< -o $@

# --- On-device (cross) build -------------------------------------------------

APP_SRCS    := $(shell find src -name '*.c')
APP_OBJS    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SRCS))

APP_CFLAGS  := -std=c17 -Wall -Wextra -O2 -g -I $(LVGL_DIR)
APP_LDFLAGS := -lm -lpthread

.PHONY: build
build: $(BUILD_DIR)/rpod

$(BUILD_DIR)/rpod: $(APP_OBJS)
	$(CC_CROSS) $(APP_OBJS) -o $@ $(APP_LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC_CROSS) $(APP_CFLAGS) -c $< -o $@

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

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(SIM_BUILD_DIR)
