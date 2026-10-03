/* ide_test.c (v2) - teste da interface IDE emulada (RP2350B) no m68k
 *
 * LEIA ANTES DE RODAR
 *   A PIO nao tem timeout de /DTACK: se algo estiver errado no lado do
 *   Pico, o m68k TRAVA no acesso em vez de receber um erro. Por isso
 *   cada acesso ao hardware e precedido de uma mensagem, e a ULTIMA
 *   linha impressa antes de um travamento mostra onde ele ficou preso.
 *   con_putchar() deve retornar so depois de o caractere ter sido enviado
 *   (espere o TX esvaziar), senao a ultima mensagem pode se perder.
 *
 * Etapas (da mais simples para a mais completa):
 *   1) STATUS parado          -> esperado 0x40 (DRDY), estavel
 *   2) loopback de registros  -> valida decode de ESCRITA do task file
 *   3) READ SECTORS (1 setor) -> hexdump, assinatura 55AA, tabela MBR,
 *                                e uma segunda leitura para comparar
 *
 * Limitacoes conhecidas do firmware (ver analise): leitura de mais de
 * 1 setor ainda trava e o Pico so usa 24 bits de LBA; por isso aqui
 * sempre 1 setor e LBA < 2^24. Sem IDENTIFY DEVICE.
Com esses dois arquivos, a hipótese do alinhamento dos bits se confirma, e achei outros problemas que impediriam o teste de funcionar, mesmo antes do SD entrar. Li o código sem rodar nada, então confirme tudo com o analisador lógico.

1. Alinhamento dos bits (era o TODO). Com shift para a direita, in pins, 22; push deixa o dado em raw[31:10]: D0 vai para o bit 10 e A1 para o bit 26. O C decodifica raw & 0xFF (que dá 0) e (raw >> 16) & 7 (que dá lixo). O mesmo vale para o SM de dados: in pins, 16 deixa o word em raw[31:16], e o DMA de 16 bits lê o halfword baixo, que é zero.

A correção mais simples é mudar o shift de entrada para a esquerda nos dois SMs, em ide_interface_init. Assim o pino 0 cai no bit 0 e o decode que já está no C passa a servir:

c
sm_config_set_in_shift(&cfg,  false, false, 32);   // SM Data
sm_config_set_in_shift(&cfg2, false, false, 32);   // SM Taskfile

2. Leitura de registrador nunca dirige o barramento. No ide_taskfile, o do_read faz out pins, 8, mas nunca coloca D0-D7 como saída. O único mov pindirs do projeto está no SM de dados. O m68k leria o barramento flutuando. Substitua o do_read do ide_taskfile:

do_read:
    in   pins, 22
    push
    irq  set 1
    pull
    out  pins, 8
    mov  pindirs, ~null     ; valor primeiro, depois habilita a saida
    nop  side 0
    wait 1 gpio 24
    nop  side 1
    mov  pindirs, null
    jmp  wait_sel

No ide_data, eu faria a mesma troca de ordem: pull, out pins, 16 e só depois mov pindirs, ~null. Do jeito atual, o barramento é habilitado antes de haver dado, e, se o FIFO estiver vazio, fica habilitado esperando.

3. /DTACK nasce assertado. Depois de pio_gpio_init e pio_sm_set_consecutive_pindirs, o pino vira saída com nível 0, que é /DTACK ativo, até o primeiro side 1. Qualquer ciclo do 68k (inclusive em outros periféricos) recebe DTACK imediato. Antes de habilitar a direção, force o nível alto:

c
pio_gpio_init(g_pio, PIN_DTACK);
pio_sm_set_pins_with_mask(g_pio, g_sm_data, 1u << PIN_DTACK, 1u << PIN_DTACK);
pio_sm_set_consecutive_pindirs(g_pio, g_sm_data, PIN_DTACK, 1, true);

4. Entradas de controle não inicializadas. PIN_CS_DATA, PIN_CS_TASKFILE, PIN_DS e PIN_RW (22 a 25) nunca passam por pio_gpio_init. Pelo que lembro, no RP2350 os pads vêm isolados no reset e só ligam depois de uma seleção de função. Se for isso, a PIO nunca enxerga o CS. Adicione:

c
pio_gpio_init(g_pio, PIN_CS_DATA);
pio_gpio_init(g_pio, PIN_CS_TASKFILE);
pio_gpio_init(g_pio, PIN_DS);
pio_gpio_init(g_pio, PIN_RW);

Mudança no que esperar do teste. Não há timeout de DTACK, então uma falha não aparece como 0xFF: o m68k trava esperando. Se ele congelar na etapa 1, é o ISR ou o IRQ que não respondeu (itens 1 a 4). Pelo mesmo motivo, ler a porta de dados fora de um READ com DRQ, ou escrever nela fora de um WRITE, também congela. O teste só lê depois do DRQ.

Byte swap. Os bytes do setor saem como byte0 | byte1 << 8 (D0-D7 é o primeiro byte). Se o D0 do RP2350 estiver ligado ao D0 do 68k, no big-endian a assinatura vai aparecer como AA 55, e o teste avisa isso. Se acontecer, dá para corrigir no DMA com channel_config_set_bswap(&c, true) em arm_dma_b_from_buffer e arm_dma_c_into_buffer. Digo isso de memória, então confira no SDK.

Os problemas anteriores continuam valendo: leitura de mais de um setor trava, o último setor de escrita volta a ocioso antes do flush e falta IDENTIFY. Nenhum deles afeta o seu teste de 1 setor.

Aplique os itens 1 a 4, rode o teste e me diga em que etapa chegou.



Refiz o ide_test.c. Não compilei nem rodei esse programa, então ele ainda é leitura de código e não teste.

O que mudou por causa do que li no PIO e no ide_config.h:

Mensagem antes de cada acesso. Como não há timeout de DTACK, uma falha trava o 68k em vez de dar erro. A última linha impressa antes do travamento mostra onde ele ficou preso. Por isso o con_putchar() precisa esperar o TX esvaziar antes de retornar.
Pausa depois de cada escrita (settle()). Isso contorna um problema que descobri agora (item abaixo).
Etapa 1 lê o STATUS 8 vezes seguidas e confere se o valor é estável.
Etapa 2 testa dois conjuntos de valores, um deles com os complementos, para pegar bit preso.
Etapa 3 lê duas vezes e compara, decodifica a tabela de partições se achar a assinatura 55 AA, usa só 24 bits de LBA (é o que o Pico trata) e lê sempre 1 setor.
IDE_STAGES limita até qual etapa rodar, e TEST_LBA escolhe o setor.

Um quinto problema no firmware, que se soma aos quatro de antes: no ide_taskfile, a escrita faz irq set 0 e libera o DTACK sem esperar a ISR. Se o 68k lê logo depois de escrever, o pedido de leitura entra no FIFO enquanto o handler de escrita ainda está drenando, e o handler o consome como se fosse uma escrita. O PIO então fica parado no pull esperando um valor que nunca vem, e o 68k trava. Pelo que sei da PIO, a correção é trocar irq set 0 por irq wait 0 no do_write do ide_taskfile, o que segura o DTACK até a ISR atualizar o registrador e limpar o flag. O handler que você já tem faz o pio_interrupt_clear(g_pio, 0) no fim, então não precisa mudar. Com ou sem isso, o settle() do teste reduz a chance de a corrida acontecer. Num 68k rápido, a corrida volta.

Para rodar, ajuste: IDE_BASE (endereço real no seu mapa), con_putchar() e IDE_BYTE_LANE (1 se os D0-D7 do task file caem em D7-D0 do 68k, 0 se caem em D15-D8).

Se o 68k congelar, me diga qual foi a última linha impressa.

 */
#include <stdio.h>
#include <stdint.h>

/* ------------------- AJUSTE PARA O SEU HARDWARE ------------------- */
//#define IDE_BASE        0x00F00000UL  /* base da interface no mapa do m68k      */
#define IDE_BASE        0x00FF4000UL  /* base da interface no mapa do m68k      */
#define IDE_REG_STRIDE  2             /* A1..A3 => registrador a cada 2 bytes   */
#define IDE_BYTE_LANE   1             /* 68000: endereco par = D15-D8, impar =
                                         D7-D0. O task file usa D0-D7 do
                                         RP2350: se isso cai em D7-D0 do 68k,
                                         use 1; se cai em D15-D8, use 0.        */
#define IDE_STAGES      3             /* roda ate esta etapa (1, 2 ou 3)        */
#define TEST_LBA        0UL           /* setor a ler (< 0x1000000)              */
#define TIMEOUT         500000UL      /* iteracoes de polling                   */
#define SETTLE_LOOPS    300           /* pausa apos cada escrita (ver abaixo)   */
/* ------------------------------------------------------------------ */

extern void putchar(char c);         /* implemente no seu sistema */

/* Registradores ATA (iguais ao ide_config.h) */
#define R_ERROR     1   /* leitura  */
#define R_COUNT     2
#define R_LBA_LOW   3
#define R_LBA_MID   4
#define R_LBA_HIGH  5
#define R_DEVHEAD   6
#define R_STATUS    7   /* leitura  */
#define R_COMMAND   7   /* escrita  */

#define ST_BSY      0x80
#define ST_DRDY     0x40
#define ST_DRQ      0x08
#define ST_ERR      0x01

#define CMD_READ_SECTORS 0x20

#define REG8(n)  (*(volatile uint8_t *)(IDE_BASE + (uint32_t)(n) * IDE_REG_STRIDE + IDE_BYTE_LANE))
#define DATA16   (*(volatile uint16_t *)(IDE_BASE))

static uint16_t sector_a[256];
static uint16_t sector_b[256];

/* ---------------- saida de texto ---------------- */

//static void printf(const char *s) { while (*s) putchar(*s++); }
static void crlf(void)           { putchar('\r'); putchar('\n'); }

static void hex(uint32_t v, int digits) {
    while (digits--) putchar("0123456789ABCDEF"[(v >> (digits * 4)) & 0xF]);
}

static void dump(const uint8_t *p, int from, int to) {
    for (int i = from; i < to; i += 16) {
        printf("    "); hex(i, 3); printf(": ");
        for (int j = 0; j < 16; j++) { hex(p[i + j], 2); putchar(' '); }
        crlf();
    }
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------- acesso aos registradores ---------------- */

/* Pausa curta depois de cada escrita. A SM do task file libera o
 * /DTACK da escrita antes de a ISR do Pico atualizar o registrador-
 * sombra; uma leitura logo em seguida pode correr contra essa ISR.
 * Com `irq wait 0` no PIO (ver analise) a pausa deixa de ser
 * necessaria, mas nao atrapalha. */
static void settle(void) { for (volatile int i = 0; i < SETTLE_LOOPS; i++) { } }

static void    wr(uint8_t reg, uint8_t v){ 
    REG8(reg) = v; 
    settle(); 
}
static uint8_t rd(uint8_t reg){ 
    printf("Lendo reg8(%08x)\n",(volatile uint8_t *)(IDE_BASE + (uint32_t)(reg) * IDE_REG_STRIDE + IDE_BYTE_LANE));
    return REG8(reg); 
}

/* ---------------- etapa 1 ---------------- */

static int stage1_status(void) {
    uint8_t s, v;
    int stable = 1;

    printf(" [1] lendo STATUS... ");
    s = rd(R_STATUS);
    printf("0x"); hex(s, 2);
    if(s == ST_DRDY)
        printf("  OK (DRDY)\n");
    else{
        printf("  inesperado (esperado 0x40)[0x%02x]\n",s);
    }
    crlf();

    printf("    8 leituras seguidas... ");
    for (int i = 0; i < 8; i++) {
        v = rd(R_STATUS);
        if (v != s) 
            stable = 0;
    }
    if(stable){
        printf(" estavel\n");
    }else{
        printf(" instavel\n");
    }
    

    if (s == 0xFF || s == 0x00) {
        printf("    0xFF/0x00: barramento flutuando ou byte lane errado"); crlf();
        printf("    (tente IDE_BYTE_LANE = 0 / 1)"); crlf();
    }
    return s == ST_DRDY && stable;
}

/* ---------------- etapa 2 ---------------- */

static int stage2_regs(void) {
    static const uint8_t reg[4]  = { R_COUNT, R_LBA_LOW, R_LBA_MID, R_LBA_HIGH };
    static const uint8_t pass[2][4] = {
        { 0x5A, 0x11, 0x22, 0x33 },
        { 0xA5, 0xEE, 0xDD, 0xCC }     /* complementos: pega bit preso */
    };
    int ok = 1;

    printf("[2] loopback de registradores"); crlf();
    for (int p = 0; p < 2; p++) {
        printf("    escrevendo... ");
        for (int i = 0; i < 4; i++) wr(reg[i], pass[p][i]);
        printf("ok, lendo..."); crlf();
        for (int i = 0; i < 4; i++) {
            uint8_t r = rd(reg[i]);
            printf("    reg "); hex(reg[i], 1);
            printf(": escreveu 0x"); hex(pass[p][i], 2);
            printf(" leu 0x"); hex(r, 2);
            if (r != pass[p][i]) { printf("  ERRO"); ok = 0; }
            crlf();
        }
    }
    return ok;
}

/* ---------------- etapa 3 ---------------- */

static int read_sector(uint32_t lba, uint16_t *dst) {
    uint8_t  s = 0;
    uint32_t t;

    lba &= 0xFFFFFFUL;                       /* o Pico usa so 24 bits */

    printf("    programando registradores... ");
    wr(R_DEVHEAD,  0xE0);                    /* LBA, drive 0 */
    wr(R_COUNT,    1);
    wr(R_LBA_LOW,  (uint8_t)(lba));
    wr(R_LBA_MID,  (uint8_t)(lba >> 8));
    wr(R_LBA_HIGH, (uint8_t)(lba >> 16));
    printf("ok"); crlf();

    printf("    enviando READ SECTORS... ");
    wr(R_COMMAND, CMD_READ_SECTORS);
    printf("ok, aguardando DRQ... ");

    for (t = TIMEOUT; t; t--) {
        s = rd(R_STATUS);
        if (!(s & ST_BSY) && (s & (ST_DRQ | ST_ERR))) break;
    }
    printf("STATUS=0x"); hex(s, 2); crlf();
    if (!t)         { printf("    TIMEOUT esperando DRQ (leitura do SD travada?)"); crlf(); return 0; }
    if (s & ST_ERR) { printf("    ERR, ERROR=0x"); hex(rd(R_ERROR), 2); crlf(); return 0; }

    /* DRQ sobe uma vez e cai quando o DMA do Pico termina, nao quando o
     * 68k termina de ler: leia os 256 words sem rechecar o status. */
    printf("    lendo 256 words... ");
    for (int i = 0; i < 256; i++) dst[i] = DATA16;
    printf("ok"); crlf();

    printf("    STATUS final = 0x"); hex(rd(R_STATUS), 2);
    printf("  (esperado 0x40)"); crlf();
    return 1;
}

static void show_mbr(const uint8_t *b) {
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = b + 0x1BE + 16 * i;
        printf("    part "); hex(i + 1, 1);
        printf(": boot=");   hex(e[0], 2);
        printf(" tipo=");     hex(e[4], 2);
        printf(" inicio=");   hex(le32(e + 8), 8);
        printf(" setores=");  hex(le32(e + 12), 8);
        crlf();
    }
}

static int stage3_read(void) {
    const uint8_t *a = (const uint8_t *)sector_a;

    printf("[3] READ SECTORS, LBA 0x"); hex(TEST_LBA, 6); printf(", 1 setor"); crlf();
    if (!read_sector(TEST_LBA, sector_a)) return 0;

    dump(a, 0x000, 0x040);
    printf("    ..."); crlf();
    dump(a, 0x1B0, 0x200);

    printf("    assinatura [510..511] = ");
    hex(a[510], 2); putchar(' '); hex(a[511], 2);
    if (a[510] == 0x55 && a[511] == 0xAA) {
        printf("  OK (55 AA)"); crlf();
        if (TEST_LBA == 0) show_mbr(a);
    } else if (a[510] == 0xAA && a[511] == 0x55) {
        printf("  BYTES TROCADOS em cada word"); crlf();
        printf("    (corrija na fiacao ou com channel_config_set_bswap nos DMAs)"); crlf();
    } else {
        printf("  sem assinatura (setor sem MBR/boot, ou dado errado)"); crlf();
    }

    printf("    segunda leitura para comparar:"); crlf();
    if (!read_sector(TEST_LBA, sector_b)) return 0;
    for (int i = 0; i < 256; i++) {
        if (sector_a[i] != sector_b[i]) {
            printf("    DIFERENTE no word "); hex(i, 2); crlf();
            return 0;
        }
    }
    printf("    as duas leituras sao identicas"); crlf();
    return 1;
}

int main(void) {
    printf("== teste IDE/RP2350B v2 =="); crlf();
    if (!stage1_status())                         return 1;
    if (IDE_STAGES >= 2 && !stage2_regs())        return 2;
    if (IDE_STAGES >= 3 && !stage3_read())        return 3;
    printf("fim: tudo certo"); crlf();
    return 0;
}
