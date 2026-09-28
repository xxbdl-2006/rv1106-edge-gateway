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

# Sensor sources are deliberately NOT part of MEDIA_SRC: nothing in the video
# pipeline references them yet, so adding them here would only enlarge the
# board binary without changing behaviour.
#
# src/i2c_bitbang.c is host safe: it touches nothing but struct i2c_gpio_ops,
# which is exactly why the timing can be unit tested with a recording fake.
# src/gpio_sysfs.c and src/mpu6050_i2c.c are Linux only and compile to nothing
# elsewhere, which is what lets the test targets list them unconditionally.
SENSOR_SRC := src/mpu6050.c src/mpu6050_i2c.c src/i2c_bitbang.c src/gpio_sysfs.c

.PHONY: all clean test host-syntax mpu6050-probe
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

# Only src/mpu6050.c is compiled here; src/mpu6050_i2c.c and src/gpio_sysfs.c
# are #ifdef __linux__ and would contribute nothing. The decoding maths is the
# part worth testing and the part that is transport independent, which is the
# whole reason the driver is split up.
test-mpu6050: tests/test_mpu6050.c src/mpu6050.c src/mpu6050.h
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_mpu6050.c src/mpu6050.c -lm

# The I2C timing, driven through a recording fake gpio backend. No sysfs, no
# filesystem, no sleeps: this asserts on the actual edge sequence, which is the
# only way to catch a master that produces plausible but wrong waveforms.
test-i2c-bitbang: tests/test_i2c_bitbang.c src/i2c_bitbang.c src/i2c_bitbang.h \
	src/mpu6050_gpio.h src/mpu6050.c src/mpu6050.h
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_i2c_bitbang.c \
		src/i2c_bitbang.c src/mpu6050.c -lm

# The sensor data plane: source interface, ring, attitude solver, mock source.
# Nothing here needs Linux or a board, which is the point of the layering - the
# OSD and alarm logic above this can be built and tested before the hardware is
# even powered. -lm is for the test's own fabsf, not for the code under test:
# the production files carry their own square root and trig precisely because
# the rootfs has no libm.
test-sensor: tests/test_sensor.c src/sensor_ring.c src/sensor_ring.h \
	src/sensor_source.h src/sensor_attitude.c src/sensor_attitude.h \
	src/sensor_math.h src/mock_sensor.c src/mock_sensor.h
	$(HOSTCC) $(TEST_CFLAGS) -o $@ tests/test_sensor.c \
		src/sensor_ring.c src/sensor_attitude.c src/mock_sensor.c \
		-lpthread -lm

test: test-packet-queue test-rtp-rtsp test-frame-ring test-capture-thread \
	test-mpu6050 test-i2c-bitbang test-sensor board-flags
	./test-packet-queue
	./test-rtp-rtsp
	./test-frame-ring
	./test-capture-thread
	./test-mpu6050
	./test-i2c-bitbang
	./test-sensor

# Includes every target that compiles a file from tools/, which is where the
# "living in another directory" problem comes from. Files under src/ resolve
# their quoted includes against their own directory and never need this.
TOOLS_CPPFLAGS := -Isrc

# Board side only: cross compiled, drives the gpio pins directly.
mpu6050-probe: tools/mpu6050-probe.c $(SENSOR_SRC) src/mpu6050.h
	$(CC) $(CPPFLAGS) $(TOOLS_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
		tools/mpu6050-probe.c $(SENSOR_SRC) $(LDLIBS)

# src/rtsp_server.c is the only file that needs POSIX sockets, which MinGW does
# not provide. src/v4l2_mpp_encode.c cannot be checked here at all because it
# pulls in linux/videodev2.h through v4l2_capture.c. On Windows these targets
# still catch syntax errors, typos and new warnings through tests/host-stubs.
# On Linux they are unnecessary but harmless.
#
# The last two define __linux__ so the Linux-only halves are compiled too,
# against the stubs in tests/host-stubs/linux. That is how src/mpu6050_i2c.c
# gets checked without a board; it already paid for itself by catching an
# EREMOTEIO that uclibc does not always export.
host-syntax: src/rtsp_server.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) -Itests/host-stubs \
		-include extra.h src/rtsp_server.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/capture_thread.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/frame_ring.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/mpu6050.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/i2c_bitbang.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/sensor_ring.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/sensor_attitude.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) src/mock_sensor.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) -Itests/host-stubs -D__linux__ \
		src/mpu6050_i2c.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) -Itests/host-stubs -D__linux__ \
		src/gpio_sysfs.c
	$(HOSTCC) -fsyntax-only $(TEST_CFLAGS) -Itests/host-stubs -D__linux__ \
		tools/mpu6050-probe.c

# Check the sensor sources the way the BOARD build compiles them, rather than
# with the test flags.
#
# The concrete bug this was written after: the board rule for mpu6050-probe
# needs TOOLS_CPPFLAGS for "mpu6050.h" to resolve, because the file lives in
# tools/ while the header lives in src/. The rule originally had no -I at all.
# It passed host-syntax and every test on Windows, then failed the first real
# cross compile with:
#
#     tools/mpu6050-probe.c:30:10: fatal error: mpu6050.h: No such file
#
# host-syntax could not catch it because that target checks with TEST_CFLAGS,
# which carries -Isrc. The check and the build disagreed about the flags, so
# the check was not checking the build.
#
# This target references TOOLS_CPPFLAGS and CFLAGS instead of spelling flags
# out again, so the two cannot drift apart. Verified by mutation: clearing the
# variable makes this target fail with the same fatal error the board hit,
# which is the whole point.
#
# No claim is made about catching -O2-only warnings. CFLAGS is used as-is for
# consistency, but -fsyntax-only does not run the optimisation passes that
# would produce them, so this target does not check for them.
#
# The video targets still cannot be checked on this side: they pull
# linux/videodev2.h and SDK headers that MinGW does not have.
.PHONY: board-flags
board-flags:
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(TOOLS_CPPFLAGS) $(CFLAGS) \
		-Itests/host-stubs -D__linux__ tools/mpu6050-probe.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) \
		-Itests/host-stubs -D__linux__ src/mpu6050_i2c.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) \
		-Itests/host-stubs -D__linux__ src/gpio_sysfs.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) src/mpu6050.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) src/i2c_bitbang.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) src/sensor_ring.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) src/sensor_attitude.c
	$(HOSTCC) -fsyntax-only $(CPPFLAGS) $(CFLAGS) src/mock_sensor.c

clean:
	rm -f v4l2_capture v4l2_mpp_encode mpu6050-probe test-packet-queue \
		test-rtp-rtsp test-frame-ring test-capture-thread test-mpu6050 \
		test-i2c-bitbang test-sensor
