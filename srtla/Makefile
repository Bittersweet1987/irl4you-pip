# irl4you-srtla: eigener SRTLA-Sender (siehe README.md)
VERSION ?= $(shell cat VERSION 2>/dev/null || echo dev)
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -Werror -D_FORTIFY_SOURCE=2 -fstack-protector-strong
CPPFLAGS += -DIRL4YOU_SRTLA_VERSION='"$(VERSION)"'
LDLIBS += -lm
PREFIX ?= /usr/local
BUILD ?= build

all: $(BUILD)/irl4you-srtla

$(BUILD)/irl4you-srtla: src/irl4you-srtla.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

test: $(BUILD)/irl4you-srtla
	IRL4YOU_SRTLA_BIN=$(abspath $(BUILD)/irl4you-srtla) python3 -m unittest discover -s tests -v

install: $(BUILD)/irl4you-srtla
	install -D -m 755 $(BUILD)/irl4you-srtla $(DESTDIR)$(PREFIX)/bin/irl4you-srtla
	install -D -m 755 testrecv/irl4you-srtla-testrecv.py $(DESTDIR)$(PREFIX)/bin/irl4you-srtla-testrecv

clean:
	rm -rf $(BUILD)

.PHONY: all test install clean
