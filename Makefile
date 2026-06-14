# Compiler and flags
# Use Apple's clang explicitly. PATH may have llvm-mos (a 6502/65816 cross
# compiler) which can't build host code; /usr/bin/clang++ is Apple's host clang.
CC = /usr/bin/clang++
CXXFLAGS = -Wall -Wextra -I./source
#CXXFLAGS = -std=c++17 -Wall -Wextra -I./source
LDFLAGS =

# Source and object files
SRC = ./source/c1_file.cpp ./source/c2_file.cpp ./source/gsla_file.cpp ./source/lzb.cpp ./source/main.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = gsla

# Build configurations
DEBUG_FLAGS = -g -D_DEBUG -D_CONSOLE
RELEASE_FLAGS = -O3 -DNDEBUG -D_CONSOLE

# Default build is Release
CONFIG ?= Release

ifeq ($(CONFIG),Debug)
  CXXFLAGS += $(DEBUG_FLAGS)
else ifeq ($(CONFIG),Release)
  CXXFLAGS += $(RELEASE_FLAGS)
else
  $(error Invalid CONFIG value. Use 'Debug' or 'Release')
endif

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJ)
	$(CC) $(CXXFLAGS) $(OBJ) $(LDFLAGS) -o $@

# Compile source files into object files
%.o: %.cpp
	$(CC) $(CXXFLAGS) -c $< -o $@

# Clean build files
clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
