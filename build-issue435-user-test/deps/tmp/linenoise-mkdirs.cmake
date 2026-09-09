# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src/linenoise"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src/linenoise-build"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/tmp"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src/linenoise-stamp"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src/linenoise-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src/linenoise-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/kavia/workspace/code-generation/rbus-10000000330/build-issue435-user-test/deps/src/linenoise-stamp${cfgdir}") # cfgdir has leading slash
endif()
