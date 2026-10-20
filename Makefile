CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS +=

BUILD := build

.PHONY: all clean fetch-sdk check

RUNTIME_LIBS := $(BUILD)/libc.so $(BUILD)/libm.so $(BUILD)/libdl.so $(BUILD)/libstdc++.so

all: $(BUILD)/vveye-p2p $(BUILD)/liblog.so $(RUNTIME_LIBS)

$(BUILD):
	mkdir -p $@

$(BUILD)/vveye-p2p: src/vveye-p2p.c include/libt2u.h | $(BUILD)
	@test -f include/libt2u.h || { echo "error: include/libt2u.h is missing; run 'make fetch-sdk'" >&2; exit 1; }
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/vveye-p2p.c $(LDFLAGS) -ldl

$(BUILD)/libc.so: | $(BUILD)
	@target=$$(ldconfig -p 2>/dev/null | awk '$$1 == "libc.so.6" {print $$NF; exit}'); \
	[ -n "$$target" ] || { echo "cannot locate libc.so.6" >&2; exit 1; }; \
	ln -sfn "$$target" $@

$(BUILD)/libm.so: | $(BUILD)
	@target=$$(ldconfig -p 2>/dev/null | awk '$$1 == "libm.so.6" {print $$NF; exit}'); \
	[ -n "$$target" ] || { echo "cannot locate libm.so.6" >&2; exit 1; }; \
	ln -sfn "$$target" $@

$(BUILD)/libdl.so: | $(BUILD)
	@target=$$(ldconfig -p 2>/dev/null | awk '$$1 == "libdl.so.2" {print $$NF; exit}'); \
	[ -n "$$target" ] || { echo "cannot locate libdl.so.2" >&2; exit 1; }; \
	ln -sfn "$$target" $@

$(BUILD)/libstdc++.so: | $(BUILD)
	@target=$$(ldconfig -p 2>/dev/null | awk '$$1 == "libstdc++.so.6" {print $$NF; exit}'); \
	[ -n "$$target" ] || { echo "cannot locate libstdc++.so.6" >&2; exit 1; }; \
	ln -sfn "$$target" $@

$(BUILD)/liblog.so: src/t2u-compat.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared \
		-Wl,-soname,liblog.so -o $@ src/t2u-compat.c -ldl

fetch-sdk: all
	sh vendor/fetch-libt2u.sh

check: all
	./vveye-p2p-client --help >/dev/null

clean:
	rm -rf $(BUILD)
