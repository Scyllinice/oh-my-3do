CC = cc
CFLAGS = -O2 -std=c89 -pedantic -Wall -Wextra -Werror

.PHONY: test test-firmware test-covers test-sdk test-media clean
test: build/test_menu_categories build/test_save_catalog build/test_save_cheats build/test_cover_cache build/test_preview_audio build/test_ui_font build/test_usb_media build/test_usb_media_sdk build/test_menu_position build/test_library_navigation build/test_library_features build/test_bus_diagnostics build/test_nvram build/test_saves build/test_menu_sort build/test_ode build/test_menu_config build/test_menu_ui build/test_usb_art build/test_save_names
	python3 tools/build-save-names.py --check
	./build/test_menu_categories
	./build/test_save_catalog
	python3 tools/build-save-cheats.py --check --binary build/save-cheats.3db
	./build/test_save_cheats build/save-cheats.3db
	python3 tools/build-sector-codec.py
	python3 tools/build-preview-encoder.py
	python3 tests/test_sector_codec.py
	python3 tests/test_organize_library.py
	python3 tests/test_library_maintenance.py
	python3 tests/test_library_catalog.py
	python3 tests/test_prepare_audio_disc.py
	python3 tests/test_preview_tools.py
	python3 tests/test_media_tools.py
	python3 tests/test_metadata_tools.py
	python3 tests/test_format.py
	python3 tests/test_restore_workflow.py
	python3 tests/test_queue_workflow.py
	python3 tests/test_game_folder_workflow.py
	python3 tests/test_recovery_workflow.py
	python3 tests/test_startup_workflow.py
	python3 tests/test_favorites_workflow.py
	./build/test_menu_position
	./build/test_save_names
	./build/test_library_navigation
	./build/test_library_features
	./build/test_bus_diagnostics
	./build/test_nvram
	./build/test_saves
	./build/test_menu_sort
	./build/test_ode
	./build/test_menu_config
	./build/test_menu_ui
	./build/test_cover_cache
	./build/test_ui_font
	./build/test_preview_audio
	./build/test_usb_art

test-covers: test-cover-resize build/test_usb_media
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

build/test_menu_ui: src/menu_ui.c src/menu_categories.c src/menu_ui.h tests/test_menu_ui.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_ui.c src/menu_categories.c tests/test_menu_ui.c $(LDFLAGS) -o $@

build/test_usb_art: src/usb_art.c src/usb_art.h src/menu_config.h src/ode.h src/menu_ui.c src/menu_categories.c src/menu_ui.h src/cover_catalog.c tests/test_usb_art.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_usb_art.c $(LDFLAGS) -o $@

clean:
	rm -f build/test_save_cheats build/test_cover_cache build/test_preview_audio build/test_ui_font build/test_usb_media build/test_usb_media_sdk build/test_menu_position build/test_library_navigation build/test_library_features build/test_bus_diagnostics build/test_nvram build/test_saves build/test_menu_sort build/test_ode build/test_menu_config build/test_menu_ui build/test_usb_art build/test_save_names

build/test_saves: src/save_archive.c src/save_archive.h src/save_usb.c src/save_usb.h src/usb_art.c src/usb_file.h src/menu_ui.c src/menu_categories.c src/menu_ui.h src/cover_catalog.c src/menu_settings.c src/menu_settings.h src/menu_config.c tests/test_saves.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/save_archive.c src/save_usb.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/menu_settings.c src/menu_config.c tests/test_saves.c $(LDFLAGS) -o $@

build/test_nvram: src/nvram_io.c src/nvram_io.h tests/test_nvram.c $(wildcard tests/nvram_sdk/*.h) src/save_archive.c src/save_archive.h src/usb_art.c src/usb_file.h src/menu_ui.c src/menu_categories.c src/menu_ui.h src/cover_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/nvram_sdk -Isrc src/nvram_io.c src/save_archive.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_nvram.c $(LDFLAGS) -o $@

build/test_bus_diagnostics: src/ode_bus.c src/ode_bus.h tests/test_bus_diagnostics.c $(wildcard tests/nvram_sdk/*.h)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/nvram_sdk -Isrc src/ode_bus.c tests/test_bus_diagnostics.c $(LDFLAGS) -o $@

test-sdk:
	python3 tests/test_format_abi.py
	python3 tests/test_stack_usage.py
	python3 tests/test_runtime.py
	python3 tests/test_preview_asm.py

build/test_library_features: src/library_features.c src/library_features.h src/ode.c src/ode.h src/menu_ui.c src/menu_categories.c tests/test_library_features.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/library_features.c src/ode.c src/menu_sort.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_library_features.c $(LDFLAGS) -o $@

build/test_library_navigation: src/library_features.c src/library_navigation.c src/library_navigation.h src/menu_ui.c src/menu_categories.c tests/test_library_navigation.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/library_navigation.c src/library_features.c src/menu_sort.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_library_navigation.c $(LDFLAGS) -o $@

build/test_save_names: src/save_name_db.h src/save_names.c src/save_names.h src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_names.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/save_names.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_names.c $(LDFLAGS) -o $@

build/test_menu_position: src/menu_position.c src/menu_position.h tests/test_menu_position.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_position.c tests/test_menu_position.c $(LDFLAGS) -o $@

build/test_usb_media: src/library_index.c src/library_index.h tests/test_usb_media.c src/usb_media.c src/preview_v5.c src/preview_v5.h src/usb_media.h src/usb_art.c src/png_decode.c src/png_inflate.c $(wildcard vendor/pngle/*)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc tests/test_usb_media.c src/library_index.c src/library_navigation.c src/library_features.c src/ode.c src/menu_sort.c src/usb_media.c src/preview_v5.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/png_decode.c src/png_inflate.c $(LDFLAGS) -o $@

build/test_usb_media_sdk: src/library_index.c src/library_index.h tests/test_usb_media.c tests/media_sdk/mem.h src/usb_media.c src/preview_v5.c src/preview_v5.h src/usb_media.h src/usb_art.c src/png_decode.c src/png_inflate.c $(wildcard vendor/pngle/*)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DOH3DO_SDK=1 -Itests/media_sdk -Isrc tests/test_usb_media.c src/library_index.c src/library_navigation.c src/library_features.c src/ode.c src/menu_sort.c src/usb_media.c src/preview_v5.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/png_decode.c src/png_inflate.c $(LDFLAGS) -o $@

test-media: build/test_usb_media build/test_usb_media_sdk
	python3 tests/test_media_tools.py

build/test_ui_font: src/ui_font.c src/ui_font.h src/ui_font_data.h tests/test_ui_font.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/ui_font.c tests/test_ui_font.c $(LDFLAGS) -o $@

.PHONY: test-cover-resize
test-cover-resize: build/test_usb_media
	python3 tests/test_resize_covers.py

build/test_preview_audio: src/preview_audio.c src/preview_audio.h tests/audio_sdk/audio.h tests/audio_sdk/mem.h tests/test_preview_audio.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/audio_sdk -Isrc src/preview_audio.c tests/test_preview_audio.c $(LDFLAGS) -o $@



build/test_cover_cache: src/cover_cache.c src/cover_cache.h src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_cover_cache.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/cover_cache.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_cover_cache.c $(LDFLAGS) -o $@

.PHONY: test-desktop
test-desktop:
	cargo test --locked --manifest-path desktop/Cargo.toml

build/test_save_cheats: src/save_cheats.c src/save_cheats.h src/save_cheat_db.h tests/test_save_cheats.c src/save_archive.c src/save_archive.h src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/save_cheats.c src/save_archive.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_cheats.c $(LDFLAGS) -o $@

build/test_save_catalog: src/save_catalog.c src/save_catalog.h src/save_name_db.h src/save_names.c src/save_archive.c src/usb_art.c tests/test_save_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/save_catalog.c src/save_names.c src/save_archive.c src/usb_art.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_catalog.c $(LDFLAGS) -o $@

build/test_menu_categories: src/menu_categories.c src/menu_categories.h src/menu_ui.c tests/test_menu_categories.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/menu_categories.c src/menu_ui.c src/cover_catalog.c tests/test_menu_categories.c $(LDFLAGS) -o $@
