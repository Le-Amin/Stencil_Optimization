# Makefile du stencil optimisé
#
#   make            -> version optimisée OpenMP (float) pour la machine locale
#   make double     -> version optimisée OpenMP en double précision
#   make serial     -> version optimisée sans OpenMP (1 cœur)
#   make original   -> code d'origine, compilé comme dans le Makefile initial
#   make check      -> lance 1024x1024x100 et compare à la référence
#   make bench      -> compare original / optimisé sur 1024x1024x100
#
# Variables : CC=clang, ARCH=-march=znver3, OMPFLAGS=... peuvent être surchargées.
#   macOS (clang d'Apple sans OpenMP) : brew install libomp gcc puis make CC=gcc-14

ifeq ($(origin CC),default)
CC = gcc
endif
ARCH     ?= -march=native
OPTFLAGS ?= -O3 $(ARCH) -fno-math-errno $(VECFLAGS)

# Sur x86_64 (Intel/AMD avec AVX-512), autoriser les vecteurs 512 bits
# (gcc se limite à 256 bits par défaut). Sans effet sur une machine AVX2.
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
VECFLAGS ?= -mprefer-vector-width=512
endif
OMPFLAGS ?= -fopenmp
CFLAGS   ?= -std=c99 -Wall -Wextra
PYTHON   ?= python3

all: stencil

stencil: stencil.c
	$(CC) $(CFLAGS) $(OPTFLAGS) $(OMPFLAGS) $< -o $@

double: stencil.c
	$(CC) $(CFLAGS) $(OPTFLAGS) $(OMPFLAGS) -DUSE_DOUBLE $< -o stencil_double

serial: stencil.c
	$(CC) $(CFLAGS) -Wno-unknown-pragmas $(OPTFLAGS) $< -o stencil_serial

original: original/stencil_original.c
	$(CC) -std=c99 -Wall $< -o stencil_original

check: stencil
	./stencil 1024 1024 100
	$(PYTHON) check.py --ref-stencil-file stencil_1024_1024_100.pgm --stencil-file stencil.pgm

bench: stencil original
	./stencil_original 1024 1024 100
	./stencil 1024 1024 100
	$(PYTHON) check.py --ref-stencil-file stencil_1024_1024_100.pgm --stencil-file stencil.pgm

clean:
	rm -f stencil stencil_double stencil_serial stencil_original stencil.pgm
	rm -rf run_orig_*

.PHONY: all double serial original check bench clean
