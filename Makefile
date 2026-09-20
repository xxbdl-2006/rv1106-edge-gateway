CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc

SDK_ROOT ?= /home/aaazhx/luckfox-pico
MPP_ROOT ?= /home/aaazhx/luckfox-pico/media/mpp/release_mpp_rv1106_arm-rockchip830-linux-uclibcgnueabihf
MPP_INCLUDE_DIR := $(MPP_ROOT)/include
MPP_HEADER_DIR := $(MPP_INCLUDE_DIR)/rockchip
MPP_LIBRARY_DIR := $(MPP_ROOT)/lib

ROCKIT_INCLUDE_DIR ?= $(SDK_ROOT)/media/rockit/rockit/mpi/sdk/include
ROCKIT_LIBRARY_DIR ?= $(SDK_ROOT)/media/out/lib
ROCKIT_ROOT_LIBRARY_DIR ?= $(SDK_ROOT)/media/out/root/usr/lib

CPPFLAGS ?=
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -std=gnu11
LDFLAGS ?=
LDLIBS ?= -lrt

MPP_CPPFLAGS := -I$(MPP_INCLUDE_DIR) -I$(MPP_HEADER_DIR)
MPP_LDFLAGS := -L$(MPP_LIBRARY_DIR) \
	-Wl,-rpath-link,$(MPP_LIBRARY_DIR) \
	-Wl,-rpath,/oem/usr/lib
MPP_LDLIBS := -lrockchip_mpp -lstdc++ -lpthread -lrt -ldl -lm

ROCKIT_CPPFLAGS := -I$(ROCKIT_INCLUDE_DIR)
ROCKIT_LDFLAGS := -L$(ROCKIT_LIBRARY_DIR) \
	-L$(ROCKIT_ROOT_LIBRARY_DIR) \
	-Wl,-rpath-link,$(ROCKIT_LIBRARY_DIR):$(ROCKIT_ROOT_LIBRARY_DIR) \
	-Wl,-rpath,/oem/usr/lib
ROCKIT_LDLIBS := -lrockit -lrga -lrockchip_mpp -lstdc++ -lpthread -lrt -ldl -lm

.PHONY: all clean

all: v4l2_capture v4l2_mpp_encode

v4l2_capture: src/v4l2_capture.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

v4l2_mpp_encode: src/v4l2_mpp_encode.c src/mpp_encoder.c src/mpp_encoder.h src/v4l2_capture.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -Wno-pedantic \
		$(MPP_CPPFLAGS) $(ROCKIT_CPPFLAGS) $(LDFLAGS) \
		-o $@ src/v4l2_mpp_encode.c src/mpp_encoder.c \
		$(MPP_LDFLAGS) $(ROCKIT_LDFLAGS) $(ROCKIT_LDLIBS)

clean:
	rm -f v4l2_capture v4l2_mpp_encode
