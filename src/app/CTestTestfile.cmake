# CMake generated Testfile for 
# Source directory: /home/orangepi3/Projects/STRTex/src/app
# Build directory: /home/orangepi3/Projects/STRTex/src/app
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(inline_editor "/home/orangepi3/Projects/STRTex/src/app/paperforge-inline-editor-test")
set_tests_properties(inline_editor PROPERTIES  ENVIRONMENT "QT_QPA_PLATFORM=offscreen" _BACKTRACE_TRIPLES "/home/orangepi3/Projects/STRTex/src/app/CMakeLists.txt;100;add_test;/home/orangepi3/Projects/STRTex/src/app/CMakeLists.txt;0;")
add_test(gui_input_persistence "/home/orangepi3/Projects/STRTex/src/app/paperforge-gui-persistence-test")
set_tests_properties(gui_input_persistence PROPERTIES  ENVIRONMENT "QT_QPA_PLATFORM=offscreen" _BACKTRACE_TRIPLES "/home/orangepi3/Projects/STRTex/src/app/CMakeLists.txt;110;add_test;/home/orangepi3/Projects/STRTex/src/app/CMakeLists.txt;0;")
