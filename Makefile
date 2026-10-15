CC = cc
CFLAGS = -O2 -std=c89 -pedantic -Wall -Wextra -Werror

.PHONY: test test-firmware test-covers test-sdk clean
test: build/test_library_navigation build/test_library_features build/test_bus_diagnostics build/test_nvram build/test_saves build/test_menu_sort build/test_ode build/test_menu_config build/test_menu_ui build/test_usb_art
	python3 tests/test_format.py
	python3 tests/test_restore_workflow.py
	./build/test_library_navigation
	./build/test_library_features
	./build/test_bus_diagnostics
	./build/test_nvram
	./build/test_saves
	./build/test_menu_sort
	./build/test_ode
	./build/test_menu_config
	./build/test_menu_ui
	./build/test_usb_art

test-covers: build/test_usb_art
	python3 tests/test_cover_tools.py

test-firmware:
	python3 tests/test_firmware_navigation.py

build/test_menu_sort: src/menu_sort.c src/menu_sort.h tests/test_menu_sort.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_sort.c tests/test_menu_sort.c $(LDFLAGS) -o $@

build/test_ode: src/menu_sort.c src/menu_sort.h src/ode.c src/ode.h tests/test_ode.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_sort.c src/ode.c tests/test_ode.c $(LDFLAGS) -o $@

build/test_menu_config: src/menu_config.c src/menu_config.h src/ode.h tests/test_menu_config.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_config.c tests/test_menu_config.c $(LDFLAGS) -o $@

build/test_menu_ui: src/menu_ui.c src/menu_ui.h tests/test_menu_ui.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_ui.c tests/test_menu_ui.c $(LDFLAGS) -o $@

build/test_usb_art: src/usb_art.c src/usb_art.h src/menu_config.h src/ode.h src/menu_ui.c src/menu_ui.h src/cover_catalog.c tests/test_usb_art.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/usb_art.c src/menu_ui.c src/cover_catalog.c tests/test_usb_art.c $(LDFLAGS) -o $@

clean:
	rm -f build/test_library_navigation build/test_library_features build/test_bus_diagnostics build/test_nvram build/test_saves build/test_menu_sort build/test_ode build/test_menu_config build/test_menu_ui build/test_usb_art

build/test_saves: src/save_archive.c src/save_archive.h src/save_usb.c src/save_usb.h src/usb_art.c src/usb_file.h src/menu_ui.c src/menu_ui.h src/cover_catalog.c src/menu_settings.c src/menu_settings.h src/menu_config.c tests/test_saves.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/save_archive.c src/save_usb.c src/usb_art.c src/menu_ui.c src/cover_catalog.c src/menu_settings.c src/menu_config.c tests/test_saves.c $(LDFLAGS) -o $@

build/test_nvram: src/nvram_io.c src/nvram_io.h tests/test_nvram.c $(wildcard tests/nvram_sdk/*.h) src/save_archive.c src/save_archive.h src/usb_art.c src/usb_file.h src/menu_ui.c src/menu_ui.h src/cover_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/nvram_sdk -Isrc src/nvram_io.c src/save_archive.c src/usb_art.c src/menu_ui.c src/cover_catalog.c tests/test_nvram.c $(LDFLAGS) -o $@

build/test_bus_diagnostics: src/ode_bus.c src/ode_bus.h tests/test_bus_diagnostics.c $(wildcard tests/nvram_sdk/*.h)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/nvram_sdk -Isrc src/ode_bus.c tests/test_bus_diagnostics.c $(LDFLAGS) -o $@

test-sdk:
	python3 tests/test_format_abi.py

build/test_library_features: src/library_features.c src/library_features.h src/ode.c src/ode.h src/menu_ui.c tests/test_library_features.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/library_features.c src/ode.c src/menu_sort.c src/menu_ui.c src/cover_catalog.c tests/test_library_features.c $(LDFLAGS) -o $@

build/test_library_navigation: src/library_navigation.c src/library_navigation.h src/menu_ui.c tests/test_library_navigation.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/library_navigation.c src/menu_sort.c src/menu_ui.c src/cover_catalog.c tests/test_library_navigation.c $(LDFLAGS) -o $@
