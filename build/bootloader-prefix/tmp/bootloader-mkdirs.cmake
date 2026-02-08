# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/wyp/esp/esp-idf/components/bootloader/subproject"
  "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader"
  "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix"
  "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix/tmp"
  "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix/src/bootloader-stamp"
  "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix/src"
  "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/wyp/esp/stem_framework_idf61_lcd/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
