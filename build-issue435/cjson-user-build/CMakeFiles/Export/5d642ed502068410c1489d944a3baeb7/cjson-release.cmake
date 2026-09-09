#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "cjson" for configuration "Release"
set_property(TARGET cjson APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(cjson PROPERTIES
  IMPORTED_LOCATION_RELEASE "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435/cjson-user-install/lib/libcjson.so.1.7.18"
  IMPORTED_SONAME_RELEASE "libcjson.so.1"
  )

list(APPEND _cmake_import_check_targets cjson )
list(APPEND _cmake_import_check_files_for_cjson "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435/cjson-user-install/lib/libcjson.so.1.7.18" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
