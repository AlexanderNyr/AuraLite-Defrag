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
	src/core/exclusions.cpp \
	src/engine/policy_gate.cpp \
	src/engine/journal.cpp \
	src/engine/planner.cpp \
	src/engine/online_mode.cpp \
	src/engine/operation_controller.cpp \
	src/backend/emulator/emulator_backend.cpp \
	src/backend/emulator/scenario_loader.cpp \
	src/backend/usn/usn_parser.cpp \
	src/reporting/cluster_aggregator.cpp \
	src/reporting/statistics.cpp \
	src/reporting/csv_exporter.cpp \
	src/reporting/chart_data_model.cpp \
	src/reporting/csv_reader.cpp \
	src/differential/differential_oracle.cpp \
	src/differential/golden_trace.cpp

CLI_SRCS := \
	src/cli/cli_app.cpp \
	src/cli/main.cpp

TEST_SRCS := \
	src/cli/cli_app.cpp \
	src/scheduler/task_scheduler.cpp \
	src/ui/theme.cpp \
	tests/unit/test_main.cpp

BUILD_DIR := build_linux

.PHONY: all cli test charts reporting exclusions usn fuzz fuzz-quick crash-campaign sbom audit-negative bench bench-reported perf perf-gate perf-baseline differential golden golden-verify maps docs-check docs-negative clean audit

all: cli test charts reporting exclusions usn bench differential

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

cli: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) $(CLI_SRCS) -pthread -o $(BUILD_DIR)/auralite-cli

test: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) $(TEST_SRCS) -pthread -o $(BUILD_DIR)/auralite-tests
	./$(BUILD_DIR)/auralite-tests

charts: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tests/unit/test_chart_model.cpp -pthread -o $(BUILD_DIR)/auralite-charts-tests
	./$(BUILD_DIR)/auralite-charts-tests

exclusions: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tests/unit/test_exclusions.cpp -pthread -o $(BUILD_DIR)/auralite-exclusions-tests
	./$(BUILD_DIR)/auralite-exclusions-tests

usn: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tests/unit/test_usn_parser.cpp -pthread -o $(BUILD_DIR)/auralite-usn-tests
	./$(BUILD_DIR)/auralite-usn-tests

# P9-T01 acceptance: 1,000,000 executions per target under ASan/UBSan.
fuzz: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tools/fuzz_parsers.cpp -pthread -o $(BUILD_DIR)/auralite-fuzz
	./$(BUILD_DIR)/auralite-fuzz --iterations 1000000

# P9-T03: 200 abrupt-termination trials across 5 crash points. Requires a POSIX
# host; the child binary is killed with _exit() so buffered writes really are lost.
crash-campaign: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) -I. tests/crash/crash_child.cpp -pthread -o $(BUILD_DIR)/auralite-crash-child
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) -I. tests/crash/crash_campaign.cpp -pthread -o $(BUILD_DIR)/auralite-crash-campaign
	./$(BUILD_DIR)/auralite-crash-campaign --trials 40

fuzz-quick: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tools/fuzz_parsers.cpp -pthread -o $(BUILD_DIR)/auralite-fuzz
	./$(BUILD_DIR)/auralite-fuzz --quick

reporting: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tests/unit/test_reporting.cpp -pthread -o $(BUILD_DIR)/auralite-reporting-tests
	./$(BUILD_DIR)/auralite-reporting-tests

# Release-build timings for the P7-T05/P7-T06 budgets. ASan inflates these ~10x,
# so the plan's numbers must be measured without the sanitizer.
bench-reported: $(BUILD_DIR)
	$(CXX) -std=c++20 -Wall -Wextra -Werror -Wpedantic -O2 -Isrc $(CORE_SRCS) tools/bench_reporting.cpp -pthread -o $(BUILD_DIR)/auralite-bench-reporting
	./$(BUILD_DIR)/auralite-bench-reporting

bench: bench-reported

differential: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(CORE_SRCS) tests/differential/test_oracle.cpp -pthread -o $(BUILD_DIR)/auralite-differential
	./$(BUILD_DIR)/auralite-differential

maps: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) tools/map_render.cpp -pthread -o $(BUILD_DIR)/auralite-map
	mkdir -p docs/user/images
	./$(BUILD_DIR)/auralite-map --scenario tests/scenarios/SC-01.json --mode analyze --out docs/user/images/SC-01-before.svg --title "SC-01 before analysis — 86.9% of allocated clusters are fragmented"
	./$(BUILD_DIR)/auralite-map --scenario tests/scenarios/SC-01.json --mode defrag --when after --out docs/user/images/SC-01-after.svg --title "SC-01 after defragmentation — every file contiguous, 0% fragmented"

golden: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) tools/golden_traces.cpp -pthread -o $(BUILD_DIR)/auralite-golden
	./$(BUILD_DIR)/auralite-golden --write

golden-verify: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) tools/golden_traces.cpp -pthread -o $(BUILD_DIR)/auralite-golden
	./$(BUILD_DIR)/auralite-golden --verify

# P9-T08: regenerate the SBOM from the tree. `git diff --exit-code` makes a stale
# committed SBOM a build failure rather than a silent drift.
sbom:
	python3 tools/gen_sbom.py
	git diff --exit-code sbom.json

# Negative tests: prove the licence audit is not vacuous.
PERF_SRCS := $(CORE_SRCS)

perf: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) tools/bench_perf.cpp -pthread -o $(BUILD_DIR)/auralite-bench-perf
	./$(BUILD_DIR)/auralite-bench-perf

# Two different claims, deliberately kept apart.
#
#   make perf       Budgets are absolute ("4M aggregation <= 50 ms") and the
#                   measured values sit 20-30x under them, so this is stable on
#                   any host. It is enforced and it runs in CI.
#
#   make perf-gate  Regressions are relative (+20% vs baseline) and that only
#                   means something on a host that measures the same code the
#                   same way twice. This sandbox is 2 shared cores: PT-06b was
#                   observed at 2.97, 4.46 and 5.06 ms across identical runs
#                   (70% spread), so a 20% gate here reports the scheduler, not
#                   the code. It therefore runs in *informational* mode by
#                   default and is enforced only where the host can support it.
#
# The gate mechanism itself was verified to work: an artificial 2x slowdown in
# the aggregation paint loop produced +2104% and a non-zero exit.
THRESHOLD ?= 20
ENFORCE ?= 0
perf-gate: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) tools/bench_perf.cpp -pthread -o $(BUILD_DIR)/auralite-bench-perf
	./$(BUILD_DIR)/auralite-bench-perf --reps 5 --threshold $(THRESHOLD) --baseline perf/baseline.txt --enforce $(ENFORCE)

perf-baseline: $(BUILD_DIR)
	@echo "Regenerating the performance baseline -- only do this deliberately."
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) tools/bench_perf.cpp -pthread -o $(BUILD_DIR)/auralite-bench-perf
	./$(BUILD_DIR)/auralite-bench-perf --reps 5 --save-baseline perf/baseline.txt

docs-check:
	python3 tools/docs_check.py
	python3 tools/gen_traceability.py

docs-negative:
	python3 tools/test_docs_negative.py

audit-negative:
	python3 tools/test_audit_negative.py

audit:
	python3 tools/check_layers.py
	python3 tools/check_strings.py
	python3 tools/check_licences.py

clean:
	rm -rf $(BUILD_DIR)
