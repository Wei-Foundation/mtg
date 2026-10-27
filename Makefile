# SPDX-License-Identifier: GPL-3.0-only
# Module Target Generator (MTG) 构建脚本。

CXX      ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -std=c++26 -O3
LDFLAGS  ?=

SRC := src/main.cpp

ifeq ($(OS),Windows_NT)
TARGET  := mtg.exe
RM      := del /Q
ERRNULL := 2>nul
RUN     := $(TARGET)
else
TARGET  := mtg
RM      := rm -f
ERRNULL := 2>/dev/null
RUN     := ./$(TARGET)
endif

.PHONY: all check clean

all: $(TARGET)

$(TARGET): $(SRC) Makefile
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

check: $(TARGET)
	$(RUN) --version

clean:
	-@$(RM) mtg $(ERRNULL)
	-@$(RM) mtg.exe $(ERRNULL)
