# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
#   make -C tools/overhead     build ../../build/overhead/delayacct_frames
OUTPUT := ../../build/overhead
CC     ?= gcc
CFLAGS := -g -O2 -Wall -Wextra

$(OUTPUT)/delayacct_frames: delayacct_frames.c | $(OUTPUT)
	$(CC) $(CFLAGS) $< -o $@

$(OUTPUT):
	mkdir -p $@

.PHONY: clean
clean:
	rm -f $(OUTPUT)/delayacct_frames
