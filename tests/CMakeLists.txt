add_executable(pf_tests
    TestMain.cpp
    TestDocument.cpp
    TestEditing.cpp
    TestRenderer.cpp
    TestPersistence.cpp
    TestBibliography.cpp
    TestBuild.cpp
    TestEndToEnd.cpp
)

target_include_directories(pf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pf_tests PRIVATE pf_app)
target_compile_definitions(pf_tests PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}")

enable_testing()
add_test(NAME pf_tests COMMAND pf_tests)
