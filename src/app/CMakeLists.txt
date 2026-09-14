# Qt6 GUI — PaperForge desktop editor.
set(CMAKE_AUTOMOC ON)

add_executable(paperforge-gui
    main.cpp
    MainWindow.cpp
    ProjectController.cpp
    BlockEditor.cpp
    PopupList.cpp
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
    OutlinePanel.cpp
    ProblemsPanel.cpp
    WelcomePage.cpp
)
set_property(TARGET paperforge-ui-verify PROPERTY AUTOMOC ON)
target_include_directories(paperforge-ui-verify PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paperforge-ui-verify PRIVATE pf_app Qt6::Widgets)
target_compile_definitions(paperforge-ui-verify PRIVATE PF_TECTONIC_BIN="${TECTONIC_BIN}")
