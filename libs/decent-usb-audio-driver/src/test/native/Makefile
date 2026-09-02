# Host test for the isochronous URB pipeline.
#
# Linux only: it includes <linux/usbdevice_fs.h>. It needs no Android SDK, no
# NDK, no Gradle and no DAC — the kernel is replaced by a fake usbdevfs behind
# the UsbAudioBackend seam.
#
#   cd libs/decent-usb-audio-driver/src/test/native
#   make            # build and run
#   make clean
#
# On a non-Linux host, run it in a container:
#   docker run --rm -v "$PWD/../../../../..":/w -w /w/libs/decent-usb-audio-driver/src/test/native \
#       debian:trixie sh -c 'apt-get update -qq && apt-get install -y -qq g++ make && make'

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O1 -g -Wall -Wextra -Werror
JNI_DIR  := ../../main/jni
BUILD    := build
BIN      := $(BUILD)/usb-audio-output-test

SOURCES := $(JNI_DIR)/usb-audio-output.cpp fake-usbdevfs.cpp usb-audio-output-test.cpp

.PHONY: all run clean
all: run

run: $(BIN)
	./$(BIN)

$(BIN): $(SOURCES) $(JNI_DIR)/usb-audio-output.h fake-usbdevfs.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(JNI_DIR) -I. -o $@ $(SOURCES)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
