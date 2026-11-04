CC ?= cc
ARM_CC ?= /tmp/camera-armv6.aJ9j7F/arm-openipc-linux-musleabi_sdk-buildroot/bin/arm-openipc-linux-musleabi-gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
all: build/fixture
build:
	mkdir -p build
build/fixture: FORCE fixture.c preview.c mp4.c preview.h mp4.h | build
	$(CC) $(CFLAGS) -pthread fixture.c preview.c mp4.c -o $@
arm: | build
	$(ARM_CC) $(CFLAGS) -mcpu=arm1176jzf-s -mfloat-abi=soft -pthread -c preview.c -o build/preview.arm.o
	$(ARM_CC) $(CFLAGS) -mcpu=arm1176jzf-s -mfloat-abi=soft -c mp4.c -o build/mp4.arm.o
test: all
	python3 tests/test_preview.py
.PHONY: all arm test FORCE
FORCE:
