CC ?= gcc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -pthread
LDFLAGS ?=
LDLIBS ?= -lusb-1.0
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD_DIR ?= $(ROOT_DIR)/.build/unraid
BINARY := $(BUILD_DIR)/harboros-subscreen
EC_TEST := $(BUILD_DIR)/test-ec-permissions
SOURCE := $(ROOT_DIR)/unraid/src/panel.c
HEADERS := $(ROOT_DIR)/protocol.h

.PHONY: all clean test

all: $(BINARY)

$(BUILD_DIR):
	mkdir -p "$@"

$(BINARY): $(SOURCE) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I"$(ROOT_DIR)" -ffile-prefix-map="$(ROOT_DIR)"=. $(CFLAGS) $(LDFLAGS) -o "$@" "$(SOURCE)" $(LDLIBS)

clean:
	rm -f "$(BINARY)" "$(EC_TEST)"

$(EC_TEST): $(ROOT_DIR)/unraid/tests/ec_permissions_test.c $(SOURCE) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I"$(ROOT_DIR)" -ffile-prefix-map="$(ROOT_DIR)"=. $(CFLAGS) $(LDFLAGS) -o "$@" "$(ROOT_DIR)/unraid/tests/ec_permissions_test.c" $(LDLIBS)

test: $(EC_TEST)
	"$(EC_TEST)"
