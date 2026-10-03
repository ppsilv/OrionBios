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
#include "hardware/vreg.h"

#include "ide_config.h"
#include "ide_interface.h"



#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "hardware/regs/xip.h"
#include "hardware/structs/xip.h"

#define DELAY 500 // in microseconds
#define TEST_SIZE (1024*64)

#define SYSINFO_BASE   0x40000000UL
#define CHIP_ID_REG    (*(volatile uint32_t *)(SYSINFO_BASE + 0x00))

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

static int get_chip_id(void)
{
    uint32_t id = CHIP_ID_REG;

    uint32_t revision     = (id >> 28) & 0xF;
    uint32_t part         = (id >> 12) & 0xFFFF;
    uint32_t manufacturer = id & 0xFFF;

    printf("CHIP_ID bruto = 0x%08X\n", (unsigned int) id);
    printf("REVISION      = %u\n", (unsigned int) revision);
    printf("PART          = 0x%04X\n", (unsigned int) part);
    printf("MANUFACTURER  = 0x%03X\n", (unsigned int) manufacturer);

    return 0;
}

void runTest( int *p, size_t len){
	uint64_t start = to_us_since_boot (get_absolute_time());
	for (int i=0; i < len; i++){
		p[i] = i;
		if (p[i] != i){
			printf("RAM Write failed\n");
			return;
		}
	}

	int total =0;
	for (int i=0; i < len; i++){
		total = p[i] ;
	}
	uint64_t end = to_us_since_boot (get_absolute_time());
	uint64_t ms = end - start;
	printf("Completed in %llu us\n", ms);
}
#include "hardware/dma.h"

static uint32_t sram_buf[16384];   // 64 KB

void test_dma_psram(void) {
    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    uint64_t t0 = time_us_64();
    dma_channel_configure(ch, &c, sram_buf, (void *)0x11000000, 16384, true);
    dma_channel_wait_for_finish_blocking(ch);
    uint64_t us = time_us_64() - t0;

    printf("64 KB em %llu us = %.1f MB/s\n", (unsigned long long)us, 65536.0 / us);
    dma_channel_unclaim(ch);
}
#include "hardware/structs/qmi.h"

int main(void) {
    stdio_init_all();

    // 1. Aumenta a tensão do núcleo para suportar o overclock (ex: VREG_VOLTAGE_1_20V ou 1_30V)
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(2); // Dá um tempo para a tensão estabilizar
    set_sys_clock_khz(250000, true);
    sleep_ms(2500);

    for (int i = 0; i < 50 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(500);
    printf("build: %s %s\n", __DATE__, __TIME__);

    qmi_hw->m[1].wfmt = 0x00001208;   // prefixo 1 bit, endereço e dados em 4 bits
    qmi_hw->m[1].wcmd = 0x0000a038;

    printf("M1 timing=%08x rfmt=%08x rcmd=%08x wfmt=%08x wcmd=%08x\n",
        qmi_hw->m[1].timing, qmi_hw->m[1].rfmt, qmi_hw->m[1].rcmd,
        qmi_hw->m[1].wfmt, qmi_hw->m[1].wcmd);


    gpio_set_function(47,GPIO_FUNC_XIP_CS1); // Diz para o sistema que a PSRAM está em GPIO47
    xip_ctrl_hw->ctrl|=XIP_CTRL_WRITABLE_M1_BITS;
    xip_cache_clean_all();     // grava as linhas sujas na PSRAM

    if (!your_sd_driver_init()) {
        printf("Erro de inicializacao do sd_driver...\n");
        fatal_blink();
    }
    printf("picoIDE running 250 MHZ...\n");
    get_chip_id();
    printf("main: Calling ide_interface_init()\n");
    ide_interface_init();
    multicore_launch_core1(ide_interface_core1_entry);
    printf("main: multicore_launch_core1 installed\n");
    // core0 livre daqui pra frente — resto do seu firmware (rede, console
    // de depuracao etc.) pode entrar aqui. As ISRs de barramento ja estao
    // registradas e ativas.
    printf("main: core0 while(true)\n");
    printf("Versao.: 2.0.0\n");
    while (true) {
        tight_loop_contents();
    }

    return 0;
}

#ifdef BUCETON
int main(void) {
    stdio_init_all();

    // 1. Aumenta a tensão do núcleo para suportar o overclock (ex: VREG_VOLTAGE_1_20V ou 1_30V)
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(2); // Dá um tempo para a tensão estabilizar
    set_sys_clock_khz(250000, true);
    sleep_ms(2500);
stdio_init_all();
for (int i = 0; i < 50 && !stdio_usb_connected(); i++) sleep_ms(100);
sleep_ms(500);
printf("build: %s %s\n", __DATE__, __TIME__);

   qmi_hw->m[1].wfmt = 0x00001208;   // prefixo 1 bit, endereço e dados em 4 bits
   qmi_hw->m[1].wcmd = 0x0000a038;

printf("M1 timing=%08x rfmt=%08x rcmd=%08x wfmt=%08x wcmd=%08x\n",
       qmi_hw->m[1].timing, qmi_hw->m[1].rfmt, qmi_hw->m[1].rcmd,
       qmi_hw->m[1].wfmt, qmi_hw->m[1].wcmd);


gpio_set_function(47,GPIO_FUNC_XIP_CS1); // Diz para o sistema que a PSRAM está em GPIO47
xip_ctrl_hw->ctrl|=XIP_CTRL_WRITABLE_M1_BITS;

//	    printf("RAM Test\n");
//	    int * ram = (int *)malloc(sizeof(int) * TEST_SIZE );
//	    runTest(ram, TEST_SIZE);
//
	    printf("PSRAM Test\n");
	    int * psram = (int *)0x11000000;
	    runTest(psram, TEST_SIZE);


xip_cache_clean_all();     // grava as linhas sujas na PSRAM

test_dma_psram();
	    while (true) {
	        //status_led_set_state(true);
	        sleep_ms(DELAY);
	        //status_led_set_state(false);
	        sleep_ms(DELAY);
	        printf(".");
	    }





    if (!your_sd_driver_init()) {
        printf("Erro de inicializacao do sd_driver...\n");
        fatal_blink();
    }
    printf("picoIDE running 150MHZ...\n");
    get_chip_id();
    printf("main: Calling ide_interface_init()\n");
    ide_interface_init();
    multicore_launch_core1(ide_interface_core1_entry);
    printf("main: multicore_launch_core1 installed\n");
    // core0 livre daqui pra frente — resto do seu firmware (rede, console
    // de depuracao etc.) pode entrar aqui. As ISRs de barramento ja estao
    // registradas e ativas.
    printf("main: core0 while(true)\n");
    while (true) {
        tight_loop_contents();
    }

    return 0;
}



#include "pico/stdlib.h"
#include <stdio.h>
#include "pico_ide.h"

int main(void) {
    stdio_init_all();
    const uint LED = 25;   // ajuste para o pino do seu LED
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    while (true) {
        gpio_put(LED, 1); sleep_ms(100);
        gpio_put(LED, 0); sleep_ms(900);
        printf("vivo\n");
    }
}
#endif
