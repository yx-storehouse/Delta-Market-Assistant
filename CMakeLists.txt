cmake_minimum_required(VERSION 3.22)
project(RelinkStudio VERSION 0.6.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Sql)
if(MSVC)
  add_compile_options(/utf-8 /W4)
else()
  add_compile_options(-Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
qt_add_executable(RelinkStudio WIN32
  src/main.cpp src/mainwindow.cpp src/mainwindow.h
  src/application/startup_config.cpp src/application/startup_config.h
  src/diagnostics/ui_self_test_runner.cpp src/diagnostics/ui_self_test_runner.h
  src/application/runtime/replay_controller.cpp src/application/runtime/replay_controller.h
  src/application/runtime/ui_projection.cpp src/application/runtime/ui_projection.h
  src/ledger/storage_self_test.cpp src/ledger/storage_self_test.h
  src/application/workspace/workspace_ui_self_test.cpp src/application/workspace/workspace_ui_self_test.h
  resources.qrc)
target_include_directories(RelinkStudio PRIVATE src)
target_link_libraries(RelinkStudio PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
# Pages own their controls and publish UI intents. This library and its tests
# deliberately have no workspace controller, runtime or SQLite link dependency.
add_library(relink_ui STATIC
  src/domain.cpp src/domain.h src/widgets.cpp src/widgets.h
  src/fluenttheme.cpp src/fluenttheme.h
  src/ui/presentation/ui_helpers.cpp src/ui/presentation/ui_helpers.h
  src/ui/pages/task_page.cpp src/ui/pages/task_page.h
  src/ui/pages/run_settings_page.cpp src/ui/pages/run_settings_page.h
  src/ui/pages/workspace_records_panel.cpp src/ui/pages/workspace_records_panel.h
  src/ui/dialogs/task_editor_dialog.cpp src/ui/dialogs/task_editor_dialog.h)
target_include_directories(relink_ui PUBLIC src)
target_link_libraries(relink_ui PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)
target_link_libraries(RelinkStudio PRIVATE relink_ui)
if(WIN32)
  # Explorer and the taskbar show the same neutral icon the window paints.
  enable_language(RC)
  target_sources(RelinkStudio PRIVATE src/app.rc)
  target_link_libraries(RelinkStudio PRIVATE dwmapi)
  target_link_libraries(relink_ui PRIVATE dwmapi)
endif()
add_executable(domain_tests tests/domain_tests.cpp src/domain.cpp src/domain.h)
target_include_directories(domain_tests PRIVATE src)
target_link_libraries(domain_tests PRIVATE Qt6::Core)

add_library(relink_business STATIC
  src/business/value_types.cpp src/business/value_types.h
  src/business/models.cpp src/business/models.h
  src/business/validator.cpp src/business/validator.h
  src/business/matcher.cpp src/business/matcher.h
  src/config/v1_adapter.cpp src/config/v1_adapter.h src/config/import_types.h
  src/config/profile_store.cpp src/config/profile_store.h)
target_include_directories(relink_business PUBLIC src)
target_link_libraries(relink_business PUBLIC Qt6::Core)

add_executable(business_tests tests/business/business_tests.cpp)
target_include_directories(business_tests PRIVATE src)
target_link_libraries(business_tests PRIVATE relink_business Qt6::Core)

add_executable(business_value_tests tests/business/value_tests.cpp)
target_include_directories(business_value_tests PRIVATE src)
target_link_libraries(business_value_tests PRIVATE relink_business Qt6::Core)

add_executable(business_rule_tests tests/business/rule_tests.cpp)
target_include_directories(business_rule_tests PRIVATE src)
target_link_libraries(business_rule_tests PRIVATE relink_business Qt6::Core)

add_executable(business_migration_tests tests/business/migration_tests.cpp)
target_include_directories(business_migration_tests PRIVATE src)
target_link_libraries(business_migration_tests PRIVATE relink_business Qt6::Core)

add_executable(profile_store_tests tests/business/profile_store_tests.cpp)
target_include_directories(profile_store_tests PRIVATE src)
target_link_libraries(profile_store_tests PRIVATE relink_business Qt6::Core)

add_library(relink_runtime STATIC
  src/application/runtime/runtime.cpp src/application/runtime/runtime.h
  src/application/runtime/observation/observation_adapter.cpp
  src/application/runtime/observation/observation_adapter.h
  src/application/vision/worker_protocol.cpp src/application/vision/worker_protocol.h
  src/application/vision/lease_pool.cpp src/application/vision/lease_pool.h)
target_include_directories(relink_runtime PUBLIC src)
target_link_libraries(relink_runtime PUBLIC Qt6::Core)
target_link_libraries(RelinkStudio PRIVATE relink_runtime)
target_link_libraries(RelinkStudio PRIVATE relink_business)

# Windows transport is independent of the replay/domain/UI layers. The fixture
# worker is a test executable, never copied into the desktop release directory.
add_library(relink_vision_transport STATIC
  src/application/vision/shared_frame_memory.cpp src/application/vision/shared_frame_memory.h
  src/application/vision/worker_process.cpp src/application/vision/worker_process.h)
target_include_directories(relink_vision_transport PUBLIC src)
target_link_libraries(relink_vision_transport PUBLIC relink_runtime Qt6::Core)
add_executable(shared_frame_memory_tests tests/runtime/shared_frame_memory_tests.cpp)
target_link_libraries(shared_frame_memory_tests PRIVATE relink_vision_transport)
add_executable(vision_worker_fixture tests/runtime/vision_worker_fixture.cpp)
target_link_libraries(vision_worker_fixture PRIVATE relink_vision_transport)
add_executable(worker_process_tests tests/runtime/worker_process_tests.cpp)
target_link_libraries(worker_process_tests PRIVATE relink_vision_transport)
add_dependencies(worker_process_tests vision_worker_fixture)

add_executable(runtime_tests tests/runtime/replay_tests.cpp)
target_include_directories(runtime_tests PRIVATE src)
target_link_libraries(runtime_tests PRIVATE relink_runtime Qt6::Core)

add_executable(observation_adapter_tests tests/runtime/observation_adapter_tests.cpp)
add_executable(worker_protocol_tests tests/runtime/worker_protocol_tests.cpp)
target_include_directories(worker_protocol_tests PRIVATE src)
target_link_libraries(worker_protocol_tests PRIVATE relink_runtime Qt6::Core)
add_executable(vision_pipeline_tests tests/runtime/vision_pipeline_tests.cpp)
target_include_directories(vision_pipeline_tests PRIVATE src)
target_link_libraries(vision_pipeline_tests PRIVATE relink_runtime Qt6::Core)
target_include_directories(observation_adapter_tests PRIVATE src)
target_link_libraries(observation_adapter_tests PRIVATE relink_runtime Qt6::Core)

# PR07 read-only import preview projection checks.  Keep the adapter and
# projection under test without constructing the visible application window.
add_executable(ui_projection_tests
  tests/runtime/ui_projection_tests.cpp
  src/application/runtime/ui_projection.cpp
  src/application/runtime/ui_projection.h)
target_include_directories(ui_projection_tests PRIVATE src)
target_link_libraries(ui_projection_tests PRIVATE relink_business relink_runtime Qt6::Core)

add_library(relink_ledger STATIC
  src/ledger/store_types.h
  src/ledger/in_memory_event_store.h
  src/ledger/in_memory_event_store.cpp)
target_include_directories(relink_ledger PUBLIC src)
target_link_libraries(relink_ledger PUBLIC Qt6::Core)

add_library(relink_sqlite STATIC
  src/ledger/sqlite_event_store.cpp src/ledger/sqlite_event_store.h)
target_include_directories(relink_sqlite PUBLIC src)
target_link_libraries(relink_sqlite PUBLIC Qt6::Core Qt6::Sql)
target_link_libraries(RelinkStudio PRIVATE relink_sqlite)

add_library(relink_workspace STATIC
  src/application/workspace/workspace_controller.cpp
  src/application/workspace/workspace_controller.h
  src/application/workspace/workspace_projection.h
  src/application/workspace/profile_catalog.cpp
  src/application/workspace/profile_catalog.h
  src/application/workspace/replay_scenario.cpp
  src/application/workspace/replay_scenario.h
  src/application/workspace/history_query_service.cpp
  src/application/workspace/history_query_service.h
  src/application/workspace/records_exporter.cpp
  src/application/workspace/records_exporter.h)
target_include_directories(relink_workspace PUBLIC src)
target_link_libraries(relink_workspace PUBLIC relink_business relink_runtime relink_sqlite Qt6::Core)
target_link_libraries(RelinkStudio PRIVATE relink_workspace)

add_executable(workspace_tests tests/application/workspace_tests.cpp)
target_link_libraries(workspace_tests PRIVATE relink_workspace)

add_executable(ui_module_tests tests/ui/ui_module_tests.cpp
  src/application/startup_config.cpp src/application/startup_config.h resources.qrc)
target_link_libraries(ui_module_tests PRIVATE relink_ui)

add_executable(sqlite_ledger_tests tests/ledger/sqlite_ledger_tests.cpp)
target_link_libraries(sqlite_ledger_tests PRIVATE relink_sqlite relink_ledger)

add_executable(ledger_tests tests/ledger/ledger_tests.cpp)
target_include_directories(ledger_tests PRIVATE src)
target_link_libraries(ledger_tests PRIVATE relink_ledger Qt6::Core)

enable_testing()
add_test(NAME domain_tests COMMAND domain_tests)
add_test(NAME business_tests COMMAND business_tests)
add_test(NAME business_value_tests COMMAND business_value_tests)
add_test(NAME business_rule_tests COMMAND business_rule_tests)
add_test(NAME business_migration_tests COMMAND business_migration_tests)
add_test(NAME profile_store_tests COMMAND profile_store_tests)
add_test(NAME runtime_tests COMMAND runtime_tests "${CMAKE_SOURCE_DIR}/docs/business_rebuild/implementation/runtime/fixtures/replay_cases.json")
add_test(NAME observation_adapter_tests COMMAND observation_adapter_tests)
add_test(NAME worker_protocol_tests COMMAND worker_protocol_tests "${CMAKE_SOURCE_DIR}/docs/business_rebuild/implementation/runtime/fixtures/messages")
add_test(NAME vision_pipeline_tests COMMAND vision_pipeline_tests "${CMAKE_SOURCE_DIR}/docs/business_rebuild/implementation/runtime/fixtures/messages")
add_test(NAME shared_frame_memory_tests COMMAND shared_frame_memory_tests)
add_test(NAME worker_process_tests COMMAND worker_process_tests "$<TARGET_FILE:vision_worker_fixture>" "${CMAKE_SOURCE_DIR}/docs/business_rebuild/implementation/runtime/fixtures/messages")
set_tests_properties(worker_process_tests PROPERTIES TIMEOUT 45)
add_test(NAME ui_projection_tests COMMAND ui_projection_tests)
add_test(NAME ledger_tests COMMAND ledger_tests)
add_test(NAME sqlite_ledger_tests COMMAND sqlite_ledger_tests)
add_test(NAME workspace_tests COMMAND workspace_tests)
add_test(NAME ui_module_tests COMMAND ui_module_tests)
set_tests_properties(ui_module_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1" TIMEOUT 60)
set_tests_properties(workspace_tests PROPERTIES TIMEOUT 120)
add_test(NAME storage_self_test COMMAND RelinkStudio --storage-self-test)
set_tests_properties(sqlite_ledger_tests PROPERTIES TIMEOUT 120)
set_tests_properties(storage_self_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1")
add_test(NAME ui_offscreen COMMAND RelinkStudio --self-test --snapshot-dir "${CMAKE_BINARY_DIR}/snapshots" --config "${CMAKE_BINARY_DIR}/selftest-config.json")
set_tests_properties(ui_offscreen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1")
