cmake_minimum_required(VERSION 3.13)

# -----------------------------------------------------------------------
# AJUSTE conforme sua placa:
#  - Esse projeto e para RP2350B (48 GPIO), nao para um modulo Pico 2
#    pronto — se sua placa for custom (chip RP2350B solto + flash +
#    PSRAM proprios), normalmente PICO_BOARD=pico2 ainda serve como
#    ponto de partida (define o RP2350 generico), mas confira o
#    tamanho de flash/PSRAM e os pinos de boot/flash no seu layout.
#  - Se voce ja tiver um arquivo de board customizado (board .h com os
#    pinos da sua placa), aponte PICO_BOARD_HEADER_DIRS pra ele.
# -----------------------------------------------------------------------
set(PICO_BOARD pico2 CACHE STRING "picoIDE")
set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PICO_BOARD pico_ide)

include(pico_sdk_import.cmake)
project(ide68k_pico C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()


add_executable(ide68k_pico
    main.c
    sd_driver.c
    ide_interface.c
)

target_compile_definitions(ide68k_pico PRIVATE PICO_RP2350A=0)

# Gera ide_pio.pio.h a partir de ide_pio.pio (usado por ide_interface.c)
pico_generate_pio_header(ide68k_pico ${CMAKE_CURRENT_LIST_DIR}/ide_pio.pio)

target_include_directories(ide68k_pico PRIVATE ${CMAKE_CURRENT_LIST_DIR})



target_link_libraries(ide68k_pico PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_irq

    hardware_clocks
    hardware_vreg        # necessário se for subir o clock acima do padrão
    hardware_spi 
    
    # hardware_psram          # so se voce migrar ide_hook_psram_buffer()
                               # em main.c pra usar essa lib (pico-sdk 2.3+)
)

# Roda o binario inteiro em SRAM (nao XIP), pra leitura/escrita na PSRAM
# nao competir com a flash pelo mesmo barramento QSPI.
pico_set_binary_type(ide68k_pico copy_to_ram)

# stdio so por USB (ajuste se voce usa a UART/DUART do seu sistema pra
# depuracao do lado do Pico tambem)
pico_enable_stdio_usb(ide68k_pico 1)
pico_enable_stdio_uart(ide68k_pico 0)

pico_add_extra_outputs(ide68k_pico)

add_custom_target(flash
    COMMAND $ENV{HOME}/.pico-sdk/picotool/2.3.0/bin/picotool load -f -x $<TARGET_FILE:ide68k_pico>
    DEPENDS ide68k_pico
    USES_TERMINAL
)