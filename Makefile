CC = cc
CFLAGS = -O2 -std=c89 -pedantic -Wall -Wextra -Werror
# The OM3DVID7 preview codec library (om3dvid/).
OM3DVID_INCLUDE = -Iom3dvid/include
OM3DVID_DECODER = om3dvid/src/om3dvid7_decoder.c
OM3DVID_PLAYER = om3dvid/src/om3dvid7_player.c

.PHONY: test test-firmware test-covers test-sdk test-media clean
test: build/test_game_info build/test_menu_help build/test_menu_categories build/test_save_catalog build/test_save_cheats build/test_cover_cache build/test_om3dvid7_sound build/test_ui_font build/test_usb_media build/test_usb_media_sdk build/test_menu_position build/test_library_navigation build/test_library_features build/test_bus_diagnostics build/test_nvram build/test_saves build/test_menu_sort build/test_ode build/test_menu_config build/test_menu_ui build/test_usb_art build/test_save_names build/test_om3dvid7_encoder build/test_om3dvid7_player build/test_preview_stream
	python3 tools/build-save-names.py --check
	./build/test_game_info tests/fixtures/game-info.txt
	./build/test_menu_help
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
	python3 tests/test_release.py
	python3 tests/test_update_manifest.py
	python3 tests/test_metadata_tools.py
	python3 tests/test_format.py
	python3 tests/test_restore_workflow.py
	python3 tests/test_queue_workflow.py
	python3 tests/test_menu_lookup_cache.py
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
	./build/test_om3dvid7_sound
	./build/test_om3dvid7_encoder build/test-om3dvid7.3dv
	./build/test_om3dvid7_player build/test-om3dvid7.3dv
	python3 om3dvid/tests/test_python.py build/media-native build/test-om3dvid7.3dv
	./build/test_preview_stream build/test-om3dvid7.3dv
	./build/test_usb_media streamvideo build/test-om3dvid7.3dv
	./build/test_usb_media_sdk streamvideo build/test-om3dvid7.3dv
	./build/test_usb_art

test-covers: test-cover-resize build/test_usb_media
	python3 tests/test_cover_tools.py

test-firmware:
	python3 tests/test_firmware_navigation.py

build/test_menu_sort: src/menu_sort.c src/menu_sort.h tests/test_menu_sort.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_sort.c tests/test_menu_sort.c $(LDFLAGS) -o $@

build/test_ode: src/menu_sort.c src/menu_sort.h src/ode.c src/ode.h tests/test_ode.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_sort.c src/ode.c tests/test_ode.c $(LDFLAGS) -o $@

build/test_menu_config: src/menu_config.c src/menu_config.h src/ode.h tests/test_menu_config.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_config.c tests/test_menu_config.c $(LDFLAGS) -o $@

build/test_menu_ui: src/menu_ui.c src/menu_categories.c src/menu_ui.h tests/test_menu_ui.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_ui.c src/menu_categories.c tests/test_menu_ui.c $(LDFLAGS) -o $@

build/test_usb_art: src/usb_art.c src/usb_file.c src/usb_art.h src/menu_config.h src/ode.h src/menu_ui.c src/menu_categories.c src/menu_ui.h src/cover_catalog.c tests/test_usb_art.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_usb_art.c $(LDFLAGS) -o $@

clean:
	rm -f build/test_om3dvid7_encoder build/test_om3dvid7_player build/test_preview_stream build/test-om3dvid7.3dv build/test_game_info build/test_menu_help build/test_save_cheats build/test_cover_cache build/test_om3dvid7_sound build/test_ui_font build/test_usb_media build/test_usb_media_sdk build/test_menu_position build/test_library_navigation build/test_library_features build/test_bus_diagnostics build/test_nvram build/test_saves build/test_menu_sort build/test_ode build/test_menu_config build/test_menu_ui build/test_usb_art build/test_save_names

build/test_saves: src/save_archive.c src/save_archive.h src/save_usb.c src/save_usb.h src/usb_art.c src/usb_file.c src/usb_file.h src/menu_ui.c src/menu_categories.c src/menu_ui.h src/cover_catalog.c src/menu_settings.c src/menu_settings.h src/menu_config.c tests/test_saves.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/save_archive.c src/save_usb.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/menu_settings.c src/menu_config.c tests/test_saves.c $(LDFLAGS) -o $@

build/test_nvram: src/nvram_io.c src/nvram_io.h tests/test_nvram.c $(wildcard tests/nvram_sdk/*.h) src/save_archive.c src/save_archive.h src/usb_art.c src/usb_file.c src/usb_file.h src/menu_ui.c src/menu_categories.c src/menu_ui.h src/cover_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/nvram_sdk -Isrc $(OM3DVID_INCLUDE) src/nvram_io.c src/save_archive.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_nvram.c $(LDFLAGS) -o $@

build/test_bus_diagnostics: src/ode_bus.c src/ode_bus.h tests/test_bus_diagnostics.c $(wildcard tests/nvram_sdk/*.h)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/nvram_sdk -Isrc $(OM3DVID_INCLUDE) src/ode_bus.c tests/test_bus_diagnostics.c $(LDFLAGS) -o $@

test-sdk:
	python3 tests/test_format_abi.py
	python3 tests/test_stack_usage.py
	python3 tests/test_runtime.py
	python3 tests/test_preview_asm.py
	python3 om3dvid/tests/test_decoder_arm.py

build/test_library_features: src/library_features.c src/library_features.h src/ode.c src/ode.h src/menu_ui.c src/menu_categories.c tests/test_library_features.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/library_features.c src/ode.c src/menu_sort.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_library_features.c $(LDFLAGS) -o $@

build/test_library_navigation: src/library_features.c src/library_navigation.c src/library_navigation.h src/menu_ui.c src/menu_categories.c tests/test_library_navigation.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/library_navigation.c src/library_features.c src/menu_sort.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_library_navigation.c $(LDFLAGS) -o $@

build/test_save_names: src/save_name_db.h src/save_names.c src/save_names.h src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_names.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/save_names.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_names.c $(LDFLAGS) -o $@

build/test_menu_position: src/menu_position.c src/menu_position.h tests/test_menu_position.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_position.c tests/test_menu_position.c $(LDFLAGS) -o $@

build/test_usb_media: src/library_index.c src/library_index.h tests/test_usb_media.c src/usb_media.c src/preview_v5.c $(OM3DVID_DECODER) src/preview_v5.h om3dvid/include/om3dvid7.h src/usb_media.h src/usb_art.c src/usb_file.c src/png_decode.c src/png_inflate.c $(wildcard vendor/pngle/*)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) tests/test_usb_media.c src/library_index.c src/library_navigation.c src/library_features.c src/ode.c src/menu_sort.c src/usb_media.c src/preview_v5.c $(OM3DVID_DECODER) src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/png_decode.c src/png_inflate.c $(LDFLAGS) -o $@

build/test_usb_media_sdk: src/library_index.c src/library_index.h tests/test_usb_media.c tests/media_sdk/mem.h src/usb_media.c src/preview_v5.c $(OM3DVID_DECODER) src/preview_v5.h om3dvid/include/om3dvid7.h src/usb_media.h src/usb_art.c src/usb_file.c src/png_decode.c src/png_inflate.c $(wildcard vendor/pngle/*)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DOH3DO_SDK=1 -Itests/media_sdk -Isrc $(OM3DVID_INCLUDE) tests/test_usb_media.c src/library_index.c src/library_navigation.c src/library_features.c src/ode.c src/menu_sort.c src/usb_media.c src/preview_v5.c $(OM3DVID_DECODER) src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/png_decode.c src/png_inflate.c $(LDFLAGS) -o $@

# The encoder test writes the synthetic clip the streaming test plays.
build/test_om3dvid7_encoder: om3dvid/src/om3dvid7_encoder.c $(OM3DVID_DECODER) om3dvid/include/om3dvid7.h om3dvid/include/om3dvid7_encoder.h om3dvid/tests/test_encoder.c
	mkdir -p build
	$(CC) $(CPPFLAGS) -O2 -std=c99 -pedantic -Wall -Wextra -Werror $(OM3DVID_INCLUDE) om3dvid/tests/test_encoder.c om3dvid/src/om3dvid7_encoder.c $(OM3DVID_DECODER) $(LDFLAGS) -lm -o $@

build/test_om3dvid7_player: $(OM3DVID_PLAYER) $(OM3DVID_DECODER) om3dvid/include/om3dvid7.h om3dvid/include/om3dvid7_player.h om3dvid/tests/test_player.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OM3DVID_INCLUDE) om3dvid/tests/test_player.c $(OM3DVID_PLAYER) $(OM3DVID_DECODER) $(LDFLAGS) -o $@

build/test_preview_stream: src/preview_stream.c src/preview_stream.h $(OM3DVID_DECODER) $(OM3DVID_PLAYER) om3dvid/include/om3dvid7.h om3dvid/include/om3dvid7_player.h tests/test_preview_stream.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/library_index.c src/library_navigation.c src/library_features.c src/ode.c src/menu_sort.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) tests/test_preview_stream.c src/preview_stream.c $(OM3DVID_DECODER) $(OM3DVID_PLAYER) src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c src/library_index.c src/library_navigation.c src/library_features.c src/ode.c src/menu_sort.c $(LDFLAGS) -o $@

test-media: build/test_usb_media build/test_usb_media_sdk
	python3 tests/test_media_tools.py

build/test_ui_font: src/ui_font.c src/ui_font.h src/ui_font_data.h tests/test_ui_font.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/ui_font.c tests/test_ui_font.c $(LDFLAGS) -o $@

.PHONY: test-cover-resize
test-cover-resize: build/test_usb_media
	python3 tests/test_resize_covers.py

build/test_om3dvid7_sound: om3dvid/src/om3dvid7_sound.c om3dvid/include/om3dvid7_sound.h om3dvid/tests/audio_sdk/audio.h om3dvid/tests/audio_sdk/mem.h om3dvid/tests/test_sound.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iom3dvid/tests/audio_sdk $(OM3DVID_INCLUDE) om3dvid/src/om3dvid7_sound.c om3dvid/tests/test_sound.c $(LDFLAGS) -o $@



build/test_cover_cache: src/cover_cache.c src/cover_cache.h src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_cover_cache.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/cover_cache.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_cover_cache.c $(LDFLAGS) -o $@

.PHONY: test-desktop
test-desktop:
	cargo test --locked --manifest-path desktop/Cargo.toml

build/test_save_cheats: src/save_cheats.c src/save_cheats.h src/save_cheat_db.h tests/test_save_cheats.c src/save_archive.c src/save_archive.h src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/save_cheats.c src/save_archive.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_cheats.c $(LDFLAGS) -o $@

build/test_save_catalog: src/save_catalog.c src/save_catalog.h src/save_name_db.h src/save_names.c src/save_archive.c src/usb_art.c src/usb_file.c tests/test_save_catalog.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/save_catalog.c src/save_names.c src/save_archive.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_save_catalog.c $(LDFLAGS) -o $@

build/test_menu_categories: src/menu_categories.c src/menu_categories.h src/menu_ui.c tests/test_menu_categories.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_categories.c src/menu_ui.c src/cover_catalog.c tests/test_menu_categories.c $(LDFLAGS) -o $@

build/test_menu_help: src/menu_help.c src/menu_help.h tests/test_menu_help.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/menu_help.c tests/test_menu_help.c $(LDFLAGS) -o $@

build/test_game_info: src/game_info.c src/game_info.h tests/test_game_info.c
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(OM3DVID_INCLUDE) src/game_info.c src/usb_art.c src/usb_file.c src/menu_ui.c src/menu_categories.c src/cover_catalog.c tests/test_game_info.c $(LDFLAGS) -o $@
