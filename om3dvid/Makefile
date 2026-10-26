# The om3dvid library's PC side, on its own: the shared library the Python
# module loads, and the tests. Nothing outside this folder is needed.
#
#   make          build/om3dvid7.so (.dylib on macOS): encoder, decoder, reader
#   make test     the C tests, then the Python tests (needs python3)
#   make test-rust  the Rust crate's tests (needs cargo)
#   make clean
#
# The console side (decoder assembly, player, Portfolio helpers, sound) is
# compiled by the 3DO program that uses it; see README.md.

CC ?= cc
BUILD ?= build
PYTHON ?= python3

ifeq ($(shell uname -s),Darwin)
SHARED_LIBRARY = $(BUILD)/om3dvid7.dylib
SHARED_FLAG = -dynamiclib
else
SHARED_LIBRARY = $(BUILD)/om3dvid7.so
SHARED_FLAG = -shared
endif

INCLUDE = -Iinclude
C89 = -O2 -std=c89 -pedantic -Wall -Wextra -Werror
C99 = -O2 -std=c99 -pedantic -Wall -Wextra -Werror

DECODER = src/om3dvid7_decoder.c
PC_SOURCES = src/om3dvid7_encoder.c $(DECODER) src/om3dvid7_reader.c
HEADERS = $(wildcard include/*.h)
TEST_CLIP = $(BUILD)/test.3dv

.PHONY: all test test-rust clean

all: $(SHARED_LIBRARY)

$(SHARED_LIBRARY): $(PC_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(C99) $(INCLUDE) $(SHARED_FLAG) -fPIC $(PC_SOURCES) -o $@

$(BUILD)/test_encoder: tests/test_encoder.c src/om3dvid7_encoder.c $(DECODER) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(C99) $(INCLUDE) tests/test_encoder.c src/om3dvid7_encoder.c $(DECODER) -lm -o $@

$(BUILD)/test_reader: tests/test_reader.c src/om3dvid7_reader.c $(DECODER) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(C99) $(INCLUDE) tests/test_reader.c src/om3dvid7_reader.c $(DECODER) -o $@

$(BUILD)/test_player: tests/test_player.c src/om3dvid7_player.c $(DECODER) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(C89) $(INCLUDE) tests/test_player.c src/om3dvid7_player.c $(DECODER) -o $@

# The 3DO sound player, against a stand-in for Portfolio's audio folio.
$(BUILD)/test_sound: tests/test_sound.c src/om3dvid7_sound.c $(HEADERS) $(wildcard tests/audio_sdk/*.h)
	mkdir -p $(BUILD)
	$(CC) $(C89) -Itests/audio_sdk $(INCLUDE) tests/test_sound.c src/om3dvid7_sound.c -o $@

test: $(SHARED_LIBRARY) $(BUILD)/test_encoder $(BUILD)/test_reader $(BUILD)/test_player $(BUILD)/test_sound
	$(BUILD)/test_encoder $(TEST_CLIP)
	$(BUILD)/test_reader $(TEST_CLIP)
	$(BUILD)/test_player $(TEST_CLIP)
	$(BUILD)/test_sound
	$(PYTHON) tests/test_python.py $(BUILD) $(TEST_CLIP)

test-rust:
	cargo test --locked --manifest-path rust/Cargo.toml

clean:
	rm -rf $(BUILD)
