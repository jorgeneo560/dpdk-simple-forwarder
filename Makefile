APP   := flowfwd
BUILD := build

# Quiet by default, run "make V=1" to see the full compiler commands.
Q := $(if $(V),,@)

SRCS := $(wildcard src/*.c lib/*.c)
OBJS := $(SRCS:%.c=$(BUILD)/%.o)

# Any warning stops the build.
WARNINGS := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
            -Wformat=2 -Wundef -Wpointer-arith -Wwrite-strings -Wvla

PKGCONF ?= pkg-config
# -MMD writes a .d file per object so a changed header rebuilds what includes it.
CFLAGS  += -O3 -g -std=gnu11 $(WARNINGS) -MMD -MP -Ilib $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk) -pthread

all: $(BUILD)/$(APP)

$(BUILD)/$(APP): $(OBJS)
	@echo "  LINK  $@"
	$(Q)$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
