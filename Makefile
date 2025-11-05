PROJECT_NAME := Demo

SRC_DIR := src
BUILD_DIR := build
INCLUDE_DIR := include

GLFW_INCLUDE_DIR := /opt/homebrew/opt/glfw/include
GLFW_LIB_DIR := /opt/homebrew/opt/glfw/lib
CGLM_INCLUDE_DIR := /opt/homebrew/opt/cglm/include/cglm
SOKOL_INCLUDE_DIR := /Users/andres/sokol/include  

CXX := g++
CC := gcc
CXXFLAGS := -Wall -Wextra -I$(INCLUDE_DIR) -I$(SRC_DIR) -I$(GLFW_INCLUDE_DIR) -I$(SOKOL_INCLUDE_DIR)
CFLAGS := -Wall -Wextra -I$(INCLUDE_DIR) -I$(SRC_DIR) -I$(GLFW_INCLUDE_DIR) -I$(SOKOL_INCLUDE_DIR)
LDFLAGS := -L$(GLFW_LIB_DIR) -lglfw -ldl -framework OpenGL -framework Cocoa

# Find all C and C++ source files recursively
C_SRCS := $(shell find $(SRC_DIR) -name '*.c')
CPP_SRCS := $(shell find $(SRC_DIR) -name '*.cpp')

# Generate object file names
C_OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))
CPP_OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(CPP_SRCS))
OBJS := $(C_OBJS) $(CPP_OBJS)

all: $(BUILD_DIR)/$(PROJECT_NAME)

$(BUILD_DIR)/$(PROJECT_NAME): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(OBJS) -o $(BUILD_DIR)/$(PROJECT_NAME) $(LDFLAGS)

# Rule for C files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for C++ files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

exec: $(BUILD_DIR)/$(PROJECT_NAME)
	./$(BUILD_DIR)/$(PROJECT_NAME)

.PHONY: all clean exec