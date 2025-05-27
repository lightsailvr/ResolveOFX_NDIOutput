PLUGINNAME = NDIOutput

INSTALL_PLUGIN := TRUE
CPU_ONLY := TRUE

UNAME_SYSTEM := $(shell uname -s)
ifeq ($(UNAME_SYSTEM), Darwin)
OS = macOS
GPU = CPU
endif
ifeq ($(UNAME_SYSTEM), Linux)
OS = Linux
GPU = CPU
endif

DR_VERSION = DR17

# Build modern version (default)
PLUGINOBJECTSCPU = $(PLUGINNAME)Plugin.o
RESOURCES = BaldavengerOFX.$(PLUGINNAME).png

PLUGINOBJECTS = $(PLUGINOBJECTSCPU)

PLUGINNAME_LG = $(PLUGINNAME)

TOP_SRCDIR = .
SRCDIR = src

# NDI Advanced SDK library linking
ifeq ($(UNAME_SYSTEM), Darwin)
CPPFLAGS += -I/Library/NDI_Advanced_SDK/include
CPUFLAGS += -L/Library/NDI_Advanced_SDK/lib/macOS -lndi_advanced
# GPU acceleration support for macOS (Metal)
CPUFLAGS += -framework Metal -framework MetalKit -framework OpenGL
endif

# Linux NDI Advanced SDK support
ifeq ($(UNAME_SYSTEM), Linux)
CPPFLAGS += -I"/usr/local/include/ndi-advanced"
CPUFLAGS += -lndi-advanced
# GPU acceleration support for Linux (OpenGL)
CPUFLAGS += -lGL -lGLEW
endif

# Override source directory
vpath %.cpp $(SRCDIR)
vpath %.h $(SRCDIR)

# Modern plugin build settings (C API only)
CXXFLAGS = -fvisibility=hidden -Iopenfx/include
LINKFLAGS = -bundle -fvisibility=hidden -exported_symbols_list openfx/Support/include/osxSymbols

# Add NDI Advanced SDK linking
ifeq ($(UNAME_SYSTEM), Darwin)
LINKFLAGS += /Library/NDI_Advanced_SDK/lib/macOS/libndi_advanced.dylib
endif

ifeq ($(UNAME_SYSTEM), Linux)
LINKFLAGS += -lndi-advanced
endif

# Default target builds modern version with version increment
all: increment-version $(PLUGINNAME).ofx.bundle

# Development build without version increment
dev: $(PLUGINNAME).ofx.bundle

# Increment version before building
increment-version:
	@echo "Incrementing version..."
	@./scripts/increment_version.sh

# Modern version using C API directly (default)
$(PLUGINNAME).ofx.bundle: version-info $(PLUGINOBJECTS) $(RESOURCES)
	mkdir -p $@/Contents/$(OS)
	cp $(RESOURCES) $@/Contents/
	cp Info.plist $@/Contents/
	$(CXX) $(PLUGINOBJECTS) -o $@/Contents/$(OS)/$(PLUGINNAME).ofx $(LINKFLAGS)
	@echo "Built NDI Output Plugin v$$(cat VERSION) successfully!"

$(PLUGINNAME)Plugin.o: $(PLUGINNAME)Plugin.cpp
	$(CXX) -c $(CXXFLAGS) $(CPPFLAGS) $< -o $@

# Install modern version (default)
install: $(PLUGINNAME).ofx.bundle
	sudo rm -rf "/Library/OFX/Plugins/$(PLUGINNAME).ofx.bundle"
	sudo cp -R $(PLUGINNAME).ofx.bundle "/Library/OFX/Plugins/"
	sudo install_name_tool -change "@rpath/libndi_advanced.dylib" "/Library/NDI_Advanced_SDK/lib/macOS/libndi_advanced.dylib" "/Library/OFX/Plugins/$(PLUGINNAME).ofx.bundle/Contents/$(OS)/$(PLUGINNAME).ofx"



# Clean targets
clean:
	rm -rf $(PLUGINNAME).ofx.bundle
	rm -f *.o

# Version management
show-version:
	@echo "Current version: $$(cat VERSION)"

# Add version info to build
version-info:
	@echo "Building NDI Output Plugin v$$(cat VERSION)"

.PHONY: all dev install clean increment-version show-version version-info 