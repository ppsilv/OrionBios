# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/pdsilva/.pico-sdk/sdk/2.3.0/tools/pioasm"
  "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pioasm"
  "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pioasm-install"
  "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/tmp"
  "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
  "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src"
  "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/pdsilva/Projects/Orion/OrionBios/peripheral/RP2350B/ide_interface_completo/build/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
