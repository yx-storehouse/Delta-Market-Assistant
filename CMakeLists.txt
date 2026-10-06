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
  src/main.cpp src/mainwindow.cpp src/mainwindow.h src/fluenttheme.cpp src/fluenttheme.h
  src/domain.cpp src/domain.h src/widgets.cpp src/widgets.h
  src/application/runtime/replay_controller.cpp src/application/runtime/replay_controller.h
  src/application/runtime/ui_projection.cpp src/application/runtime/ui_projection.h
  src/ledger/storage_self_test.cpp src/ledger/storage_self_test.h
  resources.qrc)
target_include_directories(RelinkStudio PRIVATE src)
target_link_libraries(RelinkStudio PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
if(WIN32)
  # Explorer and the taskbar show the same neutral icon the window paints.
  enable_language(RC)
  target_sources(RelinkStudio PRIVATE src/app.rc)
  target_link_libraries(RelinkStudio PRIVATE dwmapi)
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

add_library(relink_runtime STATIC src/application/runtime/runtime.cpp src/application/runtime/runtime.h)
target_include_directories(relink_runtime PUBLIC src)
target_link_libraries(relink_runtime PUBLIC Qt6::Core)
target_link_libraries(RelinkStudio PRIVATE relink_runtime)
target_link_libraries(RelinkStudio PRIVATE relink_business)

add_executable(runtime_tests tests/runtime/replay_tests.cpp)
target_include_directories(runtime_tests PRIVATE src)
target_link_libraries(runtime_tests PRIVATE relink_runtime Qt6::Core)

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
add_test(NAME ui_projection_tests COMMAND ui_projection_tests)
add_test(NAME ledger_tests COMMAND ledger_tests)
add_test(NAME sqlite_ledger_tests COMMAND sqlite_ledger_tests)
add_test(NAME storage_self_test COMMAND RelinkStudio --storage-self-test)
set_tests_properties(sqlite_ledger_tests PROPERTIES TIMEOUT 120)
set_tests_properties(storage_self_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1")
add_test(NAME ui_offscreen COMMAND RelinkStudio --self-test --snapshot-dir "${CMAKE_BINARY_DIR}/snapshots" --config "${CMAKE_BINARY_DIR}/selftest-config.json")
set_tests_properties(ui_offscreen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1")
