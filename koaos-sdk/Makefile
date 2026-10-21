# KoaOS SDK -- build wasm apps and pack them into .esp packages
#
#   make                 build every app in apps/        -> dist/<name>.esp
#   make hellowasm       build one app
#   make serve           copy dist/*.esp into repo/ and start the install server
#   make selftest        test the SDK's printf on this computer (no wasm tools needed)
#   make iotest          test the pin policy + manifest parser used for GPIO/UART/I2C/SPI (no wasm tools needed)
#   make guitest         compile the desktop (KoaOS/koa_gui.h) on this computer and write screenshots to gui_shots/*.ppm
#   make clean
#
# Needs: clang with the wasm32 target + wasm-ld (Debian/Ubuntu: apt install clang lld),
#        python3.

ifeq ($(origin CC),default)
CC := clang
endif
PYTHON ?= python3
APPS   := $(sort $(notdir $(patsubst %/,%,$(dir $(wildcard apps/*/main.c)))))

# -mcpu=mvp: plain WebAssembly 1.0 only (the interpreter in the firmware doesn't need anything newer)
# -fno-builtin: the SDK's memcpy/memset etc. must not be turned into calls to themselves
CFLAGS  := --target=wasm32 -mcpu=mvp -Os -fno-unroll-loops -ffreestanding -fno-builtin -nostdlib -Iinclude \
           -Wall -Wextra -Werror=implicit-function-declaration
LDFLAGS := -nostdlib -fuse-ld=lld -Wl,--no-entry -Wl,--gc-sections -Wl,--strip-all \
           -Wl,-z,stack-size=16384 -Wl,--initial-memory=65536 -Wl,--max-memory=524288

.PHONY: all $(APPS) serve selftest mctest iotest guitest clean
all: $(APPS)
$(APPS): %: dist/%.esp

dist/%.esp: apps/%/manifest apps/%/main.c src/koaos.c $(wildcard include/*.h)
	@mkdir -p build/$* dist
	@rm -f build/$*/*
	$(CC) $(CFLAGS) $(LDFLAGS) apps/$*/*.c src/koaos.c -o build/$*/$*.wasm
	cp apps/$*/manifest build/$*/manifest
	@for f in apps/$*/*.bin; do [ -f "$$f" ] && cp "$$f" build/$*/; done; true
	$(PYTHON) tools/esppack.py pack build/$* -o $@

serve: all
	@mkdir -p repo
	cp dist/*.esp repo/
	$(PYTHON) tools/repo_server.py repo 8000

selftest:
	gcc -std=gnu11 -Wall -Wextra -Iinclude tests/host_test.c src/koaos.c -o build_selftest
	./build_selftest
	@rm -f build_selftest

# GPIO/UART/I2C/SPI: pin policy and manifest capabilities (second build allows every pin: the OS pins must still be refused)
iotest:
	gcc -std=gnu11 -Wall -Wextra -Iinclude tests/io_test.c -o io_test
	./io_test
	gcc -std=gnu11 -Wall -Wextra -Iinclude -DKOA_IO_ALLOW_MASK_TEST -DKOA_IO_ALLOW_MASK=~0ULL tests/io_test.c -o io_test_all
	./io_test_all
	@rm -f io_test io_test_all

# MeshCore app: native tests (crypto vs python-cryptography, then the whole app against scripted radio traffic)
mctest:
	gcc -std=gnu11 -O2 -Wall -Wextra -fsanitize=address,undefined tests/mc_crypto_test.c apps/meshcore/mc_crypto.c -o mc_crypto_test
	$(PYTHON) tests/mc_crypto_check.py
	gcc -std=gnu11 -O1 -g -Wall -Wextra -Iinclude -fsanitize=address,undefined tests/mc_mock.c apps/meshcore/main.c apps/meshcore/mc_crypto.c src/koaos.c -o mc_app_host
	$(PYTHON) tests/mc_app_test.py
	@rm -f mc_crypto_test mc_app_host

guitest:
	mkdir -p gui_shots
	g++ -std=gnu++17 -Wall -I../KoaOS tests/gui_host.cpp -o gui_host
	./gui_host gui_shots
	@rm -f gui_host

clean:
	rm -rf build dist build_selftest mc_crypto_test mc_app_host io_test io_test_all gui_host gui_shots
