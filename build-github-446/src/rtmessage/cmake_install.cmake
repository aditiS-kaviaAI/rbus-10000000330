# Install script for directory: /home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/librtMessage.so.2.14.0"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/librtMessage.so.0"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      file(RPATH_CHECK
           FILE "${file}"
           RPATH "")
    endif()
  endforeach()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES
    "/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage/librtMessage.so.2.14.0"
    "/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage/librtMessage.so.0"
    )
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/librtMessage.so.2.14.0"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/librtMessage.so.0"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      file(RPATH_CHANGE
           FILE "${file}"
           OLD_RPATH "/home/kavia/workspace/code-generation/rbus-10000000330:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/deps/src/linenoise:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446:"
           NEW_RPATH "")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage/librtMessage.so")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage/rtrouted")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted"
         OLD_RPATH "/home/kavia/workspace/code-generation/rbus-10000000330:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/deps/src/linenoise:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag"
         RPATH "")
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE EXECUTABLE FILES "/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage/rtrouted_diag")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag"
         OLD_RPATH "/home/kavia/workspace/code-generation/rbus-10000000330:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/deps/src/linenoise:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446:/home/kavia/workspace/code-generation/rbus-10000000330/build-github-446/src/rtmessage:"
         NEW_RPATH "")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/rtrouted_diag")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/rtmessage" TYPE FILE FILES
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtMessage.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtMessageHeader.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtError.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtConnection.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtVector.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtRetainable.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtLog.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtList.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtTime.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtString.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtAtomic.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtAdvisory.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtThreadPool.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtHashMap.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtMemory.h"
    "/home/kavia/workspace/code-generation/rbus-10000000330/src/rtmessage/rtm_discovery_api.h"
    )
endif()

