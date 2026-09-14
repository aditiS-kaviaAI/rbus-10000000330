# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src/gtest"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src/gtest-build"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/tmp"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src/gtest-stamp"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src"
  "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src/gtest-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src/gtest-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/kavia/workspace/code-generation/rbus-10000000330/build-diagnostics/unittests/gtest/src/gtest-stamp${cfgdir}") # cfgdir has leading slash
endif()
