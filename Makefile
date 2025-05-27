PLUGINNAME = NDIOutput

INSTALL_PLUGIN := TRUE

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

PLUGINOBJECTSCPU = $(PLUGINNAME)Plugin.o
RESOURCES = BaldavengerOFX.$(PLUGINNAME).png

PLUGINOBJECTS = $(PLUGINOBJECTSCPU)

PLUGINNAME_LG = $(PLUGINNAME).$(OS).$(DR_VERSION).$(GPU)

TOP_SRCDIR = .
SRCDIR = src

# NDI library linking
ifeq ($(UNAME_SYSTEM), Darwin)
CPPFLAGS += -I"/Library/NDI SDK for Apple/include"
PLUGINOBJECTS += src/libndi.dylib
endif

# Linux NDI support
ifeq ($(UNAME_SYSTEM), Linux)
CPPFLAGS += -I"/usr/local/include/ndi"
LDFLAGS += -lndi
endif

# Override source directory
vpath %.cpp $(SRCDIR)
vpath %.h $(SRCDIR)

OFXPATH = openfx
OFXSEXTPATH = SupportExt
PATHTOROOT = $(OFXPATH)/Support

include $(PATHTOROOT)/Makefile.master 