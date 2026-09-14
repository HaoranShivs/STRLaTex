# Qt6 GUI — PaperForge desktop editor.
set(CMAKE_AUTOMOC ON)

add_executable(paperforge-gui
    main.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
)

target_include_directories(paperforge-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(paperforge-gui PRIVATE
    pf_app
    Qt6::Widgets
)
target_compile_definitions(paperforge-gui PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}")

# GUI end-to-end smoke test (offscreen-capable).
add_executable(paperforge-gui-smoke
    GuiSmokeTest.cpp
    ProjectController.cpp
)
set_property(TARGET paperforge-gui-smoke PROPERTY AUTOMOC ON)
target_link_libraries(paperforge-gui-smoke PRIVATE pf_app Qt6::Core Qt6::Widgets)
target_compile_definitions(paperforge-gui-smoke PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}")

# UI verification tool (screenshots of real window states).
add_executable(paperforge-ui-verify
    UiVerify.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
)
set_property(TARGET paperforge-ui-verify PROPERTY AUTOMOC ON)
target_include_directories(paperforge-ui-verify PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paperforge-ui-verify PRIVATE pf_app Qt6::Widgets)
target_compile_definitions(paperforge-ui-verify PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}")

# Regression test: input typed into a row must survive idle time (no periodic
# rebuild wiping the editor).
add_executable(paperforge-gui-persistence-test
    EditorPersistenceTest.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
)
set_property(TARGET paperforge-gui-persistence-test PROPERTY AUTOMOC ON)
target_include_directories(paperforge-gui-persistence-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paperforge-gui-persistence-test PRIVATE
    pf_app Qt6::Widgets Qt6::Test)
target_compile_definitions(paperforge-gui-persistence-test
    PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}")
add_test(NAME gui_input_persistence
         COMMAND paperforge-gui-persistence-test)
set_tests_properties(gui_input_persistence PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
