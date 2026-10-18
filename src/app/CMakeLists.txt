# Qt6 GUI — PaperForge desktop editor.
#
# The GUI components are compiled once into an OBJECT library and linked into
# every GUI executable (app, smoke test, screenshot tool and the four widget
# regression tests). Before this, the same 16-file list was repeated per
# target, which meant 85 compilations where 16 suffice.
set(CMAKE_AUTOMOC ON)

add_library(pf_gui_objects OBJECT
    InlineEditor.cpp
    InlineContentEditorDialog.cpp
    InsertTableDialog.cpp
    AssetConverters.cpp
    FigurePreviewRenderer.cpp
    AddReferenceDialog.cpp
    TemplateImportDialog.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
    PdfPreview.cpp
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
    MainWindow.cpp
    MathPreviewRenderer.cpp
    math/MathRenderService.cpp
    InlineMathObjectRenderer.cpp
    CitationObjectRenderer.cpp
    MathEditorDialog.cpp
    MathPaletteWidget.cpp
    MathCommandCompleter.cpp
)
set_target_properties(pf_gui_objects PROPERTIES AUTOMOC ON)
target_include_directories(pf_gui_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(pf_gui_objects PUBLIC
    pf_app
    Qt6::Widgets
)
target_compile_definitions(pf_gui_objects PUBLIC PF_INSTALL_ROOT="${CMAKE_SOURCE_DIR}")

# The Q_OBJECT classes all live in pf_gui_objects. Consumers only add their own
# main/test translation units, so AUTOMOC is switched off there to avoid
# generating duplicate meta-object definitions for those same classes.
function(paperforge_gui_executable target)
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES AUTOMOC OFF)
    target_link_libraries(${target} PRIVATE pf_gui_objects)
endfunction()

# Desktop application.
paperforge_gui_executable(paperforge-gui main.cpp)

# GUI end-to-end smoke test (offscreen-capable).
paperforge_gui_executable(paperforge-gui-smoke GuiSmokeTest.cpp)
add_test(NAME gui_smoke COMMAND paperforge-gui-smoke)
set_tests_properties(gui_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "gui")

# UI verification tool (screenshots of real window states).
paperforge_gui_executable(paperforge-ui-verify UiVerify.cpp)
set_target_properties(paperforge-ui-verify PROPERTIES
    PROJECT_LABEL "tool: screenshot verification")

# Regression test: input typed into a row must survive idle time (no periodic
# rebuild wiping the editor).
paperforge_gui_executable(paperforge-gui-persistence-test EditorPersistenceTest.cpp)
target_include_directories(paperforge-gui-persistence-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(paperforge-gui-persistence-test PRIVATE Qt6::Test)
add_test(NAME gui_input_persistence COMMAND paperforge-gui-persistence-test)
set_tests_properties(gui_input_persistence PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "gui;integration")

# Rich inline editor tests (widget level, so they need a QApplication).
paperforge_gui_executable(paperforge-inline-editor-test
    InlineEditorEditorTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlineEditor.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlinePdf.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlineToolbar.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestMathPreview.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestMathPalette.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestInlineMathGui.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestCaptionTableGui.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestFigureGui.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestCitationGui.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestProblemsPanel.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestMathRenderService.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestProjectPreviewGui.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestReferenceEntryGui.cpp
    ${CMAKE_SOURCE_DIR}/tests/TestTemplateImportGui.cpp
)
target_include_directories(paperforge-inline-editor-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(paperforge-inline-editor-test PRIVATE Qt6::Test)
add_test(NAME inline_editor COMMAND paperforge-inline-editor-test)
set_tests_properties(inline_editor PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "gui;sanitizer")

# P0-05 regression: every block kind must have a GUI projection at every
# heading level (figures/tables below a Section used to render nowhere).
paperforge_gui_executable(paperforge-block-projection-test BlockProjectionTest.cpp)
target_include_directories(paperforge-block-projection-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME block_projection COMMAND paperforge-block-projection-test)
set_tests_properties(block_projection PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    LABELS "gui")
