CFLAGS = -O3 -std=c99
PREFIX=$(MIX_APP_PATH)/priv

ifndef MIX_ENV
	MIX_ENV = dev
endif

ifdef DEBUG
	CFLAGS +=  -pedantic -Weverything -Wall -Wextra -Wno-unused-parameter -Wno-gnu
endif

ifeq ($(MIX_ENV),dev)
	CFLAGS += -g
endif

CFLAGS += -fPIC -I$(NERVES_SDK_SYSROOT)/usr/include/drm
LDFLAGS += -lGLESv2 -lm -lrt -ldl -lEGL -lgbm -ldrm

.PHONY: all clean

all: $(PREFIX)/$(MIX_ENV)/scenic_driver_egl
# fonts

SRCS = c_src/main.c c_src/comms.c c_src/nanovg/nanovg.c \
	c_src/utils.c c_src/render_script.c c_src/tx.c

$(PREFIX)/$(MIX_ENV)/scenic_driver_egl: $(SRCS)
	mkdir -p $(PREFIX)/$(MIX_ENV)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	$(RM) -r $(PREFIX)/$(MIX_ENV)
