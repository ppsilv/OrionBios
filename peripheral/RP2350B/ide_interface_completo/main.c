// main.c
//
// Ponto de entrada do firmware da interface IDE emulada.
//
//   core0: inicializa tudo, registra as ISRs de barramento (PIO/DMA) e
//          fica livre pro resto do seu firmware.
//   core1: fica só rodando ide_interface_core1_entry(), que faz as
//          chamadas bloqueantes de SD (via ide_interface_task()).
//
// TODO antes de compilar:
//   1) Implemente your_sd_driver_init/read_block/write_block em outro
//      arquivo (ex. sd_driver.c), ligando no seu cartão SD real.
//   2) Confira o header/nome exato de psram_or_malloc() na versão do
//      pico-sdk instalada — a lib hardware_psram é nova (pico-sdk 2.3+);
//      se sua versão for mais antiga, troque por chamadas do seu
//      próprio driver de PSRAM.

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "ide_config.h"
#include "ide_interface.h"

// -----------------------------------------------------------------------
// Buffers de estagio (IDE_NUM_BUFFERS x 512 bytes, definidos em
// ide_config.h). Por enquanto em SRAM comum (.bss) so pra compilar e
// testar a logica de barramento em qualquer versao do pico-sdk.
//
// PARA FICAR EM PSRAM DE VERDADE (o objetivo do projeto): troque o
// corpo de ide_hook_psram_buffer() abaixo pra apontar pro seu driver
// de PSRAM real. Duas opcoes comuns:
//   a) Se voce ja tem a PSRAM mapeada num endereco fixo (ex. via QMI,
//      alocado "a mao" numa regiao reservada no linker script), so
//      retorne ponteiros dentro dessa regiao.
//   b) Se preferir usar a lib hardware_psram do proprio pico-sdk, ela
//      so existe a partir da 2.3.0 — confirme sua versao com
//      `git -C $PICO_SDK_PATH describe --tags` (ou veja
//      $PICO_SDK_PATH/pico_sdk_version.cmake) antes de linkar ela nesse
//      projeto e usar psram_or_malloc()/psram_check_address().
// -----------------------------------------------------------------------
static uint8_t g_stage_buffers[IDE_NUM_BUFFERS][IDE_SECTOR_SIZE_BYTES];

uint8_t *ide_hook_psram_buffer(unsigned buffer_index) {
    return g_stage_buffers[buffer_index];
}

// -----------------------------------------------------------------------
// Ganchos de SD — implemente estas 3 funcoes em outro .c do seu projeto,
// ligando no seu driver de cartao SD real (SPI/SDIO). Aqui so declaramos
// o contrato que ide_interface.c espera.
// -----------------------------------------------------------------------
extern bool your_sd_driver_init(void);
extern bool your_sd_driver_read_block(uint32_t lba, uint8_t *dst512);
extern bool your_sd_driver_write_block(uint32_t lba, const uint8_t *src512);

bool ide_hook_sd_read_sector(uint32_t lba, uint8_t *dst512) {
    return your_sd_driver_read_block(lba, dst512);
}

bool ide_hook_sd_write_sector(uint32_t lba, const uint8_t *src512) {
    return your_sd_driver_write_block(lba, src512);
}

static void fatal_blink(void) {
    // erro de inicializacao (SD ou PSRAM): fica preso aqui em vez de
    // seguir com o m68k pensando que tem um disco funcional.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    while (true) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        sleep_ms(100);
    }
}

int main(void) {
    stdio_init_all();

    if (!your_sd_driver_init()) {
        fatal_blink();
    }

    ide_interface_init();
    multicore_launch_core1(ide_interface_core1_entry);

    // core0 livre daqui pra frente — resto do seu firmware (rede, console
    // de depuracao etc.) pode entrar aqui. As ISRs de barramento ja estao
    // registradas e ativas.
    while (true) {
        tight_loop_contents();
    }

    return 0;
}
