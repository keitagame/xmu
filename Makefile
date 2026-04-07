# XMU Hypervisor Makefile

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -g \
          -D_GNU_SOURCE
LDFLAGS = -lm

SRCS = src/xmu_cpu.c \
       src/xmu_mem.c \
       src/xmu_dev.c \
       src/xmu_hv.c  \
       src/xmu_main.c

OBJS = $(SRCS:src/%.c=build/%.o)
TARGET = xmu

.PHONY: all clean test

all: build $(TARGET)

build:
	mkdir -p build

build/%.o: src/%.c include/xmu.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo ""
	@echo "  ✓ Built XMU Hypervisor → ./$(TARGET)"
	@echo ""

clean:
	rm -rf build $(TARGET)

test: $(TARGET)
	@echo "── Test 1: Real-mode hello ──────────────────"
	./$(TARGET) --demo --run
	@echo ""
	@echo "── Test 2: Long-mode transition ─────────────"
	./$(TARGET) --demo2 --run
	@echo ""
	@echo "── All tests passed ─────────────────────────"