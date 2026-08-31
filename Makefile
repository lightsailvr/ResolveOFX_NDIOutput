# NDI Output Plugin Makefile

# Version management
VERSION_FILE = VERSION
VERSION = $(shell cat $(VERSION_FILE))

# Directories
NDI_SDK_PATH = "/Library/NDI Advanced SDK for Apple"
NDI_INCLUDE = $(NDI_SDK_PATH)/include
NDI_LIB = $(NDI_SDK_PATH)/lib/macOS/libndi_advanced.dylib

# Deployment target / architectures.
# libndi_advanced.dylib requires macOS 13+, so that is our floor. Without an
# explicit -mmacosx-version-min the binary is stamped with the build host's OS
# version and refuses to load on anything older (bit us in v1.12: minos 26.0).
# ARCHFLAGS is empty for dev builds (host arch); release packaging passes
# ARCHFLAGS="-arch arm64 -arch x86_64" for a universal binary.
DEPLOYMENT_TARGET ?= 13.0
ARCHFLAGS ?=

# Compiler settings
# third_party/braw holds the vendored (Boost-licensed) Blackmagic RAW API
# header + dispatch shim; the framework itself is resolved at runtime from the
# host app (Resolve ships it), so nothing Blackmagic is linked or bundled.
CXX = c++
CXXFLAGS = -c -fvisibility=hidden -mmacosx-version-min=$(DEPLOYMENT_TARGET) $(ARCHFLAGS) -Iopenfx/include -I$(NDI_INCLUDE) -Ithird_party/braw
OBJCXXFLAGS = -c -fvisibility=hidden -mmacosx-version-min=$(DEPLOYMENT_TARGET) $(ARCHFLAGS) -Iopenfx/include -I$(NDI_INCLUDE) -x objective-c++
LDFLAGS = -bundle -fvisibility=hidden -mmacosx-version-min=$(DEPLOYMENT_TARGET) $(ARCHFLAGS) -exported_symbols_list openfx/Support/include/osxSymbols $(NDI_LIB) -framework Metal -framework MetalKit -framework Foundation -framework AppKit -framework UniformTypeIdentifiers -lz

# Source files
SOURCES = src/NDIOutputPlugin.cpp src/BRAWImmersiveReader.cpp src/TimelineClipWatcher.cpp
OBJCXX_SOURCES = src/MetalGPUAcceleration.mm src/MacFileDialog.mm
OBJECTS = $(SOURCES:.cpp=.o) $(OBJCXX_SOURCES:.mm=.o)

# Bundle structure
BUNDLE_NAME = NDIOutput.ofx.bundle
BUNDLE_EXECUTABLE = $(BUNDLE_NAME)/Contents/MacOS/NDIOutput.ofx

# Build targets
.PHONY: all clean dev install test test-metal

all: $(BUNDLE_EXECUTABLE)

dev: $(BUNDLE_EXECUTABLE)

# Host-independent unit tests (no Resolve or NDI SDK needed)
test:
	mkdir -p build
	$(CXX) -Isrc tests/test_render_probe.cpp -o build/test_render_probe
	./build/test_render_probe
	$(CXX) -Isrc tests/test_stream_resolution.cpp -o build/test_stream_resolution
	./build/test_stream_resolution
	$(CXX) -Isrc tests/test_stereo_pair.cpp -o build/test_stereo_pair
	./build/test_stereo_pair
	$(CXX) -Isrc tests/test_platform_paths.cpp -o build/test_platform_paths
	./build/test_platform_paths
	$(CXX) -Isrc tests/test_ndi_loader.cpp -o build/test_ndi_loader
	./build/test_ndi_loader
	$(CXX) -Isrc tests/test_stmap.cpp -o build/test_stmap -lz
	./build/test_stmap
	$(CXX) -Isrc tests/test_brawmap.cpp -o build/test_brawmap -lz
	./build/test_brawmap

# GPU kernel correctness tests (needs a Metal device, but no Resolve or NDI SDK)
test-metal: src/MetalGPUAcceleration.o
	mkdir -p build
	$(CXX) -Isrc tests/test_metal_downscale.mm src/MetalGPUAcceleration.o \
		-o build/test_metal_downscale -framework Metal -framework Foundation -lz
	./build/test_metal_downscale

# Pipeline timing harness at production 8K dims (needs Metal + NDI SDK, no
# Resolve). Reproduces the plugin's per-pair send pattern; see the file header.
bench: src/MetalGPUAcceleration.o
	mkdir -p build
	$(CXX) -Isrc -I$(NDI_INCLUDE) tests/bench_pipeline.mm src/MetalGPUAcceleration.o \
		-o build/bench_pipeline -framework Metal -framework Foundation $(NDI_LIB)
	install_name_tool -change "@rpath/libndi_advanced.dylib" $(NDI_LIB) build/bench_pipeline
	./build/bench_pipeline

$(BUNDLE_EXECUTABLE): $(OBJECTS) | bundle_structure
	@echo "Building NDI Output Plugin v$(VERSION)"
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)
	@echo "Built NDI Output Plugin v$(VERSION) successfully!"

# Object file compilation
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

%.o: %.mm
	$(CXX) $(OBJCXXFLAGS) $< -o $@

# Bundle structure
bundle_structure:
	mkdir -p $(BUNDLE_NAME)/Contents/MacOS
	mkdir -p $(BUNDLE_NAME)/Contents/Resources
	cp BaldavengerOFX.NDIOutput.png $(BUNDLE_NAME)/Contents/Resources/
	cp Info.plist $(BUNDLE_NAME)/Contents/
	cp src/ndi_timeline_watch.py $(BUNDLE_NAME)/Contents/Resources/

# Installation
install: $(BUNDLE_EXECUTABLE)
	sudo rm -rf "/Library/OFX/Plugins/$(BUNDLE_NAME)"
	sudo cp -R $(BUNDLE_NAME) "/Library/OFX/Plugins/"
	sudo install_name_tool -change "@rpath/libndi_advanced.dylib" $(NDI_LIB) "/Library/OFX/Plugins/$(BUNDLE_EXECUTABLE)"

# Clean
clean:
	rm -rf $(BUNDLE_NAME)
	rm -f *.o src/*.o
	rm -rf build

# Version increment (for development)
bump-patch:
	@echo "$(shell echo $(VERSION) | awk -F. '{print $$1"."$$2"."$$3+1}')" > $(VERSION_FILE)
	@echo "Version bumped to $(shell cat $(VERSION_FILE))"

bump-minor:
	@echo "$(shell echo $(VERSION) | awk -F. '{print $$1"."$$2+1".0"}')" > $(VERSION_FILE)
	@echo "Version bumped to $(shell cat $(VERSION_FILE))"

bump-major:
	@echo "$(shell echo $(VERSION) | awk -F. '{print $$1+1".0.0"}')" > $(VERSION_FILE)
	@echo "Version bumped to $(shell cat $(VERSION_FILE))" 