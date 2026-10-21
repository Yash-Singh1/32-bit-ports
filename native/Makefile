BUILD_DIR := build
-include local.mk
CONTINUE_WHEN_INACTIVE ?= 0

# One loader serves every supported title; the bundle targets pick the title
# with GAME=pirates (default), clonewars, marvel, saga, cod2, cod2mp, cod4, cod4mp,
# mw2, mw2mp, mw3, mw3mp, gangstar or asphalt6.
GAME ?= pirates
ifneq ($(filter $(GAME),cod cod2 cod2mp cod4 cod4mp),)
# Aspyr's Carbon-era ports share tools/bundle_cod4.py.
COD_GAME := $(if $(filter cod2 cod2mp,$(GAME)),cod2,cod4)
COD_STEM := $(if $(filter cod2,$(COD_GAME)),COD2,COD4)
COD4_MODE := $(if $(filter cod2mp cod4mp,$(GAME)),mp,sp)
COD4_SUFFIX := $(if $(filter mp,$(COD4_MODE)),MP,)
SOURCE_APP ?=
BUNDLE := $(BUILD_DIR)/$(COD_STEM)$(COD4_SUFFIX)-Compat.app
IMAGE_NAME := $(COD_STEM)$(COD4_SUFFIX).image
LOADER_NAME := $(COD_STEM)$(COD4_SUFFIX)Compat
CG_SOURCE :=
ICON_NAME := $(if $(filter mp,$(COD4_MODE)),Game_mp.icns,Game.icns)
GUEST_RUNTIME_DIR ?= $(BUILD_DIR)/guest-runtime
else ifneq ($(filter $(GAME),mw2 mw2mp mw3 mw3mp),)
MW2_GAME := $(if $(filter mw3 mw3mp,$(GAME)),mw3,mw2)
MW2_MODE := $(if $(filter mw2mp mw3mp,$(GAME)),mp,sp)
MW2_STEM := $(if $(filter mw3 mw3mp,$(GAME)),MW3,MW2)$(if $(filter mp,$(MW2_MODE)),MP,)
SOURCE_APP ?=
BUNDLE := $(BUILD_DIR)/$(MW2_STEM)-Compat.app
IMAGE_NAME := $(MW2_STEM).image
LOADER_NAME := $(MW2_STEM)Compat
CG_SOURCE :=
ICON_NAME := AppIcon.icns
else ifeq ($(GAME),gangstar)
# Gameloft's Gangstar: Miami Vindication. SOURCE_APP may be GangstarMV.app,
# its folder or the release's .dmg (mounted read-only while copying).
SOURCE_APP ?=
BUNDLE := $(BUILD_DIR)/Gangstar-Compat.app
IMAGE_NAME := Gangstar.image
LOADER_NAME := GangstarCompat
CG_SOURCE :=
ICON_NAME := icon.icns
GUEST_RUNTIME_DIR ?= $(BUILD_DIR)/guest-runtime
# GANGSTAR_GL=apple builds an app on Apple's OpenGL instead of GLMetal.
GANGSTAR_GL ?= metal
else ifeq ($(GAME),asphalt6)
SOURCE_APP ?=
BUNDLE := $(BUILD_DIR)/Asphalt6-Compat.app
IMAGE_NAME := Asphalt6.image
LOADER_NAME := Asphalt6Compat
CG_SOURCE :=
ICON_NAME := IconMacOSX.icns
GUEST_RUNTIME_DIR ?= $(BUILD_DIR)/guest-runtime
ASPHALT6_GL ?= apple
else ifeq ($(GAME),clonewars)
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
else ifeq ($(GAME),marvel)
BUNDLE := $(BUILD_DIR)/LEGOMarvel-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/marvel/LEGO Marvel Super Heroes.app
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/LEGOMarvel.macbin
IMAGE_PREREQ :=
IMAGE_NAME := LEGOMarvel.image
LOADER_NAME := LEGOMarvelCompat
INFO_PLIST := Info-Marvel.plist
GAME_DATA := steam_appid.txt
CG_SOURCE := $(SOURCE_CONTENTS)/Frameworks/Cg.framework
ICON := $(SOURCE_CONTENTS)/Resources/GameIcon.icns
ICON_NAME := GameIcon.icns
else ifeq ($(GAME),saga)
SAGA_EDITION ?= steam
ifeq ($(SAGA_EDITION),steam)
BUNDLE := $(BUILD_DIR)/LEGOCompleteSaga-Steam-Compat.app
# The Steam library location must be supplied by the caller.
SOURCE_APP ?=
ifneq ($(filter bundle run-game,$(MAKECMDGOALS)),)
ifeq ($(strip $(SOURCE_APP)),)
$(error Steam Saga requires SOURCE_APP="/path/to/LEGO Star Wars Saga.app")
endif
endif
SAGA_DATA_SOURCE ?= $(SOURCE_APP)/../LEGOStarWarsSagaData
IMAGE_PREREQ :=
ICON := $(SOURCE_APP)/Contents/Resources/GameIcon.icns
ICON_NAME := GameIcon.icns
else ifeq ($(SAGA_EDITION),retail)
BUNDLE := $(BUILD_DIR)/LEGOCompleteSaga-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/lswc/LEGO Star Wars Saga.app
SAGA_UPDATE_ZIP ?= /Volumes/Storage/games/mac/lswc/legostarwarssaga_111_update.zip
ifneq ($(strip $(SAGA_UPDATE_ZIP)),)
# Stage the publisher's update without changing the user's original copy.
# SOURCE_APP on the command line still selects an already updated app.
SOURCE_APP := $(abspath $(BUILD_DIR)/saga-update)/LEGO Star Wars Saga.app
IMAGE_PREREQ := saga-update
else
IMAGE_PREREQ :=
endif
SAGA_DATA_SOURCE ?= $(abspath $(BUILD_DIR))/Data
RESOURCE_FORKS := 1
ICON := $(SOURCE_APP)/Contents/Resources/Saga.icns
ICON_NAME := Saga.icns
else
$(error Unknown SAGA_EDITION "$(SAGA_EDITION)"; choose steam or retail)
endif
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/LEGO Star Wars Saga
IMAGE_NAME := LEGOCompleteSaga.image
LOADER_NAME := LEGOCompleteSagaCompat
INFO_PLIST := Info-Saga.plist
GAME_DATA :=
CG_SOURCE :=
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
$(error Unknown GAME "$(GAME)"; choose pirates, clonewars, marvel, saga, cod2, cod2mp, cod4, cod4mp, mw2, mw2mp, mw3, mw3mp, gangstar or asphalt6)
endif
CG_SOURCE ?= $(SOURCE_CONTENTS)/Library/Frameworks/Cg.framework
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

include lp32gl/lp32gl.mk

# Standalone converter, adapted from tfu/portal2. Users need no developer tools.
COD4_CONVERTER_APP := $(BUILD_DIR)/COD4-Converter.app
COD4_CONVERTER_CORE := cod4-converter/Conversion.swift cod4-converter/Source.swift cod4-converter/ModernWarfare.swift
COD4_CONVERTER_SOURCES := $(COD4_CONVERTER_CORE) cod4-converter/App.swift cod4-converter/main.swift
SWIFTC := $(shell xcrun --find swiftc)

$(BUILD_DIR)/cod4-converter-arm64: $(COD4_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target arm64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/cod4-converter-x86_64: $(COD4_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target x86_64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

# Always ask GLMetal's own dependency graph to refresh its driver/helper pair.
GLMETAL_SOURCE ?= glmetal
GLMETAL_BUILD ?= $(GLMETAL_SOURCE)/build
.PHONY: glmetal-lib cod-converters cod2-converter cod4-converter mw2-converter mw3-converter test-cod4-converter gangstar-converter
glmetal-lib:
	@if test "$(GLMETAL_SOURCE)" = glmetal && ! test -f glmetal/Makefile; then \
		git -C .. submodule update --init --recursive -- native/glmetal; \
	fi
	$(MAKE) -C "$(GLMETAL_SOURCE)" lib

cod-converters: cod2-converter cod4-converter mw2-converter mw3-converter
cod4-converter mw2-converter mw3-converter: glmetal-lib
cod2-converter cod4-converter mw2-converter mw3-converter: $(BUILD_DIR)/cod4-converter-arm64 $(BUILD_DIR)/cod4-converter-x86_64 $(BUILD_DIR)/game_loader cod4-converter/Info.plist
	python3 tools/package_converter.py --game "$(@:-converter=)" --build "$(BUILD_DIR)" --glmetal "$(GLMETAL_BUILD)"

# Gangstar's converter reuses the COD converter's runtime download, GLMetal
# validation and copy helpers (COD4_CONVERTER_CORE).
GANGSTAR_CONVERTER_SOURCES := $(COD4_CONVERTER_CORE) gangstar-converter/Gangstar.swift gangstar-converter/App.swift gangstar-converter/main.swift

$(BUILD_DIR)/gangstar-converter-arm64: $(GANGSTAR_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target arm64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/gangstar-converter-x86_64: $(GANGSTAR_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target x86_64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

gangstar-converter: glmetal-lib $(BUILD_DIR)/gangstar-converter-arm64 $(BUILD_DIR)/gangstar-converter-x86_64 $(BUILD_DIR)/game_loader gangstar-converter/Info.plist tools/package_gangstar_converter.py
	python3 tools/package_gangstar_converter.py --build "$(BUILD_DIR)" --glmetal "$(GLMETAL_BUILD)"

# Asphalt 6 has its own source validation, ZIP input and converter app.
ASPHALT6_CONVERTER_SOURCES := $(COD4_CONVERTER_CORE) asphalt6-converter/Asphalt6.swift asphalt6-converter/App.swift asphalt6-converter/main.swift

$(BUILD_DIR)/asphalt6-converter-arm64: $(ASPHALT6_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target arm64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

$(BUILD_DIR)/asphalt6-converter-x86_64: $(ASPHALT6_CONVERTER_SOURCES) | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target x86_64-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: asphalt6-converter
asphalt6-converter: glmetal-lib $(BUILD_DIR)/asphalt6-converter-arm64 $(BUILD_DIR)/asphalt6-converter-x86_64 $(BUILD_DIR)/game_loader asphalt6-converter/Info.plist tools/package_asphalt6_converter.py
	python3 tools/package_asphalt6_converter.py --build "$(BUILD_DIR)" --glmetal "$(GLMETAL_BUILD)"

$(BUILD_DIR)/test_asphalt6_converter: $(COD4_CONVERTER_CORE) asphalt6-converter/Asphalt6.swift tests/test_asphalt6_converter.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

.PHONY: test-asphalt6-converter
test-asphalt6-converter: $(BUILD_DIR)/test_asphalt6_converter
	./$(BUILD_DIR)/test_asphalt6_converter

$(BUILD_DIR)/test_cod4_converter: $(COD4_CONVERTER_CORE) tests/test_cod4_converter.swift | $(BUILD_DIR)
	$(SWIFTC) -O -swift-version 5 -warnings-as-errors -target $(shell uname -m)-apple-macosx11.0 -sdk $(SDK) $^ -o $@

test-cod4-converter: $(BUILD_DIR)/test_cod4_converter
	./$(BUILD_DIR)/test_cod4_converter

.PHONY: test-game-profile all bundle promote-loader icons clean run run-game controller-probe test-controller test-arb-program-guard test-arb-sampler-usage mode-switch-stress

all: $(BUILD_DIR)/mode32_probe $(BUILD_DIR)/game_loader

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

$(BUILD_DIR)/game_loader: src/game_loader.c src/guest_context.S src/guest_dyld.c src/macho_file.c src/hitch_recorder.c src/game_profile.c src/macho_loader.c src/compat_runtime.c src/guest_once.c src/audio_bridge.c src/audio_converter_bridge.c src/sound_manager_bridge.c src/audio_queue_bridge.c src/time_manager_bridge.c src/openal_bridge.c src/hid_bridge.c src/dispatch_bridge.m src/dlfcn_bridge.c src/gl_core_bridge.c src/asphalt_glsl.c src/curl_bridge.c src/steam_bridge.c src/steam_storage_fix.c src/resource_bridge.c src/stl_bridge.c src/libcpp_bridge.c src/libcpp_future_bridge.c src/cxx_exception_bridge.c src/libcpp_stream_bridge.cpp src/tlv_bridge.c src/blocks_bridge.c src/socket_bridge.c src/zlib_bridge.c src/sdl_bridge.m src/qtkit_bridge.m src/regex_bridge.c src/iconv_bridge.c src/rtti_bridge.c src/carbon_bridge.c src/carbon_text.m src/carbon_ui.m src/cfnetwork_bridge.c src/crypto_bridge.c src/quicktime_bridge.m src/controller_bridge.m src/objc_bridge.m src/objc_legacy_bridge.m src/arb_program_guard.c src/arb_sampler_usage.c src/gl_backend.c $(BUILD_DIR)/gl_dispatch.S src/mode32_switch.S src/import_gateway.S src/address_space_reserve.S $(wildcard src/*.h) $(BUILD_DIR)/gl_dispatch_names.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -I$(BUILD_DIR) $(LDFLAGS) $(filter %.c %.m %.cpp %.S,$^) -lc++ -lffi -lcurl -lz -liconv -framework CoreText -framework Security -framework ForceFeedback -framework OpenAL -framework CFNetwork -framework SystemConfiguration -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Cocoa -framework Carbon -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework GameController -framework IOKit -o $@
	codesign --force --sign - $@

$(BUILD_DIR)/game_loader: src/focus_policy.m

# OpenGL is reached through exported trampolines (src/gl_backend.h).
$(BUILD_DIR)/gl_dispatch.S: tools/gen_gl_dispatch.py lp32gl/data/apple-gl-exports.txt lp32gl/data/apple-cgl-exports.txt | $(BUILD_DIR)
	python3 tools/gen_gl_dispatch.py lp32gl/data/apple-gl-exports.txt lp32gl/data/apple-cgl-exports.txt --asm $@ --header $(BUILD_DIR)/gl_dispatch_names.h
$(BUILD_DIR)/gl_dispatch_names.h: $(BUILD_DIR)/gl_dispatch.S

$(BUILD_DIR)/test_game_profile: tests/test_game_profile.c src/game_profile.c src/game_profile.h src/macho_loader.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_game_profile.c src/game_profile.c -o $@
	codesign --force --sign - $@

test-game-profile: $(BUILD_DIR)/test_game_profile
	./$(BUILD_DIR)/test_game_profile

.PHONY: test-focus-policy
$(BUILD_DIR)/test_focus_policy: tests/test_focus_policy.m src/focus_policy.m src/focus_policy.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_focus_policy.m src/focus_policy.m -framework Cocoa -o $@

test-focus-policy: $(BUILD_DIR)/test_focus_policy
	env -u LP32_CONTINUE_WHEN_INACTIVE -u LP32_BACKGROUND_TEST -u LP32_TEST_FOCUS_LOSS ./$(BUILD_DIR)/test_focus_policy 0
	env -u LP32_BACKGROUND_TEST -u LP32_TEST_FOCUS_LOSS LP32_CONTINUE_WHEN_INACTIVE=1 ./$(BUILD_DIR)/test_focus_policy 1
	env -u LP32_BACKGROUND_TEST -u LP32_TEST_FOCUS_LOSS LP32_CONTINUE_WHEN_INACTIVE=0 ./$(BUILD_DIR)/test_focus_policy 0
	python3 tests/test_configure_bundle.py

.PHONY: test-focus-bridge
test-focus-bridge: $(BUILD_DIR)/game_loader
	env -u LP32_CONTINUE_WHEN_INACTIVE -u LP32_BACKGROUND_TEST -u LP32_TEST_FOCUS_LOSS LP32_FOCUS_SELFTEST=0 ./$(BUILD_DIR)/game_loader
	env -u LP32_BACKGROUND_TEST -u LP32_TEST_FOCUS_LOSS LP32_CONTINUE_WHEN_INACTIVE=1 LP32_FOCUS_SELFTEST=1 ./$(BUILD_DIR)/game_loader
	env -u LP32_BACKGROUND_TEST -u LP32_TEST_FOCUS_LOSS LP32_CONTINUE_WHEN_INACTIVE=0 LP32_FOCUS_SELFTEST=0 ./$(BUILD_DIR)/game_loader

$(BUILD_DIR)/test_macho_entry: tests/test_macho_entry.c src/macho_loader.c src/macho_loader.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_macho_entry.c -o $@

.PHONY: test-macho-entry
test-macho-entry: $(BUILD_DIR)/test_macho_entry
	./$(BUILD_DIR)/test_macho_entry

.PHONY: test-objc-proxy
test-objc-proxy: $(BUILD_DIR)/game_loader
	env LP32_OBJC_PROXY_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader

$(BUILD_DIR)/test_arb_sampler_usage: tests/test_arb_sampler_usage.c src/arb_sampler_usage.c src/arb_sampler_usage.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_sampler_usage.c src/arb_sampler_usage.c -o $@

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

ifneq ($(filter $(GAME),cod cod2 cod2mp cod4 cod4mp mw2 mw2mp mw3 mw3mp),)
run-game: promote-loader
	arch -x86_64 "./$(BUNDLE)/Contents/MacOS/$(LOADER_NAME)"
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

.PHONY: saga-update
saga-update:
	test -f "$(SAGA_UPDATE_ZIP)" || { echo "missing $(SAGA_UPDATE_ZIP); pass SAGA_UPDATE_ZIP=/path/to/legostarwarssaga_111_update.zip, or SAGA_UPDATE_ZIP= with SOURCE_APP=/path/to/an/updated.app" >&2; exit 1; }
	mkdir -p $(BUILD_DIR)/saga-update
	ditto -x -k "$(SAGA_UPDATE_ZIP)" $(BUILD_DIR)/saga-update

ifneq ($(filter $(GAME),cod cod2 cod2mp cod4 cod4mp),)
bundle: $(BUILD_DIR)/game_loader glmetal-lib tools/bundle_cod4.py tools/prepare_guest_runtime.py tools/package_glmetal.py
	python3 tools/bundle_cod4.py --game "$(COD_GAME)" --source-app "$(SOURCE_APP)" --mode "$(COD4_MODE)" --loader "$(BUILD_DIR)/game_loader" --bundle "$(BUNDLE)" --runtime "$(GUEST_RUNTIME_DIR)" --continue-when-inactive "$(CONTINUE_WHEN_INACTIVE)" --glmetal "$(GLMETAL_BUILD)"
else ifneq ($(filter $(GAME),mw2 mw2mp mw3 mw3mp),)
bundle: $(BUILD_DIR)/game_loader glmetal-lib tools/bundle_mw2.py tools/bundle_cod4.py tools/package_glmetal.py
	python3 tools/bundle_mw2.py --game "$(MW2_GAME)" --source-app "$(SOURCE_APP)" --mode "$(MW2_MODE)" --loader "$(BUILD_DIR)/game_loader" --bundle "$(BUNDLE)" --continue-when-inactive "$(CONTINUE_WHEN_INACTIVE)" --glmetal "$(GLMETAL_BUILD)"
else ifeq ($(GAME),gangstar)
bundle: $(BUILD_DIR)/game_loader glmetal-lib tools/bundle_gangstar.py tools/package_glmetal.py
	test -n "$(SOURCE_APP)" || { echo "pass SOURCE_APP=/path/to/GangstarMV.app (or its .dmg)" >&2; exit 1; }
	test -f "$(GUEST_RUNTIME_DIR)/libstdc++.6.dylib" || $(MAKE) cod4-runtime
	python3 tools/bundle_gangstar.py --source "$(SOURCE_APP)" --loader "$(BUILD_DIR)/game_loader" --bundle "$(BUNDLE)" --runtime "$(GUEST_RUNTIME_DIR)" --glmetal "$(GLMETAL_BUILD)" $(if $(filter apple,$(GANGSTAR_GL)),--apple-gl,)
else ifeq ($(GAME),asphalt6)
bundle: $(BUILD_DIR)/game_loader glmetal-lib tools/bundle_asphalt6.py tools/package_glmetal.py
	test -n "$(SOURCE_APP)" || { echo "pass SOURCE_APP=/path/to/Asphalt.6.zip (or the Mac app)" >&2; exit 1; }
	test -f "$(GUEST_RUNTIME_DIR)/libstdc++.6.dylib" || $(MAKE) cod4-runtime
	python3 tools/bundle_asphalt6.py --source "$(SOURCE_APP)" --loader "$(BUILD_DIR)/game_loader" --bundle "$(BUNDLE)" --runtime "$(GUEST_RUNTIME_DIR)" --glmetal "$(GLMETAL_BUILD)" $(if $(filter metal,$(ASPHALT6_GL)),--metal,--apple-gl)
else ifeq ($(GAME):$(SAGA_EDITION),saga:steam)
bundle: $(BUILD_DIR)/game_loader $(INFO_PLIST) tools/bundle_saga_steam.py
	python3 tools/bundle_saga_steam.py "$(SOURCE_APP)" "$(SAGA_DATA_SOURCE)" "$(BUILD_DIR)/game_loader" "$(BUNDLE)" "$(INFO_PLIST)"
	python3 tools/configure_bundle.py "$(BUNDLE)" --continue-when-inactive "$(CONTINUE_WHEN_INACTIVE)"
	codesign --force --deep --sign - "$(BUNDLE)"
else
bundle: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ) $(INFO_PLIST)
	if test "$(GAME)" = saga; then python3 tools/stage_saga_data.py "$(SAGA_DATA_SOURCE)" "$(BUILD_DIR)/Data"; fi
	test -d "$(SOURCE_CONTENTS)" || { echo "missing $(SOURCE_APP); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE)" >&2; exit 1; }
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources $(BUNDLE)/Contents/SharedSupport $(BUNDLE)/Contents/Library/Frameworks
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	cp "$(IMAGE_SOURCE)" $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	chmod 0644 $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	for data in $(GAME_DATA); do cp "$(SOURCE_CONTENTS)/MacOS/$$data" $(BUNDLE)/Contents/MacOS/$$data; done
	if test -n "$(CG_SOURCE)"; then ditto --noextattr --noqtn "$(CG_SOURCE)" $(BUNDLE)/Contents/Library/Frameworks/Cg.framework; fi
	cp $(INFO_PLIST) $(BUNDLE)/Contents/Info.plist
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/Resources" $(BUNDLE)/Contents/Resources
	cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME)
	if test "$(RESOURCE_FORKS)" = 1; then python3 tools/flatten_resource_forks.py $(BUNDLE); fi
	python3 tools/configure_bundle.py "$(BUNDLE)" --continue-when-inactive "$(CONTINUE_WHEN_INACTIVE)"
	codesign --force --deep --sign - $(BUNDLE)
endif

# Update only the compatibility runtime when the external source-data volume
# is unavailable. The existing bundle's game data and resources are retained.
promote-loader: $(BUILD_DIR)/game_loader
	test -d $(BUNDLE)/Contents/MacOS
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new
	mv $(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	if test -f $(LP32GL_OUT)/libLP32GL.dylib; then mkdir -p $(BUNDLE)/Contents/Frameworks && rm -rf $(BUNDLE)/Contents/Frameworks/LP32GL && ditto $(LP32GL_OUT) $(BUNDLE)/Contents/Frameworks/LP32GL; fi
	if test -f "$(ICON)"; then cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME); fi
	python3 tools/configure_bundle.py "$(BUNDLE)" --continue-when-inactive "$(CONTINUE_WHEN_INACTIVE)"
	codesign --force --deep --sign - $(BUNDLE)
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
	rm -f $(BUILD_DIR)/cod4-converter-arm64 $(BUILD_DIR)/cod4-converter-x86_64 $(BUILD_DIR)/test_cod4_converter

.PHONY: test-hitch-recorder
$(BUILD_DIR)/test_hitch_recorder: tests/test_hitch_recorder.c src/hitch_recorder.c src/hitch_recorder.h src/host_diagnostics.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) tests/test_hitch_recorder.c src/hitch_recorder.c -o $@
	codesign --force --sign - $@

test-hitch-recorder: $(BUILD_DIR)/test_hitch_recorder
	arch -x86_64 ./$(BUILD_DIR)/test_hitch_recorder

.PHONY: test-steam-storage-fix
STEAM_STORAGE_LIBRARY ?= $(BUILD_DIR)/LEGOMarvel-Compat.app/Contents/Resources/libsteam_api.dylib
$(BUILD_DIR)/test_steam_storage_fix: tests/test_steam_storage_fix.c src/steam_storage_fix.c src/steam_storage_fix.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) tests/test_steam_storage_fix.c src/steam_storage_fix.c -o $@
	codesign --force --sign - $@

test-steam-storage-fix: $(BUILD_DIR)/test_steam_storage_fix
	python3 tests/test_steam_storage_fix.py $(BUILD_DIR)/test_steam_storage_fix "$(STEAM_STORAGE_LIBRARY)"

.PHONY: test-stl-bridge
$(BUILD_DIR)/test_stl_bridge: tests/test_stl_bridge.c src/stl_bridge.c src/stl_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_stl_bridge.c src/stl_bridge.c -o $@
	codesign --force --sign - $@

test-stl-bridge: $(BUILD_DIR)/test_stl_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_stl_bridge

$(BUILD_DIR)/test_resource_bridge: tests/test_resource_bridge.c src/resource_bridge.c src/resource_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_resource_bridge.c src/resource_bridge.c -o $@
	codesign --force --sign - $@

test-resource-bridge: $(BUILD_DIR)/test_resource_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_resource_bridge

.PHONY: test-rtti-bridge
$(BUILD_DIR)/test_rtti_bridge: tests/test_rtti_bridge.c src/rtti_bridge.c src/rtti_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_rtti_bridge.c -o $@

test-rtti-bridge: $(BUILD_DIR)/test_rtti_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_rtti_bridge

.PHONY: test-carbon-geometry
.PHONY: test-carbon-input
test-carbon-input: $(BUILD_DIR)/game_loader
	env -u LP32_BACKGROUND_TEST LP32_CARBON_INPUT_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-carbon-dispatch
test-carbon-dispatch: $(BUILD_DIR)/game_loader
	env LP32_CARBON_DISPATCH_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-import-return
test-import-return: $(BUILD_DIR)/game_loader
	env LP32_IMPORT_RETURN_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

test-carbon-geometry: $(BUILD_DIR)/game_loader
	env LP32_CARBON_GEOMETRY_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-cfnetwork-bridge
$(BUILD_DIR)/test_cfnetwork_bridge: tests/test_cfnetwork_bridge.c src/cfnetwork_bridge.c src/cfnetwork_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_cfnetwork_bridge.c src/cfnetwork_bridge.c -framework CFNetwork -framework SystemConfiguration -framework CoreFoundation -o $@
	codesign --force --sign - $@

test-cfnetwork-bridge: $(BUILD_DIR)/test_cfnetwork_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_cfnetwork_bridge

.PHONY: test-libcpp-bridge
$(BUILD_DIR)/test_libcpp_bridge: tests/test_libcpp_bridge.c src/libcpp_bridge.c src/libcpp_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_libcpp_bridge.c src/libcpp_bridge.c -o $@

test-libcpp-bridge: $(BUILD_DIR)/test_libcpp_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_libcpp_bridge

$(BUILD_DIR)/test_audio_queue_bridge: tests/test_audio_queue_bridge.c src/audio_queue_bridge.c src/audio_queue_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc tests/test_audio_queue_bridge.c src/audio_queue_bridge.c -framework AudioToolbox -framework CoreAudio -o $@

$(BUILD_DIR)/test_audio_queue_allocate_race: tests/test_audio_queue_allocate_race.c src/audio_queue_bridge.c src/audio_queue_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc $< -framework AudioToolbox -framework CoreAudio -o $@

.PHONY: test-audio-queue-allocate-race
test-audio-queue-allocate-race: $(BUILD_DIR)/test_audio_queue_allocate_race
	arch -x86_64 ./$(BUILD_DIR)/test_audio_queue_allocate_race

$(BUILD_DIR)/test_audio_queue_movie: tests/test_audio_queue_movie.c src/audio_queue_bridge.c src/audio_queue_bridge.h src/guest_dyld.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc $< -framework AudioToolbox -framework CoreAudio -o $@

.PHONY: test-audio-queue-movie
test-audio-queue-movie: $(BUILD_DIR)/test_audio_queue_movie
	arch -x86_64 ./$(BUILD_DIR)/test_audio_queue_movie

$(BUILD_DIR)/test_audio_queue_callback_lifetime: tests/test_audio_queue_callback_lifetime.c src/audio_queue_bridge.c src/audio_queue_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc $< -framework AudioToolbox -framework CoreAudio -o $@

.PHONY: test-audio-queue-callback-lifetime
test-audio-queue-callback-lifetime: $(BUILD_DIR)/test_audio_queue_callback_lifetime
	arch -x86_64 ./$(BUILD_DIR)/test_audio_queue_callback_lifetime

.PHONY: test-audio-queue-bridge
test-audio-queue-bridge: $(BUILD_DIR)/test_audio_queue_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_audio_queue_bridge

.PHONY: test-blocks-bridge
$(BUILD_DIR)/test_blocks_bridge: tests/test_blocks_bridge.c src/blocks_bridge.c src/blocks_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_blocks_bridge.c src/blocks_bridge.c -o $@

test-blocks-bridge: $(BUILD_DIR)/test_blocks_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_blocks_bridge

.PHONY: test-curl-bridge
$(BUILD_DIR)/test_curl_bridge: tests/test_curl_bridge.c src/curl_bridge.c src/curl_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_curl_bridge.c src/curl_bridge.c -lcurl -o $@

test-curl-bridge: $(BUILD_DIR)/test_curl_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_curl_bridge

.PHONY: test-regex-bridge
$(BUILD_DIR)/test_regex_bridge: tests/test_regex_bridge.c src/regex_bridge.c src/regex_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_regex_bridge.c -o $@

test-regex-bridge: $(BUILD_DIR)/test_regex_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_regex_bridge

.PHONY: test-gl-core-bridge
$(BUILD_DIR)/test_gl_core_bridge: tests/test_gl_core_bridge.c src/gl_core_bridge.c src/asphalt_glsl.c src/asphalt_glsl.h src/game_profile.h src/gl_core_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_gl_core_bridge.c src/gl_core_bridge.c src/asphalt_glsl.c -framework OpenGL -o $@
	codesign --force --sign - $@

test-gl-core-bridge: $(BUILD_DIR)/test_gl_core_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_gl_core_bridge

.PHONY: test-asphalt6-glsl
$(BUILD_DIR)/test_asphalt6_glsl: tests/test_asphalt6_glsl.c src/asphalt_glsl.c src/asphalt_glsl.h | $(BUILD_DIR)
	$(CLANG) -isysroot $(SDK) -O2 -g -Wall -Wextra -Werror -Isrc tests/test_asphalt6_glsl.c src/asphalt_glsl.c -o $@

test-asphalt6-glsl: $(BUILD_DIR)/test_asphalt6_glsl
	./$(BUILD_DIR)/test_asphalt6_glsl

.PHONY: test-runloop-bridge
$(BUILD_DIR)/test_runloop_bridge: tests/test_runloop_bridge.m src/cfnetwork_bridge.c src/cfnetwork_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_runloop_bridge.m src/cfnetwork_bridge.c -framework Foundation -framework CFNetwork -framework SystemConfiguration -o $@

test-runloop-bridge: $(BUILD_DIR)/test_runloop_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_runloop_bridge

.PHONY: test-steam-bridge
$(BUILD_DIR)/test_steam_bridge: tests/test_steam_bridge.c src/steam_bridge.c src/steam_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_steam_bridge.c -o $@

test-steam-bridge: $(BUILD_DIR)/test_steam_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_steam_bridge

.PHONY: test-steam-achievement-guard
MARVEL_TEST_IMAGE ?= $(BUILD_DIR)/LEGOMarvel-Compat.app/Contents/SharedSupport/LEGOMarvel.image
test-steam-achievement-guard: $(BUILD_DIR)/game_loader
	env LP32_STEAM_ACHIEVEMENT_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(MARVEL_TEST_IMAGE)"

.PHONY: test-dlfcn-bridge
$(BUILD_DIR)/test_dlfcn_bridge: tests/test_dlfcn_bridge.c src/dlfcn_bridge.c src/dlfcn_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_dlfcn_bridge.c src/dlfcn_bridge.c -o $@

test-dlfcn-bridge: $(BUILD_DIR)/test_dlfcn_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_dlfcn_bridge

$(BUILD_DIR)/test_zlib_bridge: tests/test_zlib_bridge.c src/zlib_bridge.c src/zlib_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_zlib_bridge.c -lz -o $@

test-zlib-bridge: $(BUILD_DIR)/test_zlib_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_zlib_bridge

$(BUILD_DIR)/test_libcpp_stream_bridge: tests/test_libcpp_stream_bridge.cpp src/libcpp_stream_bridge.cpp | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_libcpp_stream_bridge.cpp -lc++ -o $@

test-libcpp-stream-bridge: $(BUILD_DIR)/test_libcpp_stream_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_libcpp_stream_bridge

$(BUILD_DIR)/test_tlv_bridge: tests/test_tlv_bridge.c src/tlv_bridge.c src/tlv_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_tlv_bridge.c -o $@

test-tlv-bridge: $(BUILD_DIR)/test_tlv_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_tlv_bridge

.PHONY: test-sound-manager-bridge
$(BUILD_DIR)/test_sound_manager_bridge: tests/test_sound_manager_bridge.c src/sound_manager_bridge.c src/sound_manager_bridge.h src/game_profile.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc tests/test_sound_manager_bridge.c -framework AudioToolbox -o $@

test-sound-manager-bridge: $(BUILD_DIR)/test_sound_manager_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_sound_manager_bridge

.PHONY: test-pointer-proxy
test-pointer-proxy: $(BUILD_DIR)/game_loader
	env LP32_POINTER_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 LP32_MUTE_AUDIO=1 arch -x86_64 ./$(BUILD_DIR)/game_loader

.PHONY: test-runtime-files
test-runtime-files: $(BUILD_DIR)/game_loader
	env LP32_FILE_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 LP32_MUTE_AUDIO=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-guest-exceptions
test-guest-exceptions: $(BUILD_DIR)/game_loader
	env LP32_EXCEPTION_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 LP32_MUTE_AUDIO=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-gamma-focus
test-gamma-focus: $(BUILD_DIR)/game_loader
	env -u LP32_BACKGROUND_TEST LP32_GAMMA_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 LP32_MUTE_AUDIO=1 arch -x86_64 ./$(BUILD_DIR)/game_loader

.PHONY: cod4-runtime
cod4-runtime:
	python3 tools/prepare_guest_runtime.py --output "$(BUILD_DIR)/guest-runtime"

$(BUILD_DIR)/test_macho_file: tests/test_macho_file.c src/macho_file.c src/macho_file.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_macho_file.c src/macho_file.c -o $@

.PHONY: test-macho-file test-cod4-bundle
test-macho-file: $(BUILD_DIR)/test_macho_file
	./$(BUILD_DIR)/test_macho_file

test-cod4-bundle:
	python3 tests/test_cod4_bundle.py

.PHONY: test-cod4-runtime test-cod4-initializers
test-cod4-runtime: $(BUILD_DIR)/game_loader
	python3 tests/test_cod4_runtime.py

.PHONY: test-cod4-server-browser
test-cod4-server-browser: $(BUILD_DIR)/game_loader
	env LP32_CTYPE_SELFTEST=1 LP32_FAIL_ON_UNSUPPORTED_IMPORT=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUILD_DIR)/COD4MP-Compat.app/Contents/SharedSupport/COD4MP.image"

test-cod4-initializers: $(BUILD_DIR)/game_loader
	env LP32_INITIALIZERS_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-time-manager-bridge
$(BUILD_DIR)/test_time_manager_bridge: tests/test_time_manager_bridge.c src/time_manager_bridge.c src/time_manager_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc tests/test_time_manager_bridge.c src/time_manager_bridge.c -framework CoreServices -o $@

test-time-manager-bridge: $(BUILD_DIR)/test_time_manager_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_time_manager_bridge

.PHONY: test-carbon-file
test-carbon-file: $(BUILD_DIR)/game_loader
	env LP32_CARBON_FILE_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-arb-output-aliases
$(BUILD_DIR)/test_arb_output_aliases: tests/test_arb_output_aliases.c src/arb_program_guard.c src/arb_program_guard.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_output_aliases.c src/arb_program_guard.c -framework OpenGL -o $@

test-arb-output-aliases: $(BUILD_DIR)/test_arb_output_aliases
	arch -x86_64 ./$(BUILD_DIR)/test_arb_output_aliases

.PHONY: test-audio-converter-bridge
$(BUILD_DIR)/test_audio_converter_bridge: tests/test_audio_converter_bridge.c src/audio_converter_bridge.c src/audio_converter_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc tests/test_audio_converter_bridge.c src/audio_converter_bridge.c -framework AudioToolbox -o $@

test-audio-converter-bridge: $(BUILD_DIR)/test_audio_converter_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_audio_converter_bridge

.PHONY: test-quicktime-bridge
$(BUILD_DIR)/test_quicktime_bridge: tests/test_quicktime_bridge.m src/quicktime_bridge.m src/quicktime_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc tests/test_quicktime_bridge.m src/quicktime_bridge.m -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework ImageIO -framework Carbon -framework Foundation -o $@

test-quicktime-bridge: $(BUILD_DIR)/test_quicktime_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_quicktime_bridge

.PHONY: test-audio-callback-lifetime
$(BUILD_DIR)/test_audio_callback_lifetime: tests/test_audio_callback_lifetime.c src/audio_bridge.c src/audio_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_audio_callback_lifetime.c -framework AudioToolbox -framework CoreAudio -o $@

test-audio-callback-lifetime: $(BUILD_DIR)/test_audio_callback_lifetime
	arch -x86_64 ./$(BUILD_DIR)/test_audio_callback_lifetime

.PHONY: test-audio-render-notify
$(BUILD_DIR)/test_audio_render_notify: tests/test_audio_render_notify.c src/audio_bridge.c src/audio_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_audio_render_notify.c -framework AudioToolbox -framework CoreAudio -o $@

test-audio-render-notify: $(BUILD_DIR)/test_audio_render_notify
	arch -x86_64 ./$(BUILD_DIR)/test_audio_render_notify

$(BUILD_DIR)/test_carbon_fullscreen: tests/test_carbon_fullscreen.m src/carbon_ui.m src/carbon_ui.h src/game_profile.h src/focus_policy.m src/focus_policy.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_carbon_fullscreen.m src/focus_policy.m -framework Cocoa -framework Carbon -framework OpenGL -o $@

.PHONY: test-carbon-fullscreen
test-carbon-fullscreen: $(BUILD_DIR)/test_carbon_fullscreen
	arch -x86_64 ./$(BUILD_DIR)/test_carbon_fullscreen

.PHONY: test-gl-buffer
test-gl-buffer: $(BUILD_DIR)/game_loader
	env LP32_GL_BUFFER_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"
	@test ! -f "$(BUILD_DIR)/MW2-Compat.app/Contents/SharedSupport/MW2.image" || \
	env LP32_GL_BUFFER_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUILD_DIR)/MW2-Compat.app/Contents/SharedSupport/MW2.image"

.PHONY: test-cod4-download
test-cod4-download: $(BUILD_DIR)/game_loader
	python3 tests/test_cod4_download.py

.PHONY: test-socket-bridge
$(BUILD_DIR)/test_socket_bridge: tests/test_socket_bridge.c src/socket_bridge.c src/socket_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_socket_bridge.c src/socket_bridge.c -o $@
test-socket-bridge: $(BUILD_DIR)/test_socket_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_socket_bridge

.PHONY: test-agl-pixel-format
$(BUILD_DIR)/test_agl_pixel_format: tests/test_agl_pixel_format.c src/agl_pixel_format.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_agl_pixel_format.c -o $@
test-agl-pixel-format: $(BUILD_DIR)/test_agl_pixel_format
	arch -x86_64 ./$(BUILD_DIR)/test_agl_pixel_format

$(BUILD_DIR)/test_audio_queue_control_race: tests/test_audio_queue_control_race.c src/audio_queue_bridge.c src/audio_queue_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc $< -framework AudioToolbox -framework CoreAudio -o $@

.PHONY: test-audio-queue-control-race
test-audio-queue-control-race: $(BUILD_DIR)/test_audio_queue_control_race
	arch -x86_64 ./$(BUILD_DIR)/test_audio_queue_control_race

$(BUILD_DIR)/test_guest_once: tests/test_guest_once.c src/guest_once.c src/guest_once.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_guest_once.c src/guest_once.c -o $@

.PHONY: test-guest-once
test-guest-once: $(BUILD_DIR)/test_guest_once
	arch -x86_64 ./$(BUILD_DIR)/test_guest_once

$(BUILD_DIR)/benchmark_guest_once: tests/benchmark_guest_once.c src/guest_once.c src/guest_once.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/benchmark_guest_once.c src/guest_once.c -o $@

.PHONY: benchmark-guest-once
benchmark-guest-once: $(BUILD_DIR)/benchmark_guest_once
	arch -x86_64 ./$(BUILD_DIR)/benchmark_guest_once

.PHONY: test-guest-once-abi
test-guest-once-abi: $(BUILD_DIR)/game_loader
	python3 tests/test_guest_once_abi.py

.PHONY: test-pure-atomic
test-pure-atomic: $(BUILD_DIR)/game_loader
	python3 tests/test_pure_atomic.py
