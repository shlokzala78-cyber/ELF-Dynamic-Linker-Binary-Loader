CC ?= gcc
BUILD ?= build
CPPFLAGS := -D_GNU_SOURCE -Iinclude
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wconversion
LDFLAGS ?=
LDLIBS :=
CORE := util elf dynamic memory relocation linker inspect
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE))) $(BUILD)/call.o
PICFLAGS := -O1 -g -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
SOFLAGS := -nostdlib -shared -Wl,-z,relro,-z,now,-z,noexecstack
FIX := $(BUILD)/fixtures
FIXTURES := $(FIX)/app.so $(FIX)/override.so $(FIX)/app-gnu.so $(FIX)/app-sysv.so $(FIX)/unresolved.so $(FIX)/tls.so $(FIX)/protected-app.so

.PHONY: all fixtures demo test unit integration sanitize clean
all: $(BUILD)/elflab fixtures
fixtures: $(FIXTURES)

$(BUILD) $(FIX):
	mkdir -p $@

$(BUILD)/%.o: src/%.c include/elflab.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/call.o: src/call.S | $(BUILD)
	$(CC) -g -c $< -o $@

$(BUILD)/elflab: $(OBJECTS) $(BUILD)/main.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(FIX)/libbase.so: examples/base.c examples/add.S | $(FIX)
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both,-soname,libbase.so $^ -o $@

$(FIX)/libmath.so: examples/math.c $(FIX)/libbase.so
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both,-soname,libmath.so $< -L$(FIX) -lbase -o $@

$(FIX)/app.so: examples/app.c $(FIX)/libmath.so
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both $< -L$(FIX) -lmath -o $@

$(FIX)/app-gnu.so: examples/app.c $(FIX)/libmath.so
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=gnu $< -L$(FIX) -lmath -o $@

$(FIX)/app-sysv.so: examples/app.c $(FIX)/libmath.so
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=sysv $< -L$(FIX) -lmath -o $@

$(FIX)/override.so: examples/override.c $(FIX)/libmath.so
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both $< -L$(FIX) -lmath -o $@

$(FIX)/unresolved.so: examples/unresolved.c | $(FIX)
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both $< -o $@

$(FIX)/tls.so: examples/tls.c | $(FIX)
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both $< -o $@

$(FIX)/libprotected.so: examples/protected.c | $(FIX)
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both,-soname,libprotected.so $< -o $@

$(FIX)/protected-app.so: examples/protected_app.c $(FIX)/libprotected.so
	$(CC) $(PICFLAGS) $(SOFLAGS) -Wl,--hash-style=both $< -L$(FIX) -lprotected -o $@

$(BUILD)/unit-tests: tests/unit.c $(OBJECTS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

unit: $(BUILD)/unit-tests fixtures
	$(BUILD)/unit-tests $(FIX)/app.so

integration: all
	python3 tests/integration.py $(BUILD)/elflab $(FIX)

test: unit integration

demo: all
	$(BUILD)/elflab inspect $(FIX)/app.so
	$(BUILD)/elflab deps $(FIX)/app.so
	$(BUILD)/elflab maps $(FIX)/app.so
	$(BUILD)/elflab run $(FIX)/app.so --trace
	$(BUILD)/elflab run $(FIX)/override.so

sanitize:
	$(MAKE) BUILD=build-sanitize CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" LDFLAGS="-fsanitize=address,undefined -no-pie" test

clean:
	rm -rf build build-sanitize

-include $(wildcard $(BUILD)/*.d)
