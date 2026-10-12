APP     := flowfwd
SRCS    := src/main.c lib/hugepages.c lib/nics.c lib/pmd_ids.c lib/config.c lib/forward.c lib/logger.c lib/export.c lib/flow_export.c lib/parse.c lib/ports.c
BUILD   := build
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

PKGCONF ?= pkg-config
CFLAGS  += -O3 -g -Wall -Wextra -std=gnu11 -Ilib $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk) -pthread

all: $(BUILD)/$(APP)

$(BUILD)/$(APP): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD)/%.o: %.c $(wildcard lib/*.h src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

TEST_EXPORT := $(BUILD)/test_export
TEST_PARSE  := $(BUILD)/test_parse

test: $(TEST_EXPORT) $(TEST_PARSE)
	$(TEST_EXPORT)
	$(TEST_PARSE)

$(TEST_EXPORT): tests/test_export.c lib/export.c lib/export.h lib/flow.h
	@mkdir -p $(BUILD)
	$(CC) -O0 -g -Wall -Wextra -std=gnu11 -Ilib tests/test_export.c lib/export.c -o $@

$(TEST_PARSE): tests/test_parse.c lib/parse.c lib/parse.h
	@mkdir -p $(BUILD)
	$(CC) -O0 -g -Wall -Wextra -std=gnu11 -Ilib tests/test_parse.c lib/parse.c -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
