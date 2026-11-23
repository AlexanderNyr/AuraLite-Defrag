cmake_minimum_required(VERSION 3.25)
project(AuraLiteDefrag VERSION 1.0.0 LANGUAGES CXX)

include(cmake/CompilerOptions.cmake)

add_library(auralite_core STATIC
    src/core/error_catalog.cpp
    src/core/strings.cpp
    src/core/json.cpp
    src/core/logger.cpp
    src/core/config.cpp
    src/engine/policy_gate.cpp
    src/engine/journal.cpp
    src/engine/planner.cpp
    src/engine/online_mode.cpp
    src/engine/operation_controller.cpp
    src/backend/emulator/emulator_backend.cpp
    src/backend/emulator/scenario_loader.cpp
    src/reporting/cluster_aggregator.cpp
    src/reporting/statistics.cpp
    src/reporting/csv_exporter.cpp
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
    )
    target_link_libraries(AuraLiteDefrag PRIVATE
        auralite_core
        auralite_win32_platform
        d2d1 dwrite windowscodecs comctl32 uiautomationcore shlwapi
    )
    auralite_apply_compiler_options(AuraLiteDefrag)
endif()

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
add_test(NAME AuraLiteUnitAndScenarioSuite COMMAND auralite_tests)
