#include <stdio.h>
#include <stdlib.h>
#include <vga_video.h>
#include <fatfs/ff.h>
#include "drv_uart.h"
#include "drv_kbd.h"
#include "io.h"
#include "mc68000.h"
#include "color.h"
#include "timers.h"
#include "interrupt.h"
#include "ata.h"
#include "show_registers.h"
#include "sysflags.h"
#include "orion68.h"
#include "drv_ps2.h"
#include "kbd/ringbuffer.h"

// Aqui a memória é alocada de verdade!
SystemFlags sys_flags;

FATFS FatFs;      // Objeto de controle do sistema de arquivos (Work area)

#define LINELEN 128
char g_cmd_buffer[LINELEN];
extern int getline(char *line, int linesize);
extern void execute_cmd(char *linebuffer);

//__attribute__((section(".mram"))) char vbug_buffer[256];
//__attribute__((section(".minha_ram"))) int vbug_status_flag;
volatile __attribute__((section(".mram"))) long systemTick;
volatile __attribute__((section(".mram"))) unsigned int tick_count;
volatile __attribute__((section(".mram"))) unsigned int flg_system;

volatile uint32_t *last_mem_address = ( uint32_t *)0x80600UL;

#include "./tools/build_counter.h"

void dump_memory(void * addr,int size);
extern void xmodem_loader();
extern uint32_t get_system_tick_nmi_safe(void);
extern void listar_diretorio_raiz(void);
extern void pico_write_ch(uint8_t ch);
extern void UartWriteCh(uint8_t ch);

typedef void (*ProgramaXModem)(void);

void clr_flg_program_loaded(){
       flg_system &= 0xFE;
}
void set_flg_program_loaded(){
       flg_system |= 0x01;
}
unsigned int get_flg_program_loaded(){
       return flg_system & 0x01 ;
}
void print_msg(char *str){
        unsigned char x,y;
        y = 0x1D; //29
        x = 0;
        gotoxy(x,y); //padrao col,row
        printf("%s",str);
}

const char MsgOrionInit[] = 
    "\nPDS317-Hardware copyright (C) pdsilva(pgordao).\n"
    "bios68 V1.0.2 for m68k System.\n"
    "Build Date: " __DATE__ " - " __TIME__ "\n"
    "Build Counter: " BUILD_COUNTER "\n"
    "-----------------------------------------------\n\n";
extern volatile unsigned char debug_pkt;
 
extern void liga_debug(); 
extern int ata_detect(void);
extern int ata_init(void);
extern void abrir_arquivo();
extern void ler_e_exibir_joblog(char * filename);
extern void ler_comando(char *buffer) ;
extern void processar_comando(char *cmd_line);
extern int ata_read_identity(void);

char buffer[128];
char syspath[128];

void display_prompt(void)
{
    char drv;
    if (f_getcwd(syspath, sizeof(syspath)) == FR_OK) {
        // CurrVol armazena o número do drive atual (0, 1, 2...)
        // path armazena o caminho (que no seu caso está vindo apenas "/")
        drv = syspath[0];
        //printf("DRV: [%c]\n",drv);
        drv = drv+0x11;
        syspath[0]=drv;
        if( drv >= 'A' && drv <= 'I'){
                printf("%s>", syspath); 
        }else{
                printf("0:%s>", syspath); 
        }
    } else {
        printf("0:/> ");
    }
}

/*
f_mount(&FatFs0, "0:/", 0);
f_mount(&FatFs1, "1:/", 0);

Uso:
f_open(&file, "0:/arquivo1.txt", FA_READ);  // Usa o drive 0 
f_open(&file, "1:/arquivo2.txt", FA_READ);  // Usa o drive 1 
*/

void do_ideinit(int argc, char *argv[])
{
    FRESULT fr;
    fr = f_mount(&FatFs, "0:/", 0);
    if (fr != FR_OK) {
        printf("ERROR: Erro ao montar FAT No disks available ");
    }else{
        printf(": FAT success mounted! ");
    }
}

extern uint8_t rtc_init(void);
extern uint8_t ds3231_probe(void);
extern unsigned char ler_segundo();
extern uint8_t rtc_read_config_byte(uint16_t endereco);
extern void rtc_write_config_byte(uint16_t endereco, uint8_t valor);
extern void printString(char * str);
extern uint8_t ring_buf_get_char();
extern void duart_init_canal_a(void);
extern void picovga_putchar(char ch);
extern int dhcp_client(void);
extern void duart_a_init_38400(void);
extern void history_add(const char *cmd);
extern void readline_with_history(char *buf);

void main() {
    pico_write_ch('A');
    set_console_output(picovga_putchar);
    printf("%s",MsgOrionInit);
    printf("Memory: Rom start addr.............: 0\n");
    printf("        Rom installed  low and high: 65536 2 of 32768\n");
    printf("        Rom space end..............: 524287\n");
    printf("        First sram address.........: 524288\n");
    printf("        Last  sram address.........: %ld\n",*last_mem_address);
    printf("        CPU sram memory............: %ld words\n",*last_mem_address-0x80000);
    printf("        Total sram memory..........: %ld bytes\n",(*last_mem_address-0x80000)*2);
    pico_write_ch('c');
    m68k_enable_all_interrupts(); 
    printf("* - All Interrupts enabled.\n");
    pico_write_ch('D');
    printf("* - Console output seted to picoVGA.\n");
    pico_write_ch('E');
    set_console_input(ring_buf_get_char);
    printf("* - Console input seted to ps2 keyboard.\n");
    pico_write_ch('F');

    pico_write_ch('G');
    printf("* - Initializing:\n");
    printf("    * duart GPIO\n");
    duart_opr_init();
    pico_write_ch('H');
    printf("    * duart A\n");
    duart_a_init_38400();
    pico_write_ch('I');
   // printf("    * duart B\n");
   // duart_init_canal_a();
    
    crc32_init();
#ifdef DEBUG_ON
    ata_read_identity();    
#endif
    printf("\n    * IDE ");
    do_ideinit(0,NULL);
    pico_write_ch('J');

    printf("    * RTC: ");
    rtc_init();
    if(ds3231_probe()){
        printf("RTC-present\n");
    }else{
        printf("RTC- NOT FOUND...\n");
    }
    pico_write_ch('K');
    printf("    * Ethernet board: ");
    dhcp_client();

    pico_write_ch('L');

    ring_buf_init();
    //********************************************************
    //T H I S   M U S T   B E   T H E  L A S T    T H I N G 
    display_prompt();
    pico_write_ch('L');
    while (1){
        readline_with_history(g_cmd_buffer);
        history_add(g_cmd_buffer); 
        execute_cmd(g_cmd_buffer);
        display_prompt();
    }

}
