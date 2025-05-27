# NDI Output Plugin Makefile

# Version management
VERSION_FILE = VERSION
VERSION = $(shell cat $(VERSION_FILE))

# Directories
NDI_SDK_PATH = "/Library/NDI Advanced SDK for Apple"
NDI_INCLUDE = $(NDI_SDK_PATH)/include
NDI_LIB = $(NDI_SDK_PATH)/lib/macOS/libndi_advanced.dylib

# Compiler settings
CXX = c++
CXXFLAGS = -c -fvisibility=hidden -Iopenfx/include -I$(NDI_INCLUDE)
OBJCXXFLAGS = -c -fvisibility=hidden -Iopenfx/include -I$(NDI_INCLUDE) -x objective-c++
LDFLAGS = -bundle -fvisibility=hidden -exported_symbols_list openfx/Support/include/osxSymbols $(NDI_LIB) -framework Metal -framework MetalKit -framework Foundation

# Source files
SOURCES = src/NDIOutputPlugin.cpp
OBJCXX_SOURCES = src/MetalGPUAcceleration.mm
OBJECTS = $(SOURCES:.cpp=.o) $(OBJCXX_SOURCES:.mm=.o)

# Bundle structure
BUNDLE_NAME = NDIOutput.ofx.bundle
BUNDLE_EXECUTABLE = $(BUNDLE_NAME)/Contents/macOS/NDIOutput.ofx

# Build targets
.PHONY: all clean dev install

all: $(BUNDLE_EXECUTABLE)

dev: $(BUNDLE_EXECUTABLE)

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
	mkdir -p $(BUNDLE_NAME)/Contents/macOS
	cp BaldavengerOFX.NDIOutput.png $(BUNDLE_NAME)/Contents/
	cp Info.plist $(BUNDLE_NAME)/Contents/

# Installation
install: $(BUNDLE_EXECUTABLE)
	sudo rm -rf "/Library/OFX/Plugins/$(BUNDLE_NAME)"
	sudo cp -R $(BUNDLE_NAME) "/Library/OFX/Plugins/"
	sudo install_name_tool -change "@rpath/libndi_advanced.dylib" $(NDI_LIB) "/Library/OFX/Plugins/$(BUNDLE_EXECUTABLE)"

# Clean
clean:
	rm -rf $(BUNDLE_NAME)
	rm -f *.o

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