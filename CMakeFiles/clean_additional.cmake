# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "RelWithDebInfo")
  file(REMOVE_RECURSE
  "src/app/CMakeFiles/paperforge-gui-persistence-test_autogen.dir/AutogenUsed.txt"
  "src/app/CMakeFiles/paperforge-gui-persistence-test_autogen.dir/ParseCache.txt"
  "src/app/CMakeFiles/paperforge-gui-smoke_autogen.dir/AutogenUsed.txt"
  "src/app/CMakeFiles/paperforge-gui-smoke_autogen.dir/ParseCache.txt"
  "src/app/CMakeFiles/paperforge-gui_autogen.dir/AutogenUsed.txt"
  "src/app/CMakeFiles/paperforge-gui_autogen.dir/ParseCache.txt"
  "src/app/CMakeFiles/paperforge-inline-editor-test_autogen.dir/AutogenUsed.txt"
  "src/app/CMakeFiles/paperforge-inline-editor-test_autogen.dir/ParseCache.txt"
  "src/app/CMakeFiles/paperforge-ui-verify_autogen.dir/AutogenUsed.txt"
  "src/app/CMakeFiles/paperforge-ui-verify_autogen.dir/ParseCache.txt"
  "src/app/paperforge-gui-persistence-test_autogen"
  "src/app/paperforge-gui-smoke_autogen"
  "src/app/paperforge-gui_autogen"
  "src/app/paperforge-inline-editor-test_autogen"
  "src/app/paperforge-ui-verify_autogen"
  )
endif()
