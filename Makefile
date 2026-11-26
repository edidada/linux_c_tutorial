CC      ?= gcc
CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 -pthread
BUILD   := build
SRCS    := $(wildcard labs/*/*.c)
BINS    := $(patsubst labs/%.c,$(BUILD)/%,$(SRCS))

.PHONY: all clean list
all: $(BINS)
	@echo "built $(words $(BINS)) binaries under $(BUILD)/"

$(BUILD)/%: labs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

list:
	@for b in $(BINS); do echo $$b; done

clean:
	rm -rf $(BUILD)
