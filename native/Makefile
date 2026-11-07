BUILD_DIR := build

# One loader serves every supported title; the bundle targets pick the title
# with GAME=tfu (this branch default), pirates, clonewars or portal2.
GAME ?= tfu
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
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/portal2_osx
IMAGE_PREREQ :=
IMAGE_NAME := Portal2.image
LOADER_NAME := Portal2Compat
INFO_PLIST := Info-Portal2.plist
GAME_DATA :=
ICON := $(SOURCE_CONTENTS)/Resources/game.icns
ICON_NAME := game.icns
else ifeq ($(GAME),tfu)
BUNDLE := $(BUILD_DIR)/TFU-Compat.app
# Prefer the installed Steam Mac copy, while retaining the retail fallback.
SOURCE_APP := $(shell if test -d "$(HOME)/Library/Application Support/Steam/steamapps/common/Star Wars The Force Unleashed/Star Wars The Force Unleashed.app"; then echo "$(HOME)/Library/Application Support/Steam/steamapps/common/Star Wars The Force Unleashed/Star Wars The Force Unleashed.app"; else echo "/Volumes/Storage/games/mac/tfu/Star Wars The Force Unleashed/Star Wars The Force Unleashed.app"; fi)
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
ASSETS_DIR ?=
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/Star Wars The Force Unleashed
IMAGE_PREREQ := $(BUILD_DIR)/tfu_source
IMAGE_NAME := TFU.image
LOADER_NAME := TFUCompat
INFO_PLIST := Info-TFU.plist
GAME_DATA :=
ICON := $(SOURCE_CONTENTS)/Resources/AppIcon.icns
ICON_NAME := AppIcon.icns
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
$(error Unknown GAME "$(GAME)"; choose pirates, clonewars, portal2 or tfu)
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

$(BUILD_DIR)/test_cursor_state: tests/test_cursor_state.c src/cursor_state.c src/cursor_state.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_cursor_state.c src/cursor_state.c -o $@

test-cursor-state: $(BUILD_DIR)/test_cursor_state
	arch -x86_64 ./$(BUILD_DIR)/test_cursor_state

.PHONY: portal2-runtime
portal2-runtime:
	python3 tools/prepare_portal2_runtime.py --output "$(GUEST_RUNTIME_DIR)"

# The converter ships compiled code and downloads its own private runtime.
# Its users do not need Make, Python, Xcode, or a checkout of this repository.
CONVERTER_APP := $(BUILD_DIR)/Portal2-Converter.app
CONVERTER_SOURCES := converter/Conversion.swift converter/App.swift converter/main.swift
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

$(BUILD_DIR)/test_portal2_converter: converter/Conversion.swift tests/test_portal2_converter.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

test-portal2-converter: $(BUILD_DIR)/test_portal2_converter
	./$(BUILD_DIR)/test_portal2_converter

$(BUILD_DIR)/test_macho_file: tests/test_macho_file.c src/macho_file.c src/macho_file.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_macho_file.c src/macho_file.c -o $@

test-macho-file: $(BUILD_DIR)/test_macho_file
	./$(BUILD_DIR)/test_macho_file

.PHONY: test-large-image
test-large-image: $(BUILD_DIR)/game_loader
	python3 tests/test_large_image.py

test-guest-dyld: $(BUILD_DIR)/game_loader
	python3 tests/test_guest_dyld.py

test-portal2-loader: $(BUILD_DIR)/game_loader
	$(MAKE) GAME=portal2 test-portal2-loader-image

.PHONY: test-portal2-loader-image
test-portal2-loader-image: $(BUILD_DIR)/game_loader
	env LP32_DYLD_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(SOURCE_APP)/Contents/MacOS/portal2_osx"

.PHONY: test-tfu-loader test-tfu-loader-image test-tfu-platform test-tfu-platform-image tfu-runtime
tfu-runtime: portal2-runtime

test-tfu-loader: $(BUILD_DIR)/game_loader
	$(MAKE) GAME=tfu test-tfu-loader-image

test-tfu-loader-image: $(BUILD_DIR)/game_loader
	env LP32_HEAP_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
	env LP32_GUEST_RUNTIME_DIR="$(abspath $(GUEST_RUNTIME_DIR))" LP32_INITIALIZERS_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"

test-tfu-platform: $(BUILD_DIR)/game_loader
	$(MAKE) GAME=tfu test-tfu-platform-image

test-tfu-platform-image: $(BUILD_DIR)/game_loader
	env LP32_HEADLESS=1 LP32_TFU_TEXT_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
	env LP32_HEADLESS=1 LP32_NO_TFU_FAST_TEXT=1 LP32_TFU_TEXT_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
	env LP32_HEADLESS=1 LP32_SYNC_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
	env LP32_HEADLESS=1 LP32_AGL_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
	env LP32_HEADLESS=1 LP32_OPENAL_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"
	env LP32_HEADLESS=1 LP32_MOVIE_SELFTEST="$(ASSETS_DIR)/FMV/FMV-AttractMode.mov" arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"

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

$(BUILD_DIR)/game_loader: src/game_loader.c src/game_profile.c src/tfu_layout.c src/macho_loader.c src/macho_file.c src/guest_dyld.c src/guest_memory.c src/network_bridge.c src/font_bridge.m src/carbon_bridge.m src/carbon_ui.m src/carbon_native.m src/carbon_text.m src/carbon_files.m src/cf_format.c src/movie_bridge.m src/openal_bridge.m src/agl_bridge.m src/carbon_display.m src/xml_bridge.m src/gl_shader_bridge.c src/gl_misc_bridge.c src/gl_volume_bridge.c src/compat_runtime.c src/wide_format.c src/audio_bridge.c src/audio_queue_bridge.c src/controller_bridge.m src/tfu_controller.m src/tfu_input.c src/cursor_state.c src/objc_bridge.m src/arb_program_guard.c src/arb_sampler_usage.c src/mode32_switch.S src/import_gateway.S src/guest_context.S src/address_space_reserve.S src/tfu_timing.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $(filter-out %.h,$^) -framework Cocoa -framework Carbon -framework OpenGL -framework ImageIO -framework CoreText -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework GameController -framework IOKit -framework AVFoundation -framework CoreVideo -framework CoreMedia -framework OpenAL -lexpat -liconv -o $@
	codesign --force --sign - $@

$(BUILD_DIR)/test_arb_sampler_usage: tests/test_arb_sampler_usage.c src/arb_sampler_usage.c src/arb_sampler_usage.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_sampler_usage.c src/arb_sampler_usage.c -o $@

$(BUILD_DIR)/test_gl_shader_bridge: tests/test_gl_shader_bridge.c src/gl_shader_bridge.c src/gl_shader_bridge.h src/tfu_timing.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_gl_shader_bridge.c src/gl_shader_bridge.c -framework OpenGL -o $@

.PHONY: test-gl-shader-bridge
.PHONY: test-tfu-grip-alias
.PHONY: test-tfu-movie-end
test-tfu-movie-end:
	python3 tests/test_tfu_movie_end.py

$(BUILD_DIR)/game_loader: src/tfu_movie_end.h src/crash_trace.c src/crash_trace.h

test-tfu-grip-alias: $(BUILD_DIR)/game_loader
	env LP32_HEADLESS=1 LP32_TFU_GRIP_ALIAS_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUILD_DIR)/TFU-Compat.app/Contents/SharedSupport/TFU.image"

.PHONY: test-tfu-keyboard
test-tfu-keyboard: $(BUILD_DIR)/game_loader
	env LP32_TFU_KEYBOARD_SELFTEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUILD_DIR)/TFU-Compat.app/Contents/SharedSupport/TFU.image"

test-gl-shader-bridge: $(BUILD_DIR)/test_gl_shader_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_gl_shader_bridge

$(BUILD_DIR)/test_gl_volume_bridge: tests/test_gl_volume_bridge.c src/gl_volume_bridge.c src/gl_volume_bridge.h src/gl_misc_bridge.c src/gl_misc_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_gl_volume_bridge.c src/gl_volume_bridge.c src/gl_misc_bridge.c -framework OpenGL -o $@

.PHONY: test-gl-volume-bridge
test-gl-volume-bridge: $(BUILD_DIR)/test_gl_volume_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_gl_volume_bridge

$(BUILD_DIR)/test_gl_texture_provenance: tests/test_gl_texture_provenance.c src/gl_misc_bridge.c src/gl_misc_bridge.h src/gl_volume_bridge.c src/gl_volume_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc $(filter-out %.h,$^) -framework OpenGL -o $@

.PHONY: test-gl-texture-provenance
test-gl-texture-provenance: $(BUILD_DIR)/test_gl_texture_provenance
	arch -x86_64 ./$(BUILD_DIR)/test_gl_texture_provenance

$(BUILD_DIR)/tfu_cmd_tab_probe: tests/tfu_cmd_tab_probe.m | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -fobjc-arc -Wno-deprecated-declarations $< -framework Cocoa -framework Carbon -o $@

$(BUILD_DIR)/test_arb_program_guard: tests/test_arb_program_guard.c src/arb_program_guard.c src/arb_program_guard.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_program_guard.c src/arb_program_guard.c -o $@

test-arb-program-guard: $(BUILD_DIR)/test_arb_program_guard
	./$(BUILD_DIR)/test_arb_program_guard

test-arb-sampler-usage: $(BUILD_DIR)/test_arb_sampler_usage
	./$(BUILD_DIR)/test_arb_sampler_usage

$(BUILD_DIR)/mode_switch_stress: tests/mode_switch_stress.c tests/mode_switch_stress_asm.S src/mode32_switch.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 $^ -o $@
	codesign --force --sign - $@

# Reproduces Rosetta's wrong-mode landings after far jumps (guarded=0 crashes
# within seconds; guarded=1 must survive and report recoveries).
mode-switch-stress: $(BUILD_DIR)/mode_switch_stress
	arch -x86_64 ./$(BUILD_DIR)/mode_switch_stress guarded=1 threads=12 seconds=30 churn=1

run: $(BUILD_DIR)/mode32_probe
	arch -x86_64 ./$(BUILD_DIR)/mode32_probe

run-game: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ)
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"

# Recover the plain Pirates Mach-O by emulating the packer stub (needs the
# `unicorn` Python package, see the README).
$(BUILD_DIR)/venv/bin/python: | $(BUILD_DIR)
	python3 -m venv $(BUILD_DIR)/venv
	$(BUILD_DIR)/venv/bin/python -m pip install --quiet --disable-pip-version-check unicorn

$(BUILD_DIR)/LEGOPirates.unpacked.macbin: tools/unpack_securom.py $(UNPACK_PYTHON_PREREQ) | $(BUILD_DIR)
	test -f "$(PACKED_IMAGE)" || { echo "missing $(PACKED_IMAGE); pass SOURCE_APP=/path/to/LEGOPirates.app" >&2; exit 1; }
	$(UNPACK_PYTHON) tools/unpack_securom.py "$(PACKED_IMAGE)" "$@"

bundle: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ) $(INFO_PLIST)
ifeq ($(GAME),tfu)
	./$(BUILD_DIR)/tfu_source --validate "$(SOURCE_APP)" "$(ASSETS_DIR)"
	arch -x86_64 ./$(BUILD_DIR)/game_loader --validate-tfu "$(IMAGE_SOURCE)"
endif
	test -d "$(SOURCE_CONTENTS)" || { echo "missing $(SOURCE_APP); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE)" >&2; exit 1; }
ifneq ($(filter $(GAME),portal2 tfu),)
	test -f "$(GUEST_RUNTIME_DIR)/libstdc++.6.dylib" -a -f "$(GUEST_RUNTIME_DIR)/libc++abi.dylib" || { echo "missing i386 C++ runtime; run make $(GAME)-runtime or set GUEST_RUNTIME_DIR" >&2; exit 1; }
endif
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources $(BUNDLE)/Contents/SharedSupport $(BUNDLE)/Contents/Library/Frameworks
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	cp "$(IMAGE_SOURCE)" $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	chmod 0644 $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
ifeq ($(GAME),portal2)
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/MacOS" $(BUNDLE)/Contents/SharedSupport/Portal2
	ditto --noextattr --noqtn "$(GUEST_RUNTIME_DIR)" $(BUNDLE)/Contents/SharedSupport/Portal2/compat-runtime
else ifeq ($(GAME),tfu)
	./$(BUILD_DIR)/tfu_source --copy "$(SOURCE_APP)" "$(BUNDLE)/Contents/SharedSupport/TFU" "$(ASSETS_DIR)"
	ditto --noextattr --noqtn "$(GUEST_RUNTIME_DIR)" "$(BUNDLE)/Contents/SharedSupport/TFU/compat-runtime"
else
	for data in $(GAME_DATA); do cp "$(SOURCE_CONTENTS)/MacOS/$$data" $(BUNDLE)/Contents/MacOS/$$data; done
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/Library/Frameworks/Cg.framework" $(BUNDLE)/Contents/Library/Frameworks/Cg.framework
endif
	cp $(INFO_PLIST) $(BUNDLE)/Contents/Info.plist
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/Resources" $(BUNDLE)/Contents/Resources
	cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME)
ifeq ($(GAME),tfu)
	# Legacy Finder/resource-fork metadata cannot be sealed in the new app.
	# Strip it only from the generated copy, never from SOURCE_APP/ASSETS_DIR.
	xattr -cr "$(BUNDLE)"
endif
ifneq ($(filter $(GAME),portal2 tfu),)
	codesign --force --sign - $(BUNDLE)
else
	codesign --force --deep --sign - $(BUNDLE)
endif

	codesign --verify --deep --strict "$(BUNDLE)"

# Update only the compatibility runtime when the external source-data volume
# is unavailable. The existing bundle's game data and resources are retained.
promote-loader: $(BUILD_DIR)/game_loader
	test -d $(BUNDLE)/Contents/MacOS
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new
	mv $(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	if test -f "$(ICON)"; then cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME); fi
ifneq ($(filter $(GAME),portal2 tfu),)
	codesign --force --sign - $(BUNDLE)
else
	codesign --force --deep --sign - $(BUNDLE)
endif
	codesign --verify --deep --strict $(BUNDLE)

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

# TFU adaptation of the converter from portal2 a06456f. The Swift discovery
# and copy implementation is also used by `make GAME=tfu bundle`.
TFU_CONVERTER_APP := $(BUILD_DIR)/TFU-Converter.app
TFU_CONVERTER_CORE := tfu-converter/Conversion.swift tfu-converter/Source.swift
TFU_CONVERTER_SOURCES := $(TFU_CONVERTER_CORE) tfu-converter/App.swift tfu-converter/main.swift

$(BUILD_DIR)/tfu-converter-arm64: $(TFU_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target arm64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/tfu-converter-x86_64: $(TFU_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target x86_64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/tfu_source: $(TFU_CONVERTER_CORE) tools/tfu_source.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: tfu-converter
tfu-converter: $(BUILD_DIR)/tfu-converter-arm64 $(BUILD_DIR)/tfu-converter-x86_64 $(BUILD_DIR)/game_loader tfu-converter/Info.plist Info-TFU.plist
	mkdir -p "$(TFU_CONVERTER_APP)/Contents/MacOS" "$(TFU_CONVERTER_APP)/Contents/Resources"
	lipo -create $(BUILD_DIR)/tfu-converter-arm64 $(BUILD_DIR)/tfu-converter-x86_64 -output "$(TFU_CONVERTER_APP)/Contents/MacOS/TFUConverter"
	cp tfu-converter/Info.plist "$(TFU_CONVERTER_APP)/Contents/Info.plist"
	cp $(BUILD_DIR)/game_loader Info-TFU.plist "$(TFU_CONVERTER_APP)/Contents/Resources/"
	codesign --force --sign - "$(TFU_CONVERTER_APP)/Contents/Resources/game_loader"
	codesign --force --sign - "$(TFU_CONVERTER_APP)"
	codesign --verify --deep --strict "$(TFU_CONVERTER_APP)"

$(BUILD_DIR)/test_tfu_converter: $(TFU_CONVERTER_CORE) tests/test_tfu_converter.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: test-tfu-converter test-tfu-layout
test-tfu-converter: $(BUILD_DIR)/test_tfu_converter
	./$(BUILD_DIR)/test_tfu_converter

test-tfu-layout:
	python3 tests/test_tfu_layout.py

$(BUILD_DIR)/game_loader: src/tfu_layout.h src/game_profile.h src/tfu_movie_end.h src/objc_bridge.h src/carbon_native.h src/carbon_bridge.h src/compat_runtime.h
$(BUILD_DIR)/game_loader: src/carbon_dialog.m
$(BUILD_DIR)/game_loader: src/tfu_steam.m src/tfu_steam.h

$(BUILD_DIR)/game_loader: src/app_termination.m src/app_termination.h

$(BUILD_DIR)/test_app_termination: tests/test_app_termination.m src/app_termination.m src/app_termination.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc $(filter-out %.h,$^) -framework Cocoa -framework Carbon -o $@

.PHONY: test-app-termination
test-app-termination: $(BUILD_DIR)/test_app_termination
	./$(BUILD_DIR)/test_app_termination

$(BUILD_DIR)/test_tfu_steam: tests/test_tfu_steam.m src/tfu_steam.m src/tfu_steam.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc $(filter-out %.h,$^) -framework Cocoa -o $@

.PHONY: test-tfu-steam
test-tfu-steam: $(BUILD_DIR)/test_tfu_steam
	./$(BUILD_DIR)/test_tfu_steam missing
	./$(BUILD_DIR)/test_tfu_steam retail
	./$(BUILD_DIR)/test_tfu_steam headless
	./$(BUILD_DIR)/test_tfu_steam background
