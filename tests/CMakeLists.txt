add_executable(pf_tests
    TestMain.cpp
    TestDocument.cpp
    TestEditing.cpp
    TestRenderer.cpp
    TestPersistence.cpp
    TestDeserializationSafety.cpp
    TestSaveTransaction.cpp
    TestProjectPathBoundary.cpp
    TestLifecycleGuard.cpp
    TestBibliography.cpp
    TestBuild.cpp
    TestEndToEnd.cpp
    TestSessionAsync.cpp
    TestPreviewPersistence.cpp
    TestStructure.cpp
    TestInline.cpp
    TestMath.cpp
    TestRuntime.cpp
    TestCitationNumbering.cpp
)

target_include_directories(pf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_tests PRIVATE pf_app)
target_compile_definitions(pf_tests PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}"
                                  PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")

enable_testing()
add_test(NAME pf_tests COMMAND pf_tests)
# E-06: CTest classification so CI can select a subset (unit/integration/
# gui/runtime/sanitizer/...).
set_tests_properties(pf_tests PROPERTIES LABELS "unit;integration;runtime")

add_executable(pf_template_tests TestMain.cpp TestTemplates.cpp)
target_include_directories(pf_template_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_template_tests PRIVATE pf_app ZLIB::ZLIB)
target_compile_definitions(pf_template_tests PRIVATE PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME pf_template_tests COMMAND pf_template_tests)
set_tests_properties(pf_template_tests PROPERTIES LABELS "unit;integration;runtime;template;isolation" TIMEOUT 180)
