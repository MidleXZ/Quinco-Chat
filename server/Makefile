CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -O2
BUILD_DIR ?= build
EXE_SUFFIX ?=
LDLIBS ?=
ifeq ($(OS),Windows_NT)
EXE_SUFFIX := .exe
LDLIBS += -lws2_32 -lbcrypt
endif
SERVER_BIN := $(BUILD_DIR)/quinco-chat-server$(EXE_SUFFIX)
TEST_BIN := $(BUILD_DIR)/self_test$(EXE_SUFFIX)
PROTOCOL_TEST_BIN := $(BUILD_DIR)/protocol_test$(EXE_SUFFIX)
SERVER_SOURCES := $(wildcard src/*.cpp)
TEST_SOURCES := tests/self_test.cpp src/crypto.cpp src/json.cpp src/ws.cpp
PROTOCOL_TEST_SOURCES := tests/protocol_test.cpp src/crypto.cpp src/json.cpp src/hub.cpp src/protocol.cpp src/store.cpp src/util.cpp

.PHONY: all run test clean

all: $(SERVER_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SERVER_BIN): $(SERVER_SOURCES) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(SERVER_SOURCES) $(LDLIBS)

$(TEST_BIN): $(TEST_SOURCES) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SOURCES) $(LDLIBS)

$(PROTOCOL_TEST_BIN): $(PROTOCOL_TEST_SOURCES) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(PROTOCOL_TEST_SOURCES) $(LDLIBS)

run: $(SERVER_BIN)
	$(SERVER_BIN)

test: $(TEST_BIN) $(PROTOCOL_TEST_BIN)
	$(TEST_BIN)
	$(PROTOCOL_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)