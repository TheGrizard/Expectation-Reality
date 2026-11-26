# SPDX-License-Identifier: MPL-2.0
# desktop renderer: make -C app/tools, then app/tools/render ... (fm1-base in base/, or set FM1_BASE)
CFLAGS = $(WERROR) -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wdouble-promotion -Wshadow -ffp-contract=off -I$(FM1_BASE)/include
FM1_BASE ?= ../../base
SRC = ../dsp/drums.c ../dsp/groove.c ../dsp/engine.c ../dsp/fx.c ../dsp/dsp_init.c
render: ../dsp/*.c ../dsp/*.h render.c
	$(CC) $(CFLAGS) -o render render.c $(SRC) -lm
clean-render:
	rm -f render
.PHONY: clean-render
