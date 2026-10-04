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

#define SCRATCH_FORCE

/* ------------------- AJUSTE PARA O SEU HARDWARE ------------------- */
#define IDE_BASE        0x00FF4000UL  /* base da interface no mapa do m68k      */
#define IDE_REG_STRIDE  2             /* A1..A3 => registrador a cada 2 bytes   */
#define IDE_BYTE_LANE   1             /* 68000: endereco par = D15-D8, impar =
                                         D7-D0. O task file usa D0-D7 do
                                         RP2350: se isso cai em D7-D0 do 68k,
                                         use 1; se cai em D15-D8, use 0.        */
#define IDE_STAGES      6             /* roda ate esta etapa (1, 2, 3 ou 4)        */
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

     lba &= 0x0FFFFFFFUL;                        /* o Pico usa so 28 bits */

    printf("    programando registradores... ");
    wr(R_DEVHEAD,  0xE0 | ((lba >> 24) & 0x0F));   // LBA[27:24]
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

static int stage4_lba28(void) {
    printf("[4] fronteira de 8 GB: LBA 0x01000000 contra LBA 0\n");
    if (!read_sector(0x01000000UL, sector_b)) return 0;
    for (int i = 0; i < 256; i++) {
        if (sector_a[i] != sector_b[i]) {
            printf("    setores diferentes: LBA de 28 bits funcionando\n");
            return 1;
        }
    }
    printf("    IDENTICOS: o Pico ainda trunca o LBA em 24 bits\n");
    return 0;
} 

/* ============================================================================
 * ETAPA 5 do ide_test.c: WRITE SECTORS com leitura de volta e restauracao
 * ============================================================================
 * Cole este bloco no seu ide_test.c, depois da stage4 e antes do main().
 * Usa as funcoes que ja existem: wr, rd, hex, printf, crlf, read_sector,
 * as constantes ST_*, R_*, TIMEOUT e o buffer sector_a (setor 0, da etapa 3).
 *
 * SEGURANCA
 *   - O setor de teste e SCRATCH_LBA = 0x7FE (2046): fica no vao entre o MBR
 *     (setor 0) e o inicio da 1a particao (setor 2048, pelo seu dump).
 *   - O teste le o setor antes. Se ele NAO estiver todo 0x00 ou todo 0xFF,
 *     aborta sem gravar nada.
 *   - Termina restaurando o conteudo original e conferindo que o setor 0
 *     (MBR) continua intacto.
 *   - Mesmo assim, se o cartao tiver dados importantes, faca uma copia dele
 *     antes (a primeira escrita de verdade e o ponto de maior risco).
 */

#define CMD_WRITE_SECTORS 0x30
#define SCRATCH_LBA       0x000007FEUL

static uint16_t sector_orig[256];
static uint16_t sector_pat[256];
static uint16_t sector_chk[256];

static int write_sector(uint32_t lba, const uint16_t *src) {
    uint8_t  s = 0;
    uint32_t t;

    lba &= 0x0FFFFFFFUL;
    printf("    programando registradores... ");
    wr(R_DEVHEAD,  0xE0 | ((lba >> 24) & 0x0F));
    wr(R_COUNT,    1);
    wr(R_LBA_LOW,  (uint8_t)lba);
    wr(R_LBA_MID,  (uint8_t)(lba >> 8));
    wr(R_LBA_HIGH, (uint8_t)(lba >> 16));
    printf("ok"); crlf();

    printf("    enviando WRITE SECTORS... ");
    wr(R_COMMAND, CMD_WRITE_SECTORS);
    printf("ok, aguardando DRQ... ");
    for (t = TIMEOUT; t; t--) {
        s = rd(R_STATUS);
        if (!(s & ST_BSY) && (s & (ST_DRQ | ST_ERR))) break;
    }
    printf("STATUS=0x"); hex(s, 2); crlf();
    if (!t)         { printf("    TIMEOUT esperando DRQ"); crlf(); return 0; }
    if (s & ST_ERR) { printf("    ERR, ERROR=0x"); hex(rd(R_ERROR), 2); crlf(); return 0; }

    printf("    escrevendo 256 words... ");
    for (int i = 0; i < 256; i++) DATA16 = src[i];
    printf("ok, aguardando o SD gravar... ");

    for (t = TIMEOUT; t; t--) {
        s = rd(R_STATUS);
        if (!(s & (ST_BSY | ST_DRQ))) break;
    }
    printf("STATUS=0x"); hex(s, 2); crlf();
    if (!t)         { printf("    TIMEOUT esperando o fim da gravacao"); crlf(); return 0; }
    if (s & ST_ERR) { printf("    ERR na gravacao, ERROR=0x"); hex(rd(R_ERROR), 2); crlf(); return 0; }
    return 1;
}

static int same_bytes(const uint16_t *a, const uint16_t *b, const char *name) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (int i = 0; i < 512; i++) {
        if (pa[i] != pb[i]) {
            printf("    DIFERENTE ("); printf(name); printf(") no byte 0x"); hex(i, 3);
            printf(": esperado 0x"); hex(pa[i], 2);
            printf(" leu 0x");       hex(pb[i], 2); crlf();
            return 0;
        }
    }
    return 1;
}

static int stage5_write(void) {
    const uint8_t *po = (const uint8_t *)sector_orig;
    uint8_t       *pp = (uint8_t *)sector_pat;
    int blank = 1;

    printf("[5] WRITE SECTORS no setor de teste, LBA 0x"); hex(SCRATCH_LBA, 8); crlf();

    printf("  (a) lendo o conteudo original"); crlf();
    if (!read_sector(SCRATCH_LBA, sector_orig)) return 0;
    for (int i = 1; i < 512; i++) if (po[i] != po[0]) blank = 0;
    if (!blank || (po[0] != 0x00 && po[0] != 0xFF)) {
        printf("    o setor NAO esta vazio (todo 00 ou todo FF)."); crlf();
#ifndef SCRATCH_FORCE
        printf("    Abortando sem gravar. Escolha outro SCRATCH_LBA."); crlf();
        return 0;
#else
        printf("    SCRATCH_FORCE definido: seguindo; o original sera restaurado ao final."); crlf();
#endif
    }

    /* padrao com bytes diferentes em posicoes pares e impares: pega byte trocado */
    for (int i = 0; i < 512; i++) pp[i] = (uint8_t)(i * 7 + 3);

    printf("  (b) gravando o padrao"); crlf();
    if (!write_sector(SCRATCH_LBA, sector_pat)) return 0;

    printf("  (c) lendo de volta"); crlf();
    if (!read_sector(SCRATCH_LBA, sector_chk)) return 0;
    if (!same_bytes(sector_pat, sector_chk, "padrao")) return 0;
    printf("    padrao gravado e lido de volta: OK"); crlf();

    printf("  (d) restaurando o original"); crlf();
    if (!write_sector(SCRATCH_LBA, sector_orig)) return 0;
    if (!read_sector(SCRATCH_LBA, sector_chk)) return 0;
    if (!same_bytes(sector_orig, sector_chk, "original")) return 0;
    printf("    original restaurado: OK"); crlf();

    printf("  (e) conferindo que o setor 0 (MBR) nao foi tocado"); crlf();
    if (!read_sector(0, sector_chk)) return 0;
    if (!same_bytes(sector_a, sector_chk, "setor 0")) return 0;
    printf("    setor 0 intacto: OK"); crlf();
    return 1;
}
/* ============================================================================
 * ETAPA 6 do ide_test.c: varios setores por comando + status logo apos a ultima word
 * ============================================================================
 * Cole depois da etapa 5 (usa same_bytes() e sector_a, que ja existem la e na
 * etapa 3), antes do main(). No main(), depois da etapa 5:
 *     if (!stage6_multi()) return 6;
 *
 * O QUE ESTA ETAPA TESTA
 *   (a) READ SECTORS com count=4 contra 4 leituras avulsas.
 *   (b) WRITE SECTORS com count=4 no vao de teste (LBA 2040..2043, antes da
 *       1a particao, que comeca no setor 2048), leitura de volta e restauracao.
 *   Em todas as transferencias, o status e lido IMEDIATAMENTE depois da ultima
 *   word com leitura CRUA (STATUS_RAW, sem o print do seu rd()), pois cada print
 *   demora milissegundos e escondia a janela de BSY. Os valores ficam num vetor
 *   e so sao impressos depois.
 *
 * COMO LER O "TRACE DO STATUS"
 *   0x48 = DRDY+DRQ (pede dados)   0xC0 = DRDY+BSY (ocupado)   0x40 = ocioso
 *   Escrita, logo apos a ultima word de um setor:
 *     - valido:   [C0 C0 C0 ...]  ou, com atraso, [48 C0 C0 ...]
 *     - INVALIDO: [40 ...]  (ocioso antes de gravar)  ou [48 48 48 ... ]  (nunca ocupou)
 *   Leitura, logo apos a ultima word de um setor:
 *     - setor intermediario: C0 (ou 48 se o SD ja entregou o proximo)
 *     - ultimo setor: 40
 *
 * SEGURANCA: igual a etapa 5. Aborta sem gravar se algum dos 4 setores do vao
 * nao estiver todo 00 ou todo FF, e restaura o original no fim.
 */

#define MULTI_N        4
#define SCRATCH_MULTI  0x000007F8UL     /* LBA 2040..2043 */
#define TRACE_N        8

static uint16_t multi_rd[MULTI_N][256];    /* leituras em bloco / releituras */
static uint16_t multi_ref[MULTI_N][256];   /* leituras avulsas, depois o padrao */
static uint16_t multi_orig[MULTI_N][256];  /* conteudo original do vao */
static uint8_t  trace[TRACE_N];

static void q_setup(uint32_t lba, uint8_t count) {
    lba &= 0x0FFFFFFFUL;
    wr(R_DEVHEAD,  0xE0 | ((lba >> 24) & 0x0F));
    wr(R_COUNT,    count);
    wr(R_LBA_LOW,  (uint8_t)lba);
    wr(R_LBA_MID,  (uint8_t)(lba >> 8));
    wr(R_LBA_HIGH, (uint8_t)(lba >> 16));
}

/* espera BSY=0 e (DRQ ou ERR). Devolve 1 se DRQ sem ERR. */
static int q_wait_drq(uint8_t *st) {
    uint8_t s = 0;
    for (uint32_t t = TIMEOUT; t; t--) {
        s = rd(R_STATUS);
        if (!(s & ST_BSY) && (s & (ST_DRQ | ST_ERR))) { *st = s; return !(s & ST_ERR); }
    }
    *st = s;
    return 0;
}

/* espera BSY=0 e DRQ=0. Devolve 1 se terminou sem ERR. */
static int q_wait_idle(uint8_t *st) {
    uint8_t s = 0;
    for (uint32_t t = TIMEOUT; t; t--) {
        s = rd(R_STATUS);
        if (!(s & (ST_BSY | ST_DRQ))) { *st = s; return !(s & ST_ERR); }
    }
    *st = s;
    return 0;
}

/* Leitura CRUA do status: acesso direto ao barramento, SEM nenhum print.
 * Seu rd() imprime "Lendo reg8(...)" a cada leitura, e esse print leva
 * milissegundos: o SD termina de gravar nesse tempo e a janela de BSY some. */
#ifndef STATUS_RAW
#define STATUS_RAW (*(volatile uint8_t *)0x00FF400FUL)
#endif

/* le o status TRACE_N vezes seguidas, o mais rapido possivel */
static void q_trace(void) {
    for (int i = 0; i < TRACE_N; i++) trace[i] = STATUS_RAW;
}

static void q_print_trace(void) {
    printf("    trace do status:");
    for (int i = 0; i < TRACE_N; i++) { putchar(' '); hex(trace[i], 2); }
    crlf();
}

static int q_uniform(const uint16_t *s) {
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] != 0x00 && p[0] != 0xFF) return 0;
    for (int i = 1; i < 512; i++) if (p[i] != p[0]) return 0;
    return 1;
}

/* Escrita, logo apos a ultima word do setor k de n.
 * Por status so, "DRQ velho" e "DRQ do proximo setor" sao iguais (0x48).
 * Entao:
 *   - ULTIMO setor: nao existe proximo DRQ. Qualquer 0x48 no trace e DRQ velho
 *     (a corrida que queremos pegar). Aceitos: C0 (gravando) e 40 (ja acabou).
 *   - setor INTERMEDIARIO: 0x48 e legitimo (proximo setor ja pedindo dados).
 *     Invalido so 0x40 (ocioso com setores faltando) ou ERR.
 * *lead = quantas leituras iniciais ainda mostravam 0x48 (informativo). */
static int q_trace_write_ok(int k, int n, int *lead) {
    int i = 0;
    while (i < TRACE_N && trace[i] == (ST_DRDY | ST_DRQ)) i++;
    *lead = i;
    for (int j = 0; j < TRACE_N; j++)
        if (trace[j] & ST_ERR) return 0;
    if (k == n - 1) {
        for (int j = 0; j < TRACE_N; j++)
            if (trace[j] & ST_DRQ) return 0;            /* DRQ velho */
        return 1;
    }
    for (int j = 0; j < TRACE_N; j++)
        if (trace[j] == ST_DRDY) return 0;              /* ocioso cedo demais */
    return 1;
}

static int q_read_multi(uint32_t lba, uint8_t n, uint16_t dst[][256]) {
    uint8_t s;
    q_setup(lba, n);
    wr(R_COMMAND, CMD_READ_SECTORS);
    for (int k = 0; k < n; k++) {
        if (!q_wait_drq(&s)) {
            printf("    falhou esperando DRQ do setor "); hex(k, 1);
            printf(", STATUS=0x"); hex(s, 2); crlf();
            return 0;
        }
        for (int i = 0; i < 256; i++) dst[k][i] = DATA16;
        q_trace();                           /* status logo apos a ultima word */
        /* Leitura nao tem o guard de status: o DRQ pode ficar "velho" por alguns
         * microssegundos ate a ISR rodar. Entao so exigimos: sem ERR no trace e,
         * no ultimo setor, que o status ACABE em 0x40 (polling, sem pressa). */
        int bad = 0;
        for (int j = 0; j < TRACE_N; j++) if (trace[j] & ST_ERR) bad = 1;
        if (k == n - 1) {
            uint8_t fin;
            if (!q_wait_idle(&fin) || fin != ST_DRDY) bad = 1;
        }
        if (bad) {
            printf("    status inesperado logo apos o setor "); hex(k, 1); crlf();
            q_print_trace();
            return 0;
        }
    }
    return 1;
}

static int q_write_multi(uint32_t lba, uint8_t n, uint16_t src[][256], int *max_lead) {
    uint8_t s;
    int lead;
    *max_lead = 0;
    q_setup(lba, n);
    wr(R_COMMAND, CMD_WRITE_SECTORS);
    for (int k = 0; k < n; k++) {
        if (!q_wait_drq(&s)) {
            printf("    falhou esperando DRQ do setor "); hex(k, 1);
            printf(", STATUS=0x"); hex(s, 2); crlf();
            return 0;
        }
        for (int i = 0; i < 256; i++) DATA16 = src[k][i];
        q_trace();                           /* status logo apos a ultima word */
        if (!q_trace_write_ok(k, n, &lead)) {
            printf("    status invalido logo apos a gravacao do setor "); hex(k, 1); crlf();
            q_print_trace();
            return 0;
        }
        if (lead > *max_lead) *max_lead = lead;
    }
    if (!q_wait_idle(&s)) {
        printf("    gravacao nao terminou bem, STATUS=0x"); hex(s, 2); crlf();
        return 0;
    }
    return 1;
}

static int stage6_multi(void) {
    int lead1 = 0, lead2 = 0;

    printf("[6] varios setores por comando"); crlf();

    /* (a) leitura em bloco contra leituras avulsas */
    printf("  (a) READ SECTORS count=4 no LBA 0 contra 4 leituras avulsas"); crlf();
    if (!q_read_multi(0, MULTI_N, multi_rd)) return 0;
    for (int k = 0; k < MULTI_N; k++) {
        if (!q_read_multi((uint32_t)k, 1, &multi_ref[k])) return 0;
        if (!same_bytes(multi_rd[k], multi_ref[k], "bloco/avulso")) return 0;
    }
    printf("    4 setores em bloco iguais aos 4 avulsos: OK"); crlf();

    /* (b) escrita em bloco no vao de teste */
    printf("  (b) WRITE SECTORS count=4 no vao de teste, LBA 0x"); hex(SCRATCH_MULTI, 8); crlf();
    if (!q_read_multi(SCRATCH_MULTI, MULTI_N, multi_orig)) return 0;
    for (int k = 0; k < MULTI_N; k++) {
        if (!q_uniform(multi_orig[k])) {
            printf("    o setor "); hex(k, 1); printf(" do vao NAO esta vazio (todo 00 ou todo FF)."); crlf();
#ifndef SCRATCH_FORCE
            printf("    Abortando sem gravar. Escolha outro SCRATCH_MULTI."); crlf();
            return 0;
#else
            printf("    SCRATCH_FORCE definido: seguindo; o original sera restaurado."); crlf();
#endif
        }
    }

    for (int k = 0; k < MULTI_N; k++) {          /* padrao diferente em cada setor */
        uint8_t *p = (uint8_t *)multi_ref[k];
        for (int i = 0; i < 512; i++) p[i] = (uint8_t)(i * 7 + 3 + k * 0x11);
    }

    printf("    gravando os 4 setores em um comando"); crlf();
    if (!q_write_multi(SCRATCH_MULTI, MULTI_N, multi_ref, &lead1)) return 0;

    printf("    lendo de volta, setor por setor"); crlf();
    for (int k = 0; k < MULTI_N; k++) {
        if (!q_read_multi(SCRATCH_MULTI + k, 1, &multi_rd[k])) return 0;
        if (!same_bytes(multi_ref[k], multi_rd[k], "gravado")) return 0;
    }
    printf("    4 setores gravados em bloco e lidos de volta: OK"); crlf();

    printf("    restaurando o original (tambem em um comando)"); crlf();
    if (!q_write_multi(SCRATCH_MULTI, MULTI_N, multi_orig, &lead2)) return 0;
    for (int k = 0; k < MULTI_N; k++) {
        if (!q_read_multi(SCRATCH_MULTI + k, 1, &multi_rd[k])) return 0;
        if (!same_bytes(multi_orig[k], multi_rd[k], "original")) return 0;
    }
    printf("    original restaurado: OK"); crlf();

    /* (c) MBR intacto */
    printf("  (c) conferindo que o setor 0 (MBR) nao foi tocado"); crlf();
    if (!q_read_multi(0, 1, &multi_rd[0])) return 0;
    if (!same_bytes(sector_a, multi_rd[0], "setor 0")) return 0;
    printf("    setor 0 intacto: OK"); crlf();

    /* (d) resumo da corrida do status */
    printf("  (d) status logo apos a ultima word de cada setor gravado"); crlf();
    printf("    maior atraso visto: "); hex(lead1 > lead2 ? lead1 : lead2, 1);
    printf(" leitura(s) ainda com DRQ"); crlf();
    printf("    (0 = o Pico ja mostrava BSY na 1a leitura; >0 e aceitavel, mas o driver");
    crlf();
    printf("     do OrionDOS deve esperar BSY=0 e DRQ=0 antes de dar a escrita por pronta)");
    crlf();
    return 1;
}

/* chame antes do stage 1 */
static void recover(void) {
    uint8_t s;
    wr(R_DEVHEAD, 0xE0); wr(R_COUNT, 1);
    wr(R_LBA_LOW, 0); wr(R_LBA_MID, 0); wr(R_LBA_HIGH, 0);
    wr(R_COMMAND, CMD_READ_SECTORS);
    if (q_wait_drq(&s)) {
        for (int i = 0; i < 256; i++) (void)DATA16;
        q_wait_idle(&s);
    }
}

int main(void) {

    read_sector(0x0,(uint16_t *) 0x92000);
    return 0;


    printf("== teste IDE/RP2350B v2 =="); crlf();
    recover();
    if (!stage1_status())                         return 1;
    if (IDE_STAGES >= 2 && !stage2_regs())        return 2;
    if (IDE_STAGES >= 3 && !stage3_read())        return 3;
    if (IDE_STAGES >= 4 && !stage4_lba28())       return 4;
    if (IDE_STAGES >= 5 && !stage5_write())       return 5;
    if (IDE_STAGES >= 6 && !stage6_multi())       return 6;
    

    printf("fim: tudo certo"); crlf();
    return 0;
}
