RACK_DIR ?= $(HOME)/Rack2-SDK/Rack-SDK

FLAGS +=
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard README*)
DISTRIBUTABLES += $(wildcard patches)

include $(RACK_DIR)/plugin.mk
