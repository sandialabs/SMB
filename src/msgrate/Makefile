# -*- Makefile -*-
#
# Copyright 2006 Sandia Corporation. Under the terms of Contract
# DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
# retains certain rights in this software.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.

RELEASE_VERSION = 1.0

CC = mpicc
CPPFLAGS = -DVERSION=$(RELEASE_VERSION)
CFLAGS = -O3
LDFLAGS =
LDLIBS =

# Optional toggles:
#   make
#   make CUDA=1 [CUDA_HOME=<cuda install>]
#   make HIP=1  [ROCM_HOME=<rocm install>]
#
# If CUDA_HOME / ROCM_HOME are not given they are derived from the
# environment: CUDA_HOME from $CUDA_PATH or the location of nvcc on PATH;
# ROCM_HOME from $ROCM_PATH or `hipconfig --path`.

CUDA ?= 0
HIP  ?= 0

# Prevent enabling both at once
ifeq ($(CUDA),1)
ifeq ($(HIP),1)
$(error Enable only one of CUDA=1 or HIP=1, not both)
endif
endif

# CUDA build options
ifeq ($(CUDA),1)
CPPFLAGS += -DENABLE_CUDA
CUDA_HOME ?= $(CUDA_PATH)
ifeq ($(strip $(CUDA_HOME)),)
CUDA_HOME := $(patsubst %/bin/nvcc,%,$(shell command -v nvcc 2>/dev/null))
endif
ifeq ($(strip $(CUDA_HOME)),)
$(error CUDA=1 but CUDA install not found: set CUDA_HOME=<path> or CUDA_PATH, or put nvcc on PATH)
endif
CPPFLAGS += -I$(CUDA_HOME)/include
LDFLAGS  += -L$(CUDA_HOME)/lib64 -L$(CUDA_HOME)/lib
LDLIBS   += -lcudart
endif

# HIP build options
ifeq ($(HIP),1)
CPPFLAGS += -DENABLE_HIP -D__HIP_PLATFORM_AMD__
ROCM_HOME ?= $(ROCM_PATH)
ifeq ($(strip $(ROCM_HOME)),)
ROCM_HOME := $(shell hipconfig --path 2>/dev/null)
endif
ifeq ($(strip $(ROCM_HOME)),)
$(error HIP=1 but ROCm install not found: set ROCM_HOME=<path> or ROCM_PATH, or put hipconfig on PATH)
endif
CPPFLAGS += -I$(ROCM_HOME)/include
LDFLAGS  += -L$(ROCM_HOME)/lib -L$(ROCM_HOME)/lib64
LDLIBS   += -lamdhip64
endif

msgrate: msgrate.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

msgrate.o: msgrate.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f msgrate.o msgrate *~

dist:
	rm -rf msgrate-$(RELEASE_VERSION)
	mkdir msgrate-$(RELEASE_VERSION)
	(for file in msgrate.c README Makefile GPL.txt LICENSE.txt ; do \
	cp $$file msgrate-$(RELEASE_VERSION)/. ; \
	chmod 644 msgrate-$(RELEASE_VERSION)/$$file ; \
	done)
	tar czf msgrate-$(RELEASE_VERSION).tar.gz msgrate-$(RELEASE_VERSION)
	rm -rf msgrate-$(RELEASE_VERSION)
