BUILD_DIR := build

# One loader serves every supported title; the bundle targets pick the title
# with GAME=pirates (default), GAME=clonewars, GAME=portal2 or GAME=l4d2.
GAME ?= pirates
GUEST_RUNTIME_DIR ?= $(BUILD_DIR)/guest-runtime
ifeq ($(GAME),clonewars)
BUNDLE := $(BUILD_DIR)/LEGOCloneWars-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/lsw3/LEGOCloneWars.app
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
# Plain i386 Mach-O (no SecuROM wrapper), so the shipped binary is the image.
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/LEGOCloneWars.macbin
# The shipped path may contain spaces, which make cannot take as a
# prerequisite; the recipes test for it instead.
IMAGE_PREREQ :=
IMAGE_NAME := LEGOCloneWars.image
LOADER_NAME := LEGOCloneWarsCompat
INFO_PLIST := Info-CloneWars.plist
# Clone Wars keeps GAME*.DAT in Resources; they arrive with the Resources ditto.
GAME_DATA :=
ICON := icons/LEGOCloneWars.icns
ICON_NAME := LEGOStarWars3.icns
else ifeq ($(GAME),portal2)
BUNDLE := $(BUILD_DIR)/Portal2-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/Portal 2.app
# SOURCE_GAME accepts a Mac app, merged Steam installation, or depot download.
SOURCE_GAME ?= $(SOURCE_APP)
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/portal2_osx
IMAGE_PREREQ :=
IMAGE_NAME := Portal2.image
LOADER_NAME := Portal2Compat
INFO_PLIST := Info-Portal2.plist
GAME_DATA :=
ICON := $(SOURCE_CONTENTS)/Resources/game.icns
ICON_NAME := game.icns
SOURCE_TOOL := $(BUILD_DIR)/portal2_source
SOURCE_DATA_DIRECTORY := Portal2
else ifeq ($(GAME),l4d2)
BUNDLE := $(BUILD_DIR)/L4D2-Compat.app
# SOURCE_APP remains an alias when it contains a complete Mac game tree.
SOURCE_GAME ?= $(if $(strip $(SOURCE_APP)),$(SOURCE_APP),$(HOME)/Library/Application Support/Steam/steamapps/common/Left 4 Dead 2)
IMAGE_PREREQ :=
IMAGE_NAME := L4D2.image
LOADER_NAME := L4D2Compat
INFO_PLIST := Info-L4D2.plist
GAME_DATA :=
ICON := $(SOURCE_GAME)/left4dead2/resource/game.icns
ICON_NAME := game.icns
SOURCE_TOOL := $(BUILD_DIR)/l4d2_source
SOURCE_DATA_DIRECTORY := L4D2
else ifeq ($(GAME),pirates)
BUNDLE := $(BUILD_DIR)/LEGOPirates-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/LEGOPirates.app
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
# The shipped LEGOPirates.macbin is a SecuROM-packed stub; tools/unpack_securom.py
# recovers the plain Mach-O from it (see the rule below).  Pass IMAGE_SOURCE=
# to use an already unpacked image instead.
PACKED_IMAGE := $(SOURCE_CONTENTS)/MacOS/LEGOPirates.macbin
IMAGE_SOURCE := $(BUILD_DIR)/LEGOPirates.unpacked.macbin
IMAGE_PREREQ := $(IMAGE_SOURCE)
IMAGE_NAME := LEGOPirates.image
LOADER_NAME := LEGOPiratesCompat
INFO_PLIST := Info.plist
GAME_DATA := GAME.DAT GAME0.DAT GAME1.DAT GAME2.DAT GAME3.DAT GAME4.DAT
ICON := icons/LEGOPirates.icns
ICON_NAME := icon.icns
else
$(error Unknown GAME "$(GAME)"; choose pirates, clonewars, portal2 or l4d2)
endif
# Python for the Pirates unpacker.  By default a venv under $(BUILD_DIR) with
# `unicorn` installed is created on first use (system and Homebrew Pythons
# refuse plain `pip install`s); PYTHON=/path/to/python3 names an interpreter
# that already has unicorn instead.
PYTHON ?=
ifeq ($(PYTHON),)
UNPACK_PYTHON := $(BUILD_DIR)/venv/bin/python
UNPACK_PYTHON_PREREQ := $(UNPACK_PYTHON)
else
UNPACK_PYTHON := $(PYTHON)
UNPACK_PYTHON_PREREQ :=
endif
CLANG := $(shell xcrun --find clang)
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
CFLAGS := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g -Wall -Wextra -Werror
LDFLAGS := -Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000,-segaddr,__LEGACY,0x1000,-segprot,__LEGACY,rwx,rwx

.PHONY: all bundle promote-loader icons clean run run-game controller-probe test-controller test-arb-program-guard test-arb-sampler-usage mode-switch-stress

all: $(BUILD_DIR)/mode32_probe $(BUILD_DIR)/game_loader

.PHONY: test-macho-file test-guest-dyld test-portal2-loader test-cursor-state
.PHONY: test-steam-abi test-steam-filename test-steam-startup test-source-steam-startup test-portal2-coop

test-source-steam-startup: $(BUILD_DIR)/game_loader
	python3 tests/test_source_steam_startup.py --loader $(BUILD_DIR)/game_loader

test-portal2-coop: $(BUILD_DIR)/game_loader
	python3 tests/test_portal2_coop.py

test-steam-abi: $(BUILD_DIR)/game_loader
	python3 tests/test_steam_abi.py
	python3 tests/test_steam_records.py
	python3 tests/test_steam_filename.py

test-steam-filename: $(BUILD_DIR)/game_loader
	python3 tests/test_steam_filename.py

$(BUILD_DIR)/test_steam_startup: tests/test_steam_startup.c src/steam_startup.c src/steam_startup.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_steam_startup.c src/steam_startup.c -o $@

test-steam-startup: $(BUILD_DIR)/test_steam_startup
	arch -x86_64 ./$(BUILD_DIR)/test_steam_startup

$(BUILD_DIR)/test_cursor_state: tests/test_cursor_state.c src/cursor_state.c src/cursor_state.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_cursor_state.c src/cursor_state.c -o $@

test-cursor-state: $(BUILD_DIR)/test_cursor_state
	arch -x86_64 ./$(BUILD_DIR)/test_cursor_state

.PHONY: test-hi-mouse
test-hi-mouse: $(BUILD_DIR)/game_loader
	python3 tests/test_hi_mouse.py --loader $(BUILD_DIR)/game_loader

.PHONY: portal2-runtime
portal2-runtime:
	python3 tools/prepare_portal2_runtime.py --output "$(GUEST_RUNTIME_DIR)"

# The converter ships compiled code and downloads its own private runtime.
# Its users do not need Make, Python, Xcode, or a checkout of this repository.
CONVERTER_APP := $(BUILD_DIR)/Portal2-Converter.app
CONVERTER_CORE := converter/Conversion.swift converter/Source.swift
CONVERTER_SOURCES := $(CONVERTER_CORE) converter/App.swift converter/main.swift
SWIFTC := $(shell xcrun --find swiftc)

$(BUILD_DIR)/converter-arm64: $(CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target arm64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/converter-x86_64: $(CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target x86_64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: portal2-converter test-portal2-converter
portal2-converter: $(BUILD_DIR)/converter-arm64 $(BUILD_DIR)/converter-x86_64 $(BUILD_DIR)/game_loader converter/Info.plist Info-Portal2.plist
	mkdir -p "$(CONVERTER_APP)/Contents/MacOS" "$(CONVERTER_APP)/Contents/Resources"
	lipo -create $(BUILD_DIR)/converter-arm64 $(BUILD_DIR)/converter-x86_64 -output "$(CONVERTER_APP)/Contents/MacOS/Portal2Converter"
	cp converter/Info.plist "$(CONVERTER_APP)/Contents/Info.plist"
	cp $(BUILD_DIR)/game_loader Info-Portal2.plist "$(CONVERTER_APP)/Contents/Resources/"
	codesign --force --sign - "$(CONVERTER_APP)/Contents/Resources/game_loader"
	codesign --force --sign - "$(CONVERTER_APP)"
	codesign --verify --deep --strict "$(CONVERTER_APP)"

$(BUILD_DIR)/test_portal2_converter: $(CONVERTER_CORE) tests/test_portal2_converter.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

test-portal2-converter: $(BUILD_DIR)/test_portal2_converter
	./$(BUILD_DIR)/test_portal2_converter

$(BUILD_DIR)/test_macho_file: tests/test_macho_file.c src/macho_file.c src/macho_file.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_macho_file.c src/macho_file.c -o $@

test-macho-file: $(BUILD_DIR)/test_macho_file
	./$(BUILD_DIR)/test_macho_file

.PHONY: test-game-profile
$(BUILD_DIR)/test_game_profile: tests/test_game_profile.c src/game_profile.c src/game_profile.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_game_profile.c src/game_profile.c -o $@

test-game-profile: $(BUILD_DIR)/test_game_profile
	arch -x86_64 ./$(BUILD_DIR)/test_game_profile

test-guest-dyld: $(BUILD_DIR)/game_loader test-steam-abi test-steam-startup test-crash-trace test-portal2-coop
	python3 tests/test_guest_dyld.py

test-portal2-loader: $(BUILD_DIR)/game_loader
	$(MAKE) GAME=portal2 test-portal2-loader-image

L4D2_SOURCE_CORE := $(CONVERTER_CORE) converter/L4D2Source.swift
L4D2_EXECUTABLE ?= hl2_osx

$(BUILD_DIR)/l4d2_source: $(L4D2_SOURCE_CORE) tools/l4d2_source.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/test_l4d2_source: $(L4D2_SOURCE_CORE) tests/test_l4d2_source.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: test-l4d2-source test-l4d2-loader test-l4d2-loader-image test-l4d2-runtime
test-l4d2-source: $(BUILD_DIR)/test_l4d2_source $(BUILD_DIR)/l4d2_source
	./$(BUILD_DIR)/test_l4d2_source

test-l4d2-loader: $(BUILD_DIR)/game_loader
	$(MAKE) GAME=l4d2 test-l4d2-loader-image

test-l4d2-loader-image: bundle
	env LP32_GAME=l4d2 LP32_DYLD_SELFTEST=1 arch -x86_64 "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME)" "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

test-l4d2-runtime: $(BUILD_DIR)/game_loader
	test -n "$(L4D2_EXECUTABLE)" || { echo "pass L4D2_EXECUTABLE=the-audited-launcher-name" >&2; exit 1; }
	env LP32_TEST_GAME=l4d2 LP32_TEST_EXECUTABLE="$(L4D2_EXECUTABLE)" python3 tests/test_guest_dyld.py

$(BUILD_DIR)/portal2_source: $(CONVERTER_CORE) tools/portal2_source.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: test-portal2-loader-image
test-portal2-loader-image: bundle
	env LP32_DYLD_SELFTEST=1 arch -x86_64 "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME)" "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

$(BUILD_DIR)/controller_probe: src/controller_probe.m | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $^ -framework Foundation -framework GameController -o $@
	codesign --force --sign - $@

controller-probe: $(BUILD_DIR)/controller_probe
	arch -x86_64 ./$(BUILD_DIR)/controller_probe

# Use the existing bundle's image without launching a window or loading saves.
CONTROLLER_TEST_IMAGE ?= $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
test-controller: $(BUILD_DIR)/game_loader
	test -f "$(CONTROLLER_TEST_IMAGE)"
	env LP32_CONTROLLER_SELFTEST=1 LP32_VIRTUAL_CONTROLLERS=2 LP32_VIRTUAL_CONTROLLER_TEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(CONTROLLER_TEST_IMAGE)"

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/mode32_probe: src/mode32_probe.c src/mode32_switch.S src/address_space_reserve.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $^ -o $@
	codesign --force --sign - $@

.PHONY: test-crash-trace
$(BUILD_DIR)/test_crash_trace: tests/test_crash_trace.c src/crash_trace.c src/crash_trace.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_crash_trace.c src/crash_trace.c -o $@

test-crash-trace: $(BUILD_DIR)/test_crash_trace
	arch -x86_64 ./$(BUILD_DIR)/test_crash_trace

$(BUILD_DIR)/game_loader: src/game_loader.c src/crash_trace.c src/crash_trace.h src/physics_trace.c src/physics_trace.h src/game_profile.c src/macho_loader.c src/macho_file.c src/guest_dyld.c src/steam_bridge.c src/steam_startup.c src/steam_startup.h src/objc_bridge.h src/zlib_bridge.c src/zlib_bridge.h src/steam_bridge.h src/steam_abi.inc src/guest_memory.c src/network_bridge.c src/font_bridge.m src/gl_shader_bridge.c src/gl_misc_bridge.c src/compat_runtime.c src/wide_format.c src/audio_bridge.c src/audio_queue_bridge.c src/openal_capture_bridge.c src/openal_capture_bridge.h src/sound_manager_bridge.c src/sound_manager_bridge.h src/time_manager_bridge.c src/time_manager_bridge.h src/controller_bridge.m src/cursor_state.c src/objc_bridge.m src/arb_program_guard.c src/arb_sampler_usage.c src/mode32_switch.S src/import_gateway.S src/guest_context.S src/address_space_reserve.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $(filter %.c %.m %.S,$^) -framework Cocoa -framework Carbon -framework OpenGL -framework ImageIO -framework CoreText -framework CFNetwork -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework OpenAL -framework GameController -framework IOKit -framework AVFoundation -framework CoreMedia -framework CoreVideo -liconv -lffi -lz -o $@
	codesign --force --sign - $@

$(BUILD_DIR)/game_loader: src/game_profile.h
$(BUILD_DIR)/game_loader: src/audio_converter_bridge.c src/audio_converter_bridge.h
$(BUILD_DIR)/game_loader: src/quicktime_bridge.m src/quicktime_bridge.h src/quicktime_support.c src/quicktime_support.h

.PHONY: test-quicktime-support test-quicktime-bridge
$(BUILD_DIR)/test_quicktime_support: tests/test_quicktime_support.c src/quicktime_support.c src/quicktime_support.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_quicktime_support.c src/quicktime_support.c -pthread -Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000 -o $@

test-quicktime-support: $(BUILD_DIR)/test_quicktime_support
	arch -x86_64 ./$(BUILD_DIR)/test_quicktime_support

$(BUILD_DIR)/test_quicktime_bridge: tests/test_quicktime_bridge.m src/quicktime_bridge.m src/quicktime_support.c src/quicktime_bridge.h src/quicktime_support.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wno-deprecated-declarations -Isrc $(filter %.c %.m,$^) -framework Foundation -framework AVFoundation -framework CoreVideo -framework CoreMedia -framework AudioToolbox -framework CoreGraphics -Wl,-no_pie,-pagezero_size,0x1000 -o $@

test-quicktime-bridge: $(BUILD_DIR)/test_quicktime_bridge test-quicktime-support
	arch -x86_64 ./$(BUILD_DIR)/test_quicktime_bridge

.PHONY: test-audio-converter
$(BUILD_DIR)/test_audio_converter_bridge: tests/test_audio_converter_bridge.c src/audio_converter_bridge.c src/audio_converter_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_audio_converter_bridge.c src/audio_converter_bridge.c -framework AudioToolbox -pthread -Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000 -o $@

test-audio-converter: $(BUILD_DIR)/test_audio_converter_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_audio_converter_bridge

$(BUILD_DIR)/test_arb_sampler_usage: tests/test_arb_sampler_usage.c src/arb_sampler_usage.c src/arb_sampler_usage.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_sampler_usage.c src/arb_sampler_usage.c -o $@

$(BUILD_DIR)/test_gl_shader_bridge: tests/test_gl_shader_bridge.c src/gl_shader_bridge.c src/gl_shader_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_gl_shader_bridge.c src/gl_shader_bridge.c -framework OpenGL -o $@

.PHONY: test-gl-shader-bridge
test-gl-shader-bridge: $(BUILD_DIR)/test_gl_shader_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_gl_shader_bridge

$(BUILD_DIR)/test_arb_program_guard: tests/test_arb_program_guard.c src/arb_program_guard.c src/arb_program_guard.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_program_guard.c src/arb_program_guard.c -o $@

test-arb-program-guard: $(BUILD_DIR)/test_arb_program_guard
	./$(BUILD_DIR)/test_arb_program_guard

test-arb-sampler-usage: $(BUILD_DIR)/test_arb_sampler_usage
	./$(BUILD_DIR)/test_arb_sampler_usage

$(BUILD_DIR)/mode_switch_stress: tests/mode_switch_stress.c tests/mode_switch_stress_asm.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 $^ -o $@
	codesign --force --sign - $@

# Reproduces Rosetta's wrong-mode landings after far jumps (guarded=0 crashes
# within seconds; guarded=1 must survive and report recoveries).
mode-switch-stress: $(BUILD_DIR)/mode_switch_stress
	arch -x86_64 ./$(BUILD_DIR)/mode_switch_stress guarded=1 threads=12 seconds=30 churn=1

run: $(BUILD_DIR)/mode32_probe
	arch -x86_64 ./$(BUILD_DIR)/mode32_probe

ifneq ($(filter $(GAME),portal2 l4d2),)
run-game: bundle
	arch -x86_64 "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME)"
else
run-game: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ)
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
endif

# Recover the plain Pirates Mach-O by emulating the packer stub (needs the
# `unicorn` Python package, see the README).
$(BUILD_DIR)/venv/bin/python: | $(BUILD_DIR)
	python3 -m venv $(BUILD_DIR)/venv
	$(BUILD_DIR)/venv/bin/python -m pip install --quiet --disable-pip-version-check unicorn

$(BUILD_DIR)/LEGOPirates.unpacked.macbin: tools/unpack_securom.py $(UNPACK_PYTHON_PREREQ) | $(BUILD_DIR)
	test -f "$(PACKED_IMAGE)" || { echo "missing $(PACKED_IMAGE); pass SOURCE_APP=/path/to/LEGOPirates.app" >&2; exit 1; }
	$(UNPACK_PYTHON) tools/unpack_securom.py "$(PACKED_IMAGE)" "$@"

ifneq ($(filter $(GAME),portal2 l4d2),)
bundle: $(BUILD_DIR)/game_loader $(SOURCE_TOOL) $(INFO_PLIST) portal2-runtime
	./$(SOURCE_TOOL) --copy "$(SOURCE_GAME)" "$(BUNDLE)"
ifeq ($(GAME),l4d2)
	./$(SOURCE_TOOL) --check-output "$(BUNDLE)"
endif
	mkdir -p "$(BUNDLE)/Contents/MacOS"
	cp $(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME)"
	ditto --noextattr --noqtn "$(GUEST_RUNTIME_DIR)" "$(BUNDLE)/Contents/SharedSupport/$(SOURCE_DATA_DIRECTORY)/compat-runtime"
	cp $(INFO_PLIST) "$(BUNDLE)/Contents/Info.plist"
	codesign --force --sign - "$(BUNDLE)"
	codesign --verify --deep --strict "$(BUNDLE)"
else
bundle: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ) $(INFO_PLIST)
	test -d "$(SOURCE_CONTENTS)" || { echo "missing $(SOURCE_APP); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE)" >&2; exit 1; }
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources $(BUNDLE)/Contents/SharedSupport $(BUNDLE)/Contents/Library/Frameworks
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	cp "$(IMAGE_SOURCE)" $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	chmod 0644 $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	for data in $(GAME_DATA); do cp "$(SOURCE_CONTENTS)/MacOS/$$data" $(BUNDLE)/Contents/MacOS/$$data; done
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/Library/Frameworks/Cg.framework" $(BUNDLE)/Contents/Library/Frameworks/Cg.framework
	cp $(INFO_PLIST) $(BUNDLE)/Contents/Info.plist
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/Resources" $(BUNDLE)/Contents/Resources
	cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME)
	codesign --force --deep --sign - $(BUNDLE)
endif

# Update only the compatibility runtime when the external source-data volume
# is unavailable. The existing bundle's game data and resources are retained.
ifeq ($(GAME),l4d2)
promote-loader: $(BUILD_DIR)/l4d2_source
endif
promote-loader: $(BUILD_DIR)/game_loader
ifeq ($(GAME),l4d2)
	./$(SOURCE_TOOL) --check-output "$(BUNDLE)"
endif
	test -d "$(BUNDLE)/Contents/MacOS"
	cp $(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new"
	mv "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new" "$(BUNDLE)/Contents/MacOS/$(LOADER_NAME)"
	if test -f "$(ICON)"; then cp "$(ICON)" "$(BUNDLE)/Contents/Resources/$(ICON_NAME)"; fi
ifneq ($(filter $(GAME),portal2 l4d2),)
	codesign --force --sign - "$(BUNDLE)"
else
	codesign --force --deep --sign - "$(BUNDLE)"
endif
	codesign --verify --deep --strict "$(BUNDLE)"

# Regenerate the Dock icons from the artwork in icons/ (the shipped icons are
# cut-outs, which macOS 26 draws on a white tile).
icons: $(BUILD_DIR)/mkicon
	./$(BUILD_DIR)/mkicon icons/LEGOPirates-art.png icons/LEGOPirates.icns
	./$(BUILD_DIR)/mkicon icons/LEGOCloneWars-art.png icons/LEGOCloneWars.icns 14,16,22

$(BUILD_DIR)/mkicon: icons/mkicon.swift | $(BUILD_DIR)
	swiftc -O $< -o $@

clean:
	rm -f $(BUILD_DIR)/mode32_probe $(BUILD_DIR)/game_loader $(BUILD_DIR)/controller_probe $(BUILD_DIR)/mkicon
	rm -f $(BUILD_DIR)/converter-arm64 $(BUILD_DIR)/converter-x86_64 $(BUILD_DIR)/test_portal2_converter
	rm -f $(BUILD_DIR)/l4d2_source $(BUILD_DIR)/test_l4d2_source $(BUILD_DIR)/test_game_profile
