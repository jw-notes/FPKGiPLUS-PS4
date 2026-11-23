# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of FPKGi+. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.

# FPKGi+ build. Run inside WSL (or any Linux) from the project root:
#
#   make host          the Linux build used for development and tests
#   make tests         unit tests (host)
#   make ps4           eboot for the console
#
# PowerShell users: tools\build.ps1 runs the PS4 build and packages it.
# The OpenOrbis toolchain is shared with Librarian (../Librarian/tools/toolchain)
# unless tools/toolchain exists here.

TOOLCHAIN   ?= $(if $(wildcard $(CURDIR)/tools/toolchain),$(CURDIR)/tools/toolchain,$(abspath $(CURDIR)/../Librarian/tools/toolchain))
OO_SDK      ?= $(TOOLCHAIN)/OpenOrbis/PS4Toolchain
LLD         ?= $(TOOLCHAIN)/llvm-local/usr/lib/llvm-18/bin/lld
PS4_CXX     ?= clang++-18
PS4_CC      ?= clang-18
HOST_CXX    ?= g++
HOST_CC     ?= gcc

APP_VERSION := $(shell python3 -c 'import json;print(json.load(open("version.json"))["app_version"])')

APP_SOURCES := $(wildcard source/core/*.cpp source/gfx/*.cpp source/store/catalog.cpp source/store/covers.cpp \
                 source/store/downloads.cpp source/store/installer.cpp source/store/metadata.cpp source/store/homebrew_updates.cpp source/store/updater.cpp source/ui/*.cpp) \
               source/net/http.cpp source/net/url.cpp source/net/archive.cpp source/net/certificates.cpp
WEBP_SOURCES := $(wildcard third_party/libwebp/src/dec/*.c third_party/libwebp/src/dsp/*.c \
                  third_party/libwebp/src/utils/*.c)
MINIZ_SOURCES := third_party/miniz/miniz.c
ESCAPE_SOURCES := source/platform/sandbox_goldhen.cpp
ESCAPE_LIB_SOURCES := third_party/goldhen_sdk/source/GoldHEN.c third_party/goldhen_sdk/source/Syscall.c

INCLUDES := -I. -Isource -Ithird_party -Ithird_party/libwebp -Ithird_party/libwebp/src -Ithird_party/miniz

# ---- host --------------------------------------------------------------------
HOST_OBJ := build/host/obj
HOST_FLAGS := -DLIB_HOST -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(INCLUDES)
HOST_APP_OBJS := $(patsubst %.cpp,$(HOST_OBJ)/%.o,$(APP_SOURCES) source/net/http_host.cpp \
                   source/platform/platform_host.cpp source/store/installer_host.cpp source/store/service_host.cpp)
HOST_LIB_OBJS := $(patsubst %.c,$(HOST_OBJ)/%.o,$(WEBP_SOURCES) $(MINIZ_SOURCES))

$(HOST_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_FLAGS) -MMD -c $< -o $@

$(HOST_OBJ)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(INCLUDES) -c $< -o $@

build/host/fpkgi-plus: $(HOST_APP_OBJS) $(HOST_LIB_OBJS) $(HOST_OBJ)/source/main.o
	$(HOST_CXX) $^ -lpthread -lm -o $@

host: build/host/fpkgi-plus

TEST_SOURCES := $(wildcard tests/*.cpp)
build/host/tests: $(HOST_APP_OBJS) $(HOST_LIB_OBJS) $(patsubst %.cpp,$(HOST_OBJ)/%.o,$(TEST_SOURCES))
	$(HOST_CXX) $^ -lpthread -lm -o $@

build/host/native/installer_ps4.o: source/store/installer_ps4.cpp source/store/bgft_abi.h Makefile
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_FLAGS) -idirafter $(OO_SDK)/include -MMD -c $< -o $@

build/host/native/installer_test.o: tests/native/installer.cpp Makefile
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_FLAGS) -idirafter $(OO_SDK)/include -MMD -c $< -o $@

build/host/native-installer-tests: $(filter-out $(HOST_OBJ)/source/store/installer_host.o,$(HOST_APP_OBJS)) $(HOST_LIB_OBJS) build/host/native/installer_ps4.o build/host/native/installer_test.o
	$(HOST_CXX) $^ -lpthread -lm -o $@

native-tests: build/host/native-installer-tests
	./build/host/native-installer-tests

tests: build/host/tests
	./build/host/tests
	python3 tools/check_tls_assets.py

# ---- PS4 ---------------------------------------------------------------------
PS4_BUILD := build/ps4
PS4_OBJ := $(PS4_BUILD)/obj
PS4_TARGET := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -isysroot $(OO_SDK) -isystem $(OO_SDK)/include
PS4_FLAGS := $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DLIB_VERSION=\"$(APP_VERSION)\" \
             -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(INCLUDES)
PS4_APP_OBJS := $(patsubst %.cpp,$(PS4_OBJ)/%.o,$(APP_SOURCES) source/net/http_ps4.cpp \
                  source/platform/platform_ps4.cpp source/store/installer_ps4.cpp source/store/service_ps4.cpp                   $(ESCAPE_SOURCES) source/main.cpp)
PS4_LIB_OBJS := $(patsubst %.c,$(PS4_OBJ)/%.o,$(WEBP_SOURCES) $(MINIZ_SOURCES) $(ESCAPE_LIB_SOURCES))

# The version is compiled into the app; changing it must rebuild the objects.
$(PS4_APP_OBJS): version.json

$(PS4_OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(PS4_CXX) -std=c++17 -isystem $(OO_SDK)/include/c++/v1 $(PS4_FLAGS) -fno-exceptions -MMD -c $< -o $@

$(PS4_OBJ)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -w $(INCLUDES) -c $< -o $@

# GoldHEN Plugins SDK: the SDK command wrappers and its syscall stub.
$(PS4_OBJ)/third_party/goldhen_sdk/%.o: third_party/goldhen_sdk/%.c
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -w -Ithird_party/goldhen_sdk/include -c $< -o $@

# libSceBgft and libSceAppInstUtil are not linked: store/installer_ps4.cpp loads them at run time.
PS4_LIBS := -lc++ -lc -lm -lpthread -lclang_rt.builtins-x86_64 -lkernel -lSceLibcInternal -lSceSysmodule \
            -lSceSystemService -lSceUserService -lSceVideoOut -lScePad -lSceNet -lSceNetCtl -lSceHttp -lSceSsl \
            -lSceAudioOut -lSceLncUtil

$(PS4_BUILD)/FPKGiPlus.elf: $(PS4_APP_OBJS) $(PS4_LIB_OBJS)
	$(LLD) -flavor gnu -m elf_x86_64 -pie --wrap=clock_gettime --script $(OO_SDK)/link.x --eh-frame-hdr \
	  -L$(OO_SDK)/lib $(OO_SDK)/lib/crt1.o $^ --start-group $(PS4_LIBS) --end-group -o $@

$(PS4_OBJ)/itemzflow_jbc.o: third_party/itemzflow_jbc/src/multi-jb.c third_party/itemzflow_jbc/include/multi-jb.h
	@mkdir -p $(dir $@)
	$(PS4_CC) $(PS4_TARGET) -O2 -Ithird_party/itemzflow_jbc/include -Wno-macro-redefined -c $< -o $@

$(PS4_BUILD)/fpkgi-jbc.prx: $(PS4_OBJ)/itemzflow_jbc.o Makefile
	$(LLD) -flavor gnu -m elf_x86_64 -pie -e module_start --script $(OO_SDK)/link.x --eh-frame-hdr -L$(OO_SDK)/lib $< -lc -lkernel $(OO_SDK)/lib/crtlib.o -o $(PS4_BUILD)/fpkgi-jbc.elf
	OO_PS4_TOOLCHAIN=$(OO_SDK) $(OO_SDK)/bin/linux/create-fself -in=$(PS4_BUILD)/fpkgi-jbc.elf -out=$(PS4_BUILD)/fpkgi-jbc.oelf --lib=$@ --paid 0x3800000000000011

# The background downloader (source/service, store/service.h): the app's own
# code without its main, shipped inside the package as service/service.self.
PS4_SERVICE_OBJS := $(filter-out $(PS4_OBJ)/source/main.o,$(PS4_APP_OBJS)) $(PS4_OBJ)/source/service/main.o
$(PS4_BUILD)/service.elf: $(PS4_SERVICE_OBJS) $(PS4_LIB_OBJS)
	$(LLD) -flavor gnu -m elf_x86_64 -pie --wrap=clock_gettime --script $(OO_SDK)/link.x --eh-frame-hdr 	  -L$(OO_SDK)/lib $(OO_SDK)/lib/crt1.o $^ --start-group $(PS4_LIBS) --end-group -o $@

$(PS4_BUILD)/service.self: $(PS4_BUILD)/service.elf
	OO_PS4_TOOLCHAIN=$(OO_SDK) $(OO_SDK)/bin/linux/create-fself -in=$< -out=$(PS4_BUILD)/service.oelf --eboot $@ --paid 0x3800000000000011

ps4: $(PS4_BUILD)/FPKGiPlus.elf $(PS4_BUILD)/fpkgi-jbc.prx $(PS4_BUILD)/service.self

clean:
	rm -rf build/host build/ps4

.PHONY: host tests native-tests ps4 clean
-include $(shell find build -name '*.d' 2>/dev/null)

# The package's artwork (assets/icon0.png, assets/pic1.png), drawn by the app's renderer.
build/host/make_art: $(HOST_APP_OBJS) $(HOST_LIB_OBJS) $(HOST_OBJ)/tools/make_art.o
	$(HOST_CXX) $^ -lpthread -lm -o $@

art: build/host/make_art
	./build/host/make_art

.PHONY: art
