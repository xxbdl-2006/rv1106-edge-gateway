CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
HOSTCC ?= gcc

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

MEDIA_SRC := src/mpp_encoder.c src/sink_file.c src/sink_queue.c \
	src/packet_queue.c src/h264_util.c \
	src/rtp_h264.c src/rtsp_proto.c src/rtsp_server.c \
	src/frame_ring.c src/capture_thread.c

.PHONY: all clean test host-syntax

all: v4l2_capture v4l2_mpp_encode

v4l2_capture: src/v4l2_capture.c src/capture_signal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

v4l2_mpp_encode: src/v4l2_mpp_encode.c src/mpp_encoder.c src/mpp_encoder.h src/v4l2_capture.c \
	src/packet_sink.h src/packet_queue.h src/packet_queue.c \
	src/sink_file.h src/sink_file.c src/sink_queue.h src/sink_queue.c \
	src/h264_util.h src/h264_util.c \
	src/frame_ring.h src/frame_ring.c src/capture_thread.h src/capture_thread.c \
	src/capture_signal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -Wno-pedantic \
		$(MPP_CPPFLAGS) $(ROCKIT_CPPFLAGS) $(LDFLAGS) \
		-o $@ src/v4l2_mpp_encode.c $(MEDIA_SRC) \
		$(MPP_LDFLAGS) $(ROCKIT_LDFLAGS) $(ROCKIT_LDLIBS)

TEST_CFLAGS := -O1 -g -Wall -Wextra -Wpedantic -std=gnu11 -Isrc \
	-D__USE_MINGW_ANSI_STDIO=1

test-packet-queue: tests/test_packet_queue.c $(MEDIA_SRC) src/packet_sink.h
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_packet_queue.c \
		src/packet_queue.c src/h264_util.c src/sink_file.c src/sink_queue.c \
		-lpthread

test-rtp-rtsp: tests/test_rtp_rtsp.c src/rtp_h264.c src/rtp_h264.h \
	src/rtsp_proto.c src/rtsp_proto.h src/h264_util.c
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_rtp_rtsp.c \
		src/rtp_h264.c src/rtsp_proto.c src/h264_util.c

test-frame-ring: tests/test_frame_ring.c src/frame_ring.c src/frame_ring.h
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_frame_ring.c \
		src/frame_ring.c -lpthread

# src/capture_thread.c has no V4L2 dependency on purpose, so it builds natively
# even though its only real user is the board program.
test-capture-thread: tests/test_capture_thread.c src/capture_thread.c \
	src/capture_thread.h src/frame_ring.c src/frame_ring.h src/capture_signal.h
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_capture_thread.c \
		src/capture_thread.c src/frame_ring.c -lpthread

test: test-packet-queue test-rtp-rtsp test-frame-ring test-capture-thread
	./test-packet-queue
	./test-rtp-rtsp
	./test-frame-ring
	./test-capture-thread

# src/rtsp_server.c is the only file that needs POSIX sockets, which MinGW does
# not provide. src/v4l2_mpp_encode.c cannot be checked here at all because it
# pulls in linux/videodev2.h through v4l2_capture.c. On Windows these targets
# still catch syntax errors, typos and new warnings through tests/host-stubs.
# On Linux they are unnecessary but harmless.
host-syntax: src/rtsp_server.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) -Itests/host-stubs \
		-include extra.h $<
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/capture_thread.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/frame_ring.c

clean:
	rm -f v4l2_capture v4l2_mpp_encode test-packet-queue test-rtp-rtsp \
		test-frame-ring test-capture-thread
