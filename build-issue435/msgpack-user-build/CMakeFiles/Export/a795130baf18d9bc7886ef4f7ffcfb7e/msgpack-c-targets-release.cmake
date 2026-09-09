#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "msgpack-c" for configuration "Release"
set_property(TARGET msgpack-c APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(msgpack-c PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmsgpack-c.so.2.0.0"
  IMPORTED_SONAME_RELEASE "libmsgpack-c.so.2"
  )

list(APPEND _cmake_import_check_targets msgpack-c )
list(APPEND _cmake_import_check_files_for_msgpack-c "${_IMPORT_PREFIX}/lib/libmsgpack-c.so.2.0.0" )

# Import target "msgpack-c-static" for configuration "Release"
set_property(TARGET msgpack-c-static APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(msgpack-c-static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmsgpack-c.a"
  )

list(APPEND _cmake_import_check_targets msgpack-c-static )
list(APPEND _cmake_import_check_files_for_msgpack-c-static "${_IMPORT_PREFIX}/lib/libmsgpack-c.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
