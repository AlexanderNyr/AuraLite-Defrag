# AuraLite-Defrag Portable/Linux Build & Verification Makefile
CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -Wpedantic -O2 -Isrc
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g

CORE_SRCS := \
	src/core/error_catalog.cpp \
	src/core/strings.cpp \
	src/core/json.cpp \
	src/core/logger.cpp \
	src/core/config.cpp \
	src/engine/policy_gate.cpp \
	src/engine/journal.cpp \
	src/engine/planner.cpp \
	src/engine/online_mode.cpp \
	src/engine/operation_controller.cpp \
	src/backend/emulator/emulator_backend.cpp \
	src/backend/emulator/scenario_loader.cpp \
	src/reporting/cluster_aggregator.cpp \
	src/reporting/statistics.cpp \
	src/reporting/csv_exporter.cpp

CLI_SRCS := \
	src/cli/cli_app.cpp \
	src/cli/main.cpp

TEST_SRCS := \
	src/cli/cli_app.cpp \
	src/scheduler/task_scheduler.cpp \
	src/ui/theme.cpp \
	tests/unit/test_main.cpp

BUILD_DIR := build_linux

.PHONY: all cli test clean audit

all: cli test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

cli: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) $(CLI_SRCS) -pthread -o $(BUILD_DIR)/auralite-cli

test: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) $(TEST_SRCS) -pthread -o $(BUILD_DIR)/auralite-tests
	./$(BUILD_DIR)/auralite-tests

audit:
	python3 tools/check_layers.py
	python3 tools/check_strings.py
	python3 tools/check_licences.py

clean:
	rm -rf $(BUILD_DIR)
