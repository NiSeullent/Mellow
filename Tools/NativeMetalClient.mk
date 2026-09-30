# SPDX-License-Identifier: MIT
# Build-only native macOS entry point. Does not run a client, install a kext,
# register a Metal plugin, download dependencies, or change host configuration.
ifeq ($(shell uname -s),Darwin)
else
$(error NativeMetalClient.mk requires macOS with an installed Apple SDK)
endif

MELLOW_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
MELLOW_ARCH ?= x86_64
MELLOW_SDK := $(shell xcrun --sdk macosx --show-sdk-path)
MELLOW_SDK_VERSION := $(shell xcrun --sdk macosx --show-sdk-version)
MELLOW_OUT ?= $(MELLOW_ROOT)/build/native-metal-client/$(MELLOW_ARCH)-sdk$(MELLOW_SDK_VERSION)
MELLOW_CXX := $(shell xcrun --sdk macosx --find clang++)
ifeq ($(strip $(MELLOW_SDK)),)
$(error xcrun did not return an installed macOS SDK)
endif

MELLOW_FLAGS := -std=c++17 -O2 -g -Wall -Wextra -pthread -arch $(MELLOW_ARCH) \
                -isysroot "$(MELLOW_SDK)" -mmacosx-version-min=15.0 \
                -I"$(MELLOW_ROOT)/Runtime" -MMD -MP
MELLOW_CPP := Runtime/PlatformRuntime.cpp Runtime/OpenCLProvider.cpp \
              Runtime/ShaderJit.cpp Runtime/AirDecoder.cpp Runtime/MetalObjects.cpp \
              Runtime/OpenGLProvider.cpp Runtime/RenderShaderJit.cpp Runtime/RenderObjects.cpp
MELLOW_OBJC := Runtime/NativeMetalCompute.mm Runtime/NativeMetalRender.mm \
               Runtime/WindowSurfacePresenter.mm Tools/native-metal-client.mm
MELLOW_OBJECTS := $(addprefix $(MELLOW_OUT)/,$(MELLOW_CPP:.cpp=.o) $(MELLOW_OBJC:.mm=.o))

.PHONY: all
all: $(MELLOW_OUT)/native-metal-client $(MELLOW_OUT)/native-metal-abi

$(MELLOW_OUT)/native-metal-abi: $(MELLOW_OUT)/Tools/native-metal-abi.o
	$(MELLOW_CXX) -arch $(MELLOW_ARCH) -isysroot "$(MELLOW_SDK)" -mmacosx-version-min=15.0 \
	    "$<" -framework Foundation -framework Metal -o "$@"

$(MELLOW_OUT)/native-metal-client: $(MELLOW_OBJECTS)
	$(MELLOW_CXX) -arch $(MELLOW_ARCH) -isysroot "$(MELLOW_SDK)" -mmacosx-version-min=15.0 \
	    $(MELLOW_OBJECTS) -framework Foundation -framework Metal -framework AppKit \
	    -framework QuartzCore -framework OpenGL -framework IOSurface -framework CoreGraphics -o "$@"

$(MELLOW_OUT)/%.o: $(MELLOW_ROOT)/%.cpp $(MELLOW_ROOT)/Tools/NativeMetalClient.mk
	@mkdir -p "$(@D)"
	$(MELLOW_CXX) $(MELLOW_FLAGS) -c "$<" -o "$@"

$(MELLOW_OUT)/%.o: $(MELLOW_ROOT)/%.mm $(MELLOW_ROOT)/Tools/NativeMetalClient.mk
	@mkdir -p "$(@D)"
	$(MELLOW_CXX) $(MELLOW_FLAGS) -fobjc-arc -fblocks -c "$<" -o "$@"

-include $(MELLOW_OBJECTS:.o=.d) $(MELLOW_OUT)/Tools/native-metal-abi.d
