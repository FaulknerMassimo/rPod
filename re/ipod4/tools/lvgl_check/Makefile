# Headless LVGL 9.5 render of the generated fonts/images, for diffing against tools/ipod4re/views.py.
#   make -C tools/lvgl_check && tools/lvgl_check/render_test row.pgm
RPOD ?= $(abspath ../../../..)
OUT  ?= $(abspath ../../out/lvgl)
L    := $(RPOD)/third_party/lvgl
SRCS := $(shell find $(L)/src -name '*.c') $(wildcard $(OUT)/fonts/*.c) $(OUT)/ipod_images.c \
        $(RPOD)/src/ui/fonts/lv_font_montserrat_14.c
OBJS := $(patsubst /%,obj/%,$(SRCS:.c=.o))
CFLAGS := -O1 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I$(L) -I$(L)/.. -I$(OUT) -I$(OUT)/fonts

render_test: $(OBJS) obj/render_test.o
	$(CC) $^ -o $@ -lm
obj/render_test.o: render_test.c lv_conf.h
	@mkdir -p obj; $(CC) $(CFLAGS) -c $< -o $@
obj/%.o: /%.c lv_conf.h
	@mkdir -p $(dir $@); $(CC) $(CFLAGS) -c $< -o $@
lv_conf.h:
	sed 's/^#define LV_USE_SDL              1/#define LV_USE_SDL              0/' $(RPOD)/tools/sim/lv_conf.h > $@
clean:
	rm -rf obj render_test lv_conf.h
