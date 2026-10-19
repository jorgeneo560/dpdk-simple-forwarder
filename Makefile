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

# Unit tests: no DPDK needed, they build only the file they test.
TEST_WRITER := $(BUILD)/test_writer
TEST_PARSE  := $(BUILD)/test_parse
TEST_RSS    := $(BUILD)/test_rss
TEST_CFLAGS := -O0 -g -std=gnu11 $(WARNINGS) -Ilib

test: $(TEST_WRITER) $(TEST_PARSE) $(TEST_RSS)
	@echo "  RUN   $(TEST_WRITER)"
	$(Q)$(TEST_WRITER)
	@echo "  RUN   $(TEST_PARSE)"
	$(Q)$(TEST_PARSE)
	@echo "  RUN   $(TEST_RSS)"
	$(Q)$(TEST_RSS)

$(TEST_WRITER): tests/test_writer.c lib/writer.c lib/writer.h lib/flow.h
	@mkdir -p $(BUILD)
	@echo "  CC    $@"
	$(Q)$(CC) $(TEST_CFLAGS) tests/test_writer.c lib/writer.c -o $@

$(TEST_PARSE): tests/test_parse.c lib/parse.c lib/parse.h
	@mkdir -p $(BUILD)
	@echo "  CC    $@"
	$(Q)$(CC) $(TEST_CFLAGS) tests/test_parse.c lib/parse.c -o $@

$(TEST_RSS): tests/test_rss.c lib/rss.c lib/rss.h
	@mkdir -p $(BUILD)
	@echo "  CC    $@"
	$(Q)$(CC) $(TEST_CFLAGS) tests/test_rss.c lib/rss.c -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
