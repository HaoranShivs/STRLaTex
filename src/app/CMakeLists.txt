# Qt6 GUI — PaperForge desktop editor.
set(CMAKE_AUTOMOC ON)

add_executable(paperforge-gui
    main.cpp
    EditorItemKind.cpp
    InlineEditor.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
    MathPreviewRenderer.cpp
    MathEditorDialog.cpp
)

target_include_directories(paperforge-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(paperforge-gui PRIVATE
    pf_app
    Qt6::Widgets
)
target_compile_definitions(paperforge-gui PRIVATE PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")

# GUI end-to-end smoke test (offscreen-capable).
add_executable(paperforge-gui-smoke
    EditorItemKind.cpp
    InlineEditor.cpp
    GuiSmokeTest.cpp
    ProjectController.cpp
    MathPreviewRenderer.cpp
    MathEditorDialog.cpp
)
set_property(TARGET paperforge-gui-smoke PROPERTY AUTOMOC ON)
target_link_libraries(paperforge-gui-smoke PRIVATE pf_app Qt6::Core Qt6::Widgets)
target_compile_definitions(paperforge-gui-smoke PRIVATE PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")

# UI verification tool (screenshots of real window states).
add_executable(paperforge-ui-verify
    EditorItemKind.cpp
    InlineEditor.cpp
    UiVerify.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
    MathPreviewRenderer.cpp
    MathEditorDialog.cpp
)
set_property(TARGET paperforge-ui-verify PROPERTY AUTOMOC ON)
target_include_directories(paperforge-ui-verify PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paperforge-ui-verify PRIVATE pf_app Qt6::Widgets)
target_compile_definitions(paperforge-ui-verify PRIVATE PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")

# Regression test: input typed into a row must survive idle time (no periodic
# rebuild wiping the editor).
add_executable(paperforge-gui-persistence-test
    EditorItemKind.cpp
    InlineEditor.cpp
    EditorPersistenceTest.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
    MathPreviewRenderer.cpp
    MathEditorDialog.cpp
)
set_property(TARGET paperforge-gui-persistence-test PROPERTY AUTOMOC ON)

# Rich inline editor tests (widget level, so they need a QApplication).
add_executable(paperforge-inline-editor-test
    InlineEditorEditorTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlineEditor.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlinePdf.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlineToolbar.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestMathPreview.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlineMathGui.cpp
    EditorItemKind.cpp
    InlineEditor.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
    MathPreviewRenderer.cpp
    MathEditorDialog.cpp
)
set_property(TARGET paperforge-inline-editor-test PROPERTY AUTOMOC ON)
target_include_directories(paperforge-inline-editor-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(paperforge-inline-editor-test PRIVATE
    pf_app Qt6::Widgets)
target_compile_definitions(paperforge-inline-editor-test
    PRIVATE PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME inline_editor
         COMMAND paperforge-inline-editor-test)
set_tests_properties(inline_editor PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_include_directories(paperforge-gui-persistence-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paperforge-gui-persistence-test PRIVATE
    pf_app Qt6::Widgets Qt6::Test)
target_compile_definitions(paperforge-gui-persistence-test
    PRIVATE PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")
add_test(NAME gui_input_persistence
         COMMAND paperforge-gui-persistence-test)
set_tests_properties(gui_input_persistence PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
