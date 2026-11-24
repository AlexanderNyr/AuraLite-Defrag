cmake_minimum_required(VERSION 3.25)
project(AuraLiteDefrag VERSION 1.0.0 LANGUAGES CXX)

# The resource script is a separate "language". Without RC enabled, CMake treats
# src/app/app.rc as an unknown source and never invokes rc.exe: the GUI would
# link without its manifest (no requireAdministrator, no PerMonitorV2 DPI) and
# without any string table. Enabled only on Windows because no other generator
# has an RC rule.
if(WIN32)
    enable_language(RC)
endif()

include(cmake/CompilerOptions.cmake)

add_library(auralite_core STATIC
    src/core/error_catalog.cpp
    src/core/strings.cpp
    src/core/json.cpp
    src/core/logger.cpp
    src/core/config.cpp
    src/core/exclusions.cpp
    src/engine/policy_gate.cpp
    src/engine/journal.cpp
    src/engine/planner.cpp
    src/engine/online_mode.cpp
    src/engine/operation_controller.cpp
    src/backend/emulator/emulator_backend.cpp
    src/backend/emulator/scenario_loader.cpp
    src/backend/usn/usn_parser.cpp
    src/reporting/cluster_aggregator.cpp
    src/reporting/statistics.cpp
    src/reporting/csv_exporter.cpp
    src/reporting/chart_data_model.cpp
    src/reporting/csv_reader.cpp
    src/differential/differential_oracle.cpp
    src/differential/golden_trace.cpp
)
target_include_directories(auralite_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
auralite_apply_compiler_options(auralite_core)

if(WIN32)
    add_library(auralite_win32_platform STATIC
        src/platform/win32_platform.cpp
        src/backend/ntfs/media_detector.cpp
        src/backend/ntfs/ntfs_backend.cpp
        src/scheduler/task_scheduler.cpp
        src/shell/context_menu.cpp
    )
    target_include_directories(auralite_win32_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(auralite_win32_platform PUBLIC auralite_core taskschd ole32 oleaut32 shell32 advapi32)
    auralite_apply_compiler_options(auralite_win32_platform)
endif()

add_executable(auralite-cli
    src/cli/cli_app.cpp
    src/cli/main.cpp
)
target_link_libraries(auralite-cli PRIVATE auralite_core)
if(WIN32)
    target_link_libraries(auralite-cli PRIVATE auralite_win32_platform)
endif()
auralite_apply_compiler_options(auralite-cli)

if(WIN32)
    add_executable(AuraLiteDefrag WIN32
        src/ui/theme.cpp
        src/ui/d2d_renderer.cpp
        src/ui/wic_exporter.cpp
        src/ui/main_window.cpp
        src/app/win_main.cpp
        src/app/app.rc
    )
    target_include_directories(AuraLiteDefrag PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/res/strings
        ${CMAKE_CURRENT_SOURCE_DIR}   # app.rc includes res/strings/* from the root
    )
    target_link_libraries(AuraLiteDefrag PRIVATE
        auralite_core
        auralite_win32_platform
        d2d1 dwrite windowscodecs comctl32 uiautomationcore shlwapi
    )
    # The target is added with the WIN32 keyword, which selects /SUBSYSTEM:WINDOWS.
    # In a Unicode build the linker otherwise looks for WinMain() and fails with
    # LNK2019 unresolved external symbol WinMain, because only wWinMain exists.
    if(MSVC)
        target_link_options(AuraLiteDefrag PRIVATE /ENTRY:wWinMainCRTStartup)
        # The manifest is embedded by app.rc, and the linker must not embed its
        # own. Three distinct failures came from getting this wrong:
        #
        #   both sources active  -> CVTRES CVT1100: duplicate resource
        #                           MANIFEST, then LNK1123
        #   linker alone         -> an empty manifest, so no requireAdministrator
        #                           and no PerMonitorV2
        #   linker + /MANIFESTINPUT -> mt.exe merges the two and rejects them:
        #                           c1010 "Values of attribute level not equal",
        #                           then LNK1327
        #
        # /MANIFEST:NO removes the linker's manifest entirely, so app.rc's
        # RT_MANIFEST is the single source and mt.exe is never invoked. That is
        # how the project was originally meant to build; the missing piece was
        # only this option.
        target_link_options(AuraLiteDefrag PRIVATE /MANIFEST:NO)
        set_property(TARGET AuraLiteDefrag APPEND PROPERTY
                     LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/app.manifest")
    endif()
    auralite_apply_compiler_options(AuraLiteDefrag)
endif()

# The crash campaign spawns children through fork/mkfifo and kills them with
# _exit(). It is a POSIX CI tool, not part of the product: a Windows build that
# compiled these targets would fail on #error in crash_child.cpp, so it is
# explicitly excluded rather than silently broken.
if(NOT WIN32)
    add_executable(auralite-crash-child
        tests/crash/crash_child.cpp
    )
    target_link_libraries(auralite-crash-child PRIVATE auralite_core)
    target_include_directories(auralite-crash-child PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    auralite_apply_compiler_options(auralite-crash-child)

    add_executable(auralite-crash-campaign
        tests/crash/crash_campaign.cpp
    )
    # The campaign includes both "tests/crash/crash_channel.hpp" (root-relative)
    # and product headers (src-relative), so it needs both include roots.
    target_include_directories(auralite-crash-campaign PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    auralite_apply_compiler_options(auralite-crash-campaign)
    # The campaign execs the child binary, so it needs the real path: the build
    # directory name belongs to the generator, not to the source tree.
    add_test(NAME AuraLiteCrashCampaign
             COMMAND auralite-crash-campaign --trials 40 --child $<TARGET_FILE:auralite-crash-child>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

add_executable(auralite-usn-tests
    tests/unit/test_usn_parser.cpp
)
target_link_libraries(auralite-usn-tests PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-usn-tests)

add_executable(auralite-fuzz
    tools/fuzz_parsers.cpp
)
target_link_libraries(auralite-fuzz PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-fuzz)

add_executable(auralite-exclusions-tests
    tests/unit/test_exclusions.cpp
)
target_link_libraries(auralite-exclusions-tests PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-exclusions-tests)

add_executable(auralite-reporting-tests
    tests/unit/test_reporting.cpp
)
target_link_libraries(auralite-reporting-tests PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-reporting-tests)

add_executable(auralite-bench-reporting
    tools/bench_reporting.cpp
)
target_link_libraries(auralite-bench-reporting PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-bench-reporting)

# P9-T04: the budget and regression harness (PT-01...PT-08, PT-MEM).
add_executable(auralite-bench-perf
    tools/bench_perf.cpp
)
target_link_libraries(auralite-bench-perf PRIVATE auralite_core)
if(WIN32)
    target_link_libraries(auralite-bench-perf PRIVATE psapi)
endif()
auralite_apply_compiler_options(auralite-bench-perf)

# P9-T07: renders a scenario to SVG for the user guide.
add_executable(auralite-map
    tools/map_render.cpp
)
target_link_libraries(auralite-map PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-map)

add_executable(auralite-chart-model-tests
    tests/unit/test_chart_model.cpp
)
target_link_libraries(auralite-chart-model-tests PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-chart-model-tests)

add_executable(auralite-differential-tests
    tests/differential/test_oracle.cpp
)
target_link_libraries(auralite-differential-tests PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-differential-tests)

add_executable(auralite-golden
    tools/golden_traces.cpp
)
target_link_libraries(auralite-golden PRIVATE auralite_core)
auralite_apply_compiler_options(auralite-golden)

enable_testing()
add_executable(auralite_tests
    src/cli/cli_app.cpp
    src/ui/theme.cpp
    tests/unit/test_main.cpp
)
target_link_libraries(auralite_tests PRIVATE auralite_core)
if(WIN32)
    target_link_libraries(auralite_tests PRIVATE auralite_win32_platform)
else()
    target_sources(auralite_tests PRIVATE src/scheduler/task_scheduler.cpp)
endif()
auralite_apply_compiler_options(auralite_tests)

# The suites load tests/scenarios/*.json and tests/golden/* relative to the
# working directory, so ctest must run them from the source root. Running them
# from the build tree made every scenario-driven case fail with "checked >= 15"
# and aborted the reporting suite on a missing src/ directory.
set(AURALITE_TEST_CWD ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME AuraLiteUnitAndScenarioSuite COMMAND auralite_tests
         WORKING_DIRECTORY ${AURALITE_TEST_CWD})
add_test(NAME AuraLiteUsnParserSuite COMMAND auralite-usn-tests
         WORKING_DIRECTORY ${AURALITE_TEST_CWD})
add_test(NAME AuraLiteExclusionsSuite COMMAND auralite-exclusions-tests
         WORKING_DIRECTORY ${AURALITE_TEST_CWD})
add_test(NAME AuraLiteReportingSuite COMMAND auralite-reporting-tests
         WORKING_DIRECTORY ${AURALITE_TEST_CWD})
add_test(NAME AuraLiteChartDataModelSuite COMMAND auralite-chart-model-tests
         WORKING_DIRECTORY ${AURALITE_TEST_CWD})
add_test(NAME AuraLiteDifferentialOracleSuite COMMAND auralite-differential-tests
         WORKING_DIRECTORY ${AURALITE_TEST_CWD})
# Golden corpus must not drift: regenerate with `auralite-golden --write`
# and commit the diff together with the intentional behaviour change.
add_test(NAME AuraLiteGoldenCorpus COMMAND auralite-golden --verify
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
