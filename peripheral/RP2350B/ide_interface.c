// ide_interface.c
//
// Driver da interface IDE emulada (RP2350B <-> m68k via MIDE).
//
// IMPORTANTE — leia antes de gravar no hardware real:
//  - Este arquivo assume que voce gerou ide_pio.pio.h via
//    pico_generate_pio_header(seu_alvo ide_pio.pio) no CMakeLists.
//  - As chamadas bloqueantes de SD (ide_hook_sd_*) NUNCA sao feitas
//    dentro de uma ISR aqui — elas so acontecem em ide_interface_task(),
//    que voce deve chamar em loop apertado (idealmente rodando sozinha
//    no core1, deixando o core0 livre pra outras coisas). As ISRs so
//    mexem em flags/indices e reprogramam DMA, que e rapido.
//  - A ordem de bits capturada pela PIO (addr/dado combinados) precisa
//    ser validada com analisador logico — os extracts abaixo (IDE_ADDR_*)
//    sao o ponto a ajustar se a ordem vier trocada.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "ide_config.h"
#include "ide_interface.h"
#include "ide_pio.pio.h"   // gerado pelo build a partir de ide_pio.pio
#include "hardware/sync.h"     // __dmb()

#ifndef IDE_ALLOW_WRITE
#define IDE_ALLOW_WRITE 1      // 0 = WRITE SECTORS responde ERR (modo somente leitura)
#endif


// -----------------------------------------------------------------------
// Estado global
// -----------------------------------------------------------------------

static PIO g_pio;
static uint g_sm_data;
static uint g_sm_taskfile;

static int g_dma_b;   // PSRAM -> TX FIFO da SM Data (leitura, m68k <- disco)
static int g_dma_c;   // RX FIFO da SM Data -> PSRAM (escrita, m68k -> disco)

static uint8_t *g_buf[IDE_NUM_BUFFERS];

typedef enum { DIR_NONE, DIR_READ, DIR_WRITE } ide_dir_t;

static volatile ide_dir_t g_dir = DIR_NONE;
static volatile uint32_t  g_lba;
static volatile uint16_t  g_remaining_sectors;
static volatile int       g_active_buf;     // buffer atualmente ligado ao DMA_B/DMA_C
static volatile bool      g_buf_ready[IDE_NUM_BUFFERS];

// pedidos que o handler de IRQ deixa para ide_interface_task() executar
// (E' aqui que a chamada bloqueante ao SD realmente acontece)
static volatile bool     g_need_fetch;   // leitura: precisa buscar proximo setor do SD
static volatile int      g_fetch_buf;
static volatile uint32_t g_fetch_lba;

static volatile bool     g_need_flush;   // escrita: precisa gravar setor no SD
static volatile int      g_flush_buf;
static volatile uint32_t g_flush_lba;

// shadow task-file registers
static volatile uint8_t g_reg_error_features;
static volatile uint8_t g_reg_sector_count;
static volatile uint8_t g_reg_lba_low, g_reg_lba_mid, g_reg_lba_high;
static volatile uint8_t g_reg_device_head;
static volatile uint8_t g_reg_status = IDE_STATUS_DRDY;

// -----------------------------------------------------------------------
// Helpers de status
// -----------------------------------------------------------------------

static inline void set_busy(void) {
    g_reg_status = (g_reg_status | IDE_STATUS_BSY) & ~IDE_STATUS_DRQ;
}
static inline void set_drq(void) {
    g_reg_status = (g_reg_status | IDE_STATUS_DRQ) & ~IDE_STATUS_BSY;
}
static inline void clear_drq(void) {
    g_reg_status &= ~IDE_STATUS_DRQ;
}
static inline void set_ready_idle(void) {
    g_reg_status = IDE_STATUS_DRDY; // BSY=0, DRQ=0, ERR=0
}
static void reset_data_path(void) {
    dma_channel_abort(g_dma_b);
    dma_channel_abort(g_dma_c);
    dma_hw->ints0 = 1u << g_dma_b;          // limpa flags pendentes
    dma_hw->ints1 = 1u << g_dma_c;
    pio_sm_clear_fifos(g_pio, g_sm_data);   // descarta palavras velhas
}
// -----------------------------------------------------------------------
// Config dos canais de DMA usados pela SM Data (caminho rapido)
// -----------------------------------------------------------------------

//static void arm_dma_b_from_buffer(int buf_index) {
//    // Envia 1 setor (256 words) do PSRAM para o TX FIFO da SM Data.
//    dma_channel_config c = dma_channel_get_default_config(g_dma_b);
//    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
//    channel_config_set_read_increment(&c, true);
//    channel_config_set_write_increment(&c, false);
//    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, true /* TX */));
//    dma_channel_configure(
//        g_dma_b, &c,
//        &g_pio->txf[g_sm_data],           // destino: TX FIFO da SM Data
//        g_buf[buf_index],                  // origem: buffer em PSRAM
//        IDE_SECTOR_SIZE_WORDS,
//        true                                // dispara imediatamente
//    );
//}
// --- 2) DMA de leitura (SD -> buffer -> 68k) com bswap ----------------------
static void arm_dma_b_from_buffer(int buf_index) {
    dma_channel_config c = dma_channel_get_default_config(g_dma_b);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_bswap(&c, true);                 // <-- NOVO: troca os 2 bytes de cada word
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, true /* TX */));
    dma_channel_configure(
        g_dma_b, &c,
        &g_pio->txf[g_sm_data],
        g_buf[buf_index],
        IDE_SECTOR_SIZE_WORDS,
        true
    );
}

//static void arm_dma_c_into_buffer(int buf_index) {
//    // Recebe 1 setor (256 words) do RX FIFO da SM Data para o PSRAM.
//    dma_channel_config c = dma_channel_get_default_config(g_dma_c);
//    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
//    channel_config_set_read_increment(&c, false);
//    channel_config_set_write_increment(&c, true);
//    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, false /* RX */));
//    dma_channel_configure(
//        g_dma_c, &c,
//        g_buf[buf_index],
//        &g_pio->rxf[g_sm_data],
//        IDE_SECTOR_SIZE_WORDS,
//        true
//    );
//}
// --- 3) DMA de escrita (68k -> buffer -> SD) com bswap ----------------------
// Precisa do MESMO bswap, senao a escrita grava o setor com os bytes trocados.
static void arm_dma_c_into_buffer(int buf_index) {
    dma_channel_config c = dma_channel_get_default_config(g_dma_c);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_bswap(&c, true);                 // <-- NOVO
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, false /* RX */));
    dma_channel_configure(
        g_dma_c, &c,
        g_buf[buf_index],
        &g_pio->rxf[g_sm_data],
        IDE_SECTOR_SIZE_WORDS,
        true
    );
}
// -----------------------------------------------------------------------
// Sequencia de comando: READ SECTORS / WRITE SECTORS
// -----------------------------------------------------------------------

//static uint32_t current_lba_from_regs(void) {
//    return ((uint32_t)g_reg_lba_high << 16) |
//           ((uint32_t)g_reg_lba_mid  << 8)  |
//            (uint32_t)g_reg_lba_low;
//}
// --- 1) LBA de 28 bits --------------------------------------------------------
// ATA classico: LBA[7:0]=LBA Low, [15:8]=LBA Mid, [23:16]=LBA High e
// LBA[27:24] = nibble BAIXO do registrador Device/Head.
// (Device/Head: bit7=1, bit6=LBA, bit5=1, bit4=drive, bits3..0 = LBA[27:24])
static uint32_t current_lba_from_regs(void) {
    return ((uint32_t)(g_reg_device_head & 0x0F) << 24) |
           ((uint32_t)g_reg_lba_high << 16) |
           ((uint32_t)g_reg_lba_mid  << 8)  |
            (uint32_t)g_reg_lba_low;
}


static uint16_t sector_count_from_reg(void) {
    // Convencao ATA classica: 0 no registrador significa 256 setores.
    return g_reg_sector_count == 0 ? 256 : g_reg_sector_count;
}

//static void begin_read_sequence(void) {
//    g_dir = DIR_READ;
//    g_lba = current_lba_from_regs();
//    g_remaining_sectors = sector_count_from_reg();
//    g_active_buf = 0;
//    g_buf_ready[0] = g_buf_ready[1] = false;
//
//    set_busy();
//    g_need_fetch = true;
//    g_fetch_buf  = 0;
//    g_fetch_lba  = g_lba;
//}
static void begin_read_sequence(void) {
    reset_data_path();
    g_dir = DIR_READ;
    g_lba = current_lba_from_regs();
    g_remaining_sectors = sector_count_from_reg();
    g_active_buf = 0;
    g_buf_ready[0] = g_buf_ready[1] = false;

    set_busy();
    g_fetch_buf = 0;
    g_fetch_lba = g_lba;
    __dmb();
    g_need_fetch = true;
}

//static void begin_write_sequence(void) {
//    g_dir = DIR_WRITE;
//    g_lba = current_lba_from_regs();
//    g_remaining_sectors = sector_count_from_reg();
//    g_active_buf = 0;
//    g_buf_ready[0] = g_buf_ready[1] = false;
//
//    // Pronto pra aceitar o primeiro setor vindo do m68k.
//    set_drq();
//    arm_dma_c_into_buffer(0);
//}
static void begin_write_sequence(void) {
    reset_data_path();
    g_dir = DIR_WRITE;
    g_lba = current_lba_from_regs();
    g_remaining_sectors = sector_count_from_reg();
    g_active_buf = 0;
    g_buf_ready[0] = g_buf_ready[1] = false;

    arm_dma_c_into_buffer(0);               // DMA pronto ANTES do DRQ
    set_drq();                              // o 68k ja pode transferir o 1o setor
}
// ============================================================================
// B) ide_interface.c  -  montagem do bloco IDENTIFY
// ============================================================================
#define IDE_CMD_IDENTIFY       0xEC
#define IDE_CMD_DIAGNOSTIC     0x90
#define IDE_CMD_INIT_PARAMS    0x91
#define IDE_CMD_SET_FEATURES   0xEF
#define IDE_CMD_FLUSH_CACHE    0xE7

#define IDE_ID_MODEL   "picoIDE RP2350B SD"
#define IDE_ID_SERIAL  "ORION68-0001"
#define IDE_ID_FW      "2.3.0"

static uint32_t g_sd_sectors = 0;      // capacidade real do cartao

void ide_set_sd_sectors(uint32_t n) { g_sd_sectors = n; }

// string ATA: 2 chars por word, o 1o no byte alto, completa com espacos
static void id_put_string(uint16_t *w, int first_word, int n_words, const char *s) {
    for (int i = 0; i < n_words; i++) {
        uint8_t c0 = *s ? (uint8_t)*s++ : ' ';
        uint8_t c1 = *s ? (uint8_t)*s++ : ' ';
        w[first_word + i] = (uint16_t)((c0 << 8) | c1);
    }
}

static void build_identify(uint16_t *w) {
    uint32_t total = g_sd_sectors;
    if (total > 0x0FFFFFFFu) total = 0x0FFFFFFFu;           // limite do LBA28

    const uint32_t cyl = 16383, hd = 16, spt = 63;          // CHS "logico" padrao
    uint32_t chs = cyl * hd * spt;
    if (chs > total) chs = total;

    for (int i = 0; i < 256; i++) w[i] = 0;

    w[0]  = 0x0040;                          // disco fixo
    w[1]  = cyl;  w[3] = hd;  w[6] = spt;
    id_put_string(w, 10, 10, IDE_ID_SERIAL); // words 10..19 (20 chars)
    id_put_string(w, 23, 4,  IDE_ID_FW);     // words 23..26 (8 chars)
    id_put_string(w, 27, 20, IDE_ID_MODEL);  // words 27..46 (40 chars)
    w[47] = 0;                               // sem READ/WRITE MULTIPLE
    w[49] = 0x0200;                          // bit 9: LBA suportado
    w[53] = 0x0001;                          // words 54..58 validas
    w[54] = cyl;  w[55] = hd;  w[56] = spt;
    w[57] = (uint16_t)(chs & 0xFFFF);
    w[58] = (uint16_t)(chs >> 16);
    w[60] = (uint16_t)(total & 0xFFFF);      // total de setores LBA28 (baixa)
    w[61] = (uint16_t)(total >> 16);         //                         (alta)
    w[80] = 0x007E;                          // ATA-1 a ATA-6
    w[83] = 0x4000;  w[84] = 0x4000;  w[87] = 0x4000;   // bit 14 = campo valido

    // word 255: assinatura 0xA5 no byte baixo; byte alto = complemento da soma
    w[255] = 0x00A5;
    uint8_t sum = 0;
    const uint8_t *b = (const uint8_t *)w;
    for (int i = 0; i < 512; i++) sum += b[i];
    w[255] |= (uint16_t)((uint8_t)(0 - sum)) << 8;
}

static void begin_identify(void) {
    if (g_sd_sectors == 0) {                 // capacidade desconhecida: recusa
        g_reg_error_features = 0x04;
        g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
        return;
    }
    reset_data_path();
    set_busy();
    g_dir = DIR_READ;
    g_remaining_sectors = 1;
    g_active_buf = 0;
    g_buf_ready[0] = g_buf_ready[1] = false;

    build_identify((uint16_t *)g_buf[0]);    // g_buf e uint8_t*, alinhado em 2
    g_buf_ready[0] = true;
    __dmb();
    arm_dma_b_from_buffer(0);                // DMA antes do DRQ
    set_drq();                               // o dma_b_irq_handler fecha com 0x40
}


//static void dispatch_command(uint8_t cmd) {
//    switch (cmd) {
//        case IDE_CMD_READ_SECTORS:
//            begin_read_sequence();
//            break;
//        case IDE_CMD_WRITE_SECTORS:
//            begin_write_sequence();
//            break;
//        default:
//            // comando nao implementado: sinaliza erro, nao trava
//            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
//            break;
//    }
//}
//statge 7
//static void dispatch_command(uint8_t cmd) {
//    switch (cmd) {
//        case IDE_CMD_READ_SECTORS:
//            begin_read_sequence();
//            break;
//        case IDE_CMD_WRITE_SECTORS:
//#if IDE_ALLOW_WRITE
//            begin_write_sequence();
//#else
//            g_reg_error_features = 0x04;    // ABRT
//            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
//#endif
//            break;
//        default:
//            g_reg_error_features = 0x04;    // ABRT
//            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
//            break;
//    }
//}
//
// ============================================================================
// C) ide_interface.c  -  dispatch_command completo (substitui o anterior)
// ============================================================================
static void dispatch_command(uint8_t cmd) {
    // Comando novo aceito: o ATA manda limpar ERR e o registrador de erro.
    // Sem isso, depois de um ABRT o status fica 0x41 e o proximo comando
    // sobe o DRQ por cima do erro antigo (0x49 = DRDY+DRQ+ERR).
    // BSY ja desde agora, para o 68k nao ler um "ocioso" velho.
    g_reg_error_features = 0x00;
    g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_BSY;

    switch (cmd) {
        case IDE_CMD_READ_SECTORS:
            begin_read_sequence();
            break;

        case IDE_CMD_WRITE_SECTORS:
#if IDE_ALLOW_WRITE
            begin_write_sequence();
#else
            g_reg_error_features = 0x04;
            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
#endif
            break;

        case IDE_CMD_IDENTIFY:
            begin_identify();
            break;

        case IDE_CMD_DIAGNOSTIC:             // EXECUTE DEVICE DIAGNOSTIC
            reset_data_path();
            g_dir = DIR_NONE;
            g_reg_error_features = 0x01;     // 0x01 = "dispositivo ok"
            set_ready_idle();
            break;

        case IDE_CMD_INIT_PARAMS:            // INITIALIZE DEVICE PARAMETERS (CHS)
        case IDE_CMD_SET_FEATURES:           // ex.: desligar write cache
        case IDE_CMD_FLUSH_CACHE:            // nao ha cache: ja esta tudo no SD
        case 0x10 ... 0x1F:                  // RECALIBRATE
        case 0xE0: case 0xE1: case 0xE2:     // STANDBY / IDLE (nada a fazer)
            reset_data_path();
            g_dir = DIR_NONE;
            g_reg_error_features = 0x00;
            set_ready_idle();
            break;

        default:
            g_reg_error_features = 0x04;     // ABRT
            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
            break;
    }
}
 
// -----------------------------------------------------------------------
// IRQ: SM taskfile pediu atencao da CPU (leitura ou escrita de registrador)
// -----------------------------------------------------------------------

// Escrita: {A1..A3, dado} chegou combinado na RX FIFO (19 bits capturados,
// mas so os 3 de endereco + 8 baixos de dado importam aqui).
// AJUSTE o shift abaixo depois de confirmar a ordem real dos bits.
static inline void decode_taskfile_write(uint32_t raw, uint8_t *addr, uint8_t *data) {
    *data = (uint8_t)(raw & 0xFF);
    *addr = (uint8_t)((raw >> 16) & 0x7);   // TODO: confirmar deslocamento real
}

static void pio_taskfile_write_irq_handler(void) {
    while (!pio_sm_is_rx_fifo_empty(g_pio, g_sm_taskfile)) {
        uint32_t raw = pio_sm_get(g_pio, g_sm_taskfile);
        uint8_t addr, data;
        decode_taskfile_write(raw, &addr, &data);

        switch (addr) {
            case IDE_REG_ERROR_FEATURES: g_reg_error_features = data; break;
            case IDE_REG_SECTOR_COUNT:   g_reg_sector_count   = data; break;
            case IDE_REG_LBA_LOW:        g_reg_lba_low        = data; break;
            case IDE_REG_LBA_MID:        g_reg_lba_mid        = data; break;
            case IDE_REG_LBA_HIGH:       g_reg_lba_high       = data; break;
            case IDE_REG_DEVICE_HEAD:    g_reg_device_head    = data; break;
            case IDE_REG_STATUS_CMD:     dispatch_command(data);     break;
            default: break;
        }
    }
    pio_interrupt_clear(g_pio, 0);
}

//static void pio_taskfile_read_irq_handler(void) {
//    while (!pio_sm_is_rx_fifo_empty(g_pio, g_sm_taskfile)) {
//        uint32_t raw = pio_sm_get(g_pio, g_sm_taskfile);
//        uint8_t addr = (uint8_t)((raw >> 16) & 0x7);   // TODO: confirmar deslocamento real
//        uint8_t value;
//
//        switch (addr) {
//            case IDE_REG_ERROR_FEATURES: value = g_reg_error_features; break;
//            case IDE_REG_SECTOR_COUNT:   value = g_reg_sector_count;   break;
//            case IDE_REG_LBA_LOW:        value = g_reg_lba_low;        break;
//            case IDE_REG_LBA_MID:        value = g_reg_lba_mid;        break;
//            case IDE_REG_LBA_HIGH:       value = g_reg_lba_high;       break;
//            case IDE_REG_DEVICE_HEAD:    value = g_reg_device_head;    break;
//            case IDE_REG_STATUS_CMD:     value = g_reg_status;         break;
//            default:                     value = 0xFF;                 break;
//        }
//        pio_sm_put(g_pio, g_sm_taskfile, value);  // libera o "pull" que estava esperando
//    }
//    pio_interrupt_clear(g_pio, 1);
//}
// ============================================================================
// PATCH - ide_interface.c: status correto logo apos a ultima word de uma escrita
// ============================================================================
// O problema: depois que o 68k escreve a ultima word de um setor, o DRQ so cai
// quando a ISR do DMA_C roda (set_busy). Se o 68k ler o status antes disso,
// ve 0x48 (DRQ ainda alto) e pode achar que o Pico quer mais dados.
//
// A solucao: quando o 68k LE o status, o Pico confere se o DMA_C ja terminou o
// setor. Se terminou (canal nao esta mais ocupado) mas a ISR ainda nao rodou,
// o status ja sai com BSY. Assim o 68k nunca ve DRQ "velho".
//
// Substitua pio_taskfile_read_irq_handler() inteira por esta versao.
// So o case IDE_REG_STATUS_CMD mudou.

static void pio_taskfile_read_irq_handler(void) {
    while (!pio_sm_is_rx_fifo_empty(g_pio, g_sm_taskfile)) {
        uint32_t raw = pio_sm_get(g_pio, g_sm_taskfile);
        uint8_t addr = (uint8_t)((raw >> 16) & 0x7);
        uint8_t value;

        switch (addr) {
            case IDE_REG_ERROR_FEATURES: value = g_reg_error_features; break;
            case IDE_REG_SECTOR_COUNT:   value = g_reg_sector_count;   break;
            case IDE_REG_LBA_LOW:        value = g_reg_lba_low;        break;
            case IDE_REG_LBA_MID:        value = g_reg_lba_mid;        break;
            case IDE_REG_LBA_HIGH:       value = g_reg_lba_high;       break;
            case IDE_REG_DEVICE_HEAD:    value = g_reg_device_head;    break;
            case IDE_REG_STATUS_CMD:
                // escrita em andamento, DRQ ainda alto, mas o DMA_C ja recebeu
                // o setor inteiro: mostra BSY ja (a ISR do DMA_C confirma depois)
                if (g_dir == DIR_WRITE &&
                    (g_reg_status & IDE_STATUS_DRQ) &&
                    !dma_channel_is_busy(g_dma_c)) {
                    set_busy();
                }
                value = g_reg_status;
                break;
            default:                     value = 0xFF;                 break;
        }
        pio_sm_put(g_pio, g_sm_taskfile, value);  // libera o "pull" que estava esperando
    }
    pio_interrupt_clear(g_pio, 1);
}

// -----------------------------------------------------------------------
// IRQ: DMA_B terminou de mandar 1 setor pro m68k (leitura)
// -----------------------------------------------------------------------

//static void dma_b_irq_handler(void) {
//    dma_hw->ints0 = 1u << g_dma_b; // limpa flag
//
//    clear_drq();
//    g_buf_ready[g_active_buf] = false;
//    g_lba++;
//
//    if (--g_remaining_sectors == 0) {
//        set_ready_idle();
//        g_dir = DIR_NONE;
//        return;
//    }
//
//    int next_buf = g_active_buf ^ 1;
//    g_active_buf = next_buf;
//
//    if (g_buf_ready[next_buf]) {
//        set_drq();
//        arm_dma_b_from_buffer(next_buf);
//    } else {
//        set_busy();  // ainda esperando o SD — ide_interface_task() libera quando pronto
//    }
//
//    // pede o proximo prefetch pro outro buffer, se ainda faltar mais setor
//    g_need_fetch = true;
//    g_fetch_buf  = next_buf ^ 1;
//    g_fetch_lba  = g_lba + 1;
//}
// DMA_B terminou de entregar 1 setor ao 68k (leitura)
static void dma_b_irq_handler(void) {
    dma_hw->ints0 = 1u << g_dma_b;

    g_buf_ready[g_active_buf] = false;
    g_lba++;

    if (--g_remaining_sectors == 0) {
        set_ready_idle();
        g_dir = DIR_NONE;
        return;
    }

    // ha mais setores: BSY ate o SD entregar o proximo
    g_active_buf ^= 1;
    set_busy();
    g_fetch_buf = g_active_buf;
    g_fetch_lba = g_lba;
    __dmb();
    g_need_fetch = true;
}
// -----------------------------------------------------------------------
// IRQ: DMA_C terminou de receber 1 setor do m68k (escrita)
// -----------------------------------------------------------------------
//static void dma_c_irq_handler(void) {
//    dma_hw->ints1 = 1u << g_dma_c; // limpa flag (canal C no grupo de IRQ1, ver init)
//
//    clear_drq();
//    g_buf_ready[g_active_buf] = true;   // pronto pra ser gravado no SD
//
//    g_need_flush = true;
//    g_flush_buf  = g_active_buf;
//    g_flush_lba  = g_lba;
//    g_lba++;
//
//    if (--g_remaining_sectors == 0) {
//        // ultimo setor: so falta o flush, feito em ide_interface_task()
//        return;
//    }
//
//    int next_buf = g_active_buf ^ 1;
//    g_active_buf = next_buf;
//
//    if (!g_buf_ready[next_buf]) {
//        set_drq();
//        arm_dma_c_into_buffer(next_buf);
//    } else {
//        set_busy(); // outro buffer ainda nao foi esvaziado (gravado no SD)
//    }
//}
// DMA_C terminou de receber 1 setor do 68k (escrita)
static void dma_c_irq_handler(void) {
    dma_hw->ints1 = 1u << g_dma_c;

    set_busy();                             // BSY=1, DRQ=0 ate o SD gravar
    g_buf_ready[0] = true;
    g_flush_buf = 0;
    g_flush_lba = g_lba;
    __dmb();                                // os campos acima antes do flag
    g_need_flush = true;                    // o core1 age a partir daqui
}
// -----------------------------------------------------------------------
// Trabalho "pesado" (bloqueante) — chamar em loop a partir de main/core1
// -----------------------------------------------------------------------
//static void ide_interface_task(void) {
//    if (g_need_fetch) {
//        g_need_fetch = false;
//        int b = g_fetch_buf;
//        uint32_t lba = g_fetch_lba;
//
//        if (ide_hook_sd_read_sector(lba, g_buf[b])) {
//            g_buf_ready[b] = true;
//            if (g_dir == DIR_READ && g_active_buf == b && (g_reg_status & IDE_STATUS_BSY)) {
//                set_drq();
//                arm_dma_b_from_buffer(b);
//            }
//        } else {
//            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
//        }
//    }
//
//    if (g_need_flush) {
//        g_need_flush = false;
//        int b = g_flush_buf;
//        uint32_t lba = g_flush_lba;
//
//        if (!ide_hook_sd_write_sector(lba, g_buf[b])) {
//            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
//        }
//        g_buf_ready[b] = false; // buffer livre de novo
//
//        if (g_dir == DIR_WRITE && (g_reg_status & IDE_STATUS_BSY)) {
//            set_drq();
//            arm_dma_c_into_buffer(g_active_buf);
//        }
//        if (g_remaining_sectors == 0 && g_dir == DIR_WRITE) {
//            set_ready_idle();
//            g_dir = DIR_NONE;
//        }
//    }
//}
static void ide_interface_task(void) {
    if (g_need_fetch) {
        g_need_fetch = false;
        int b = g_fetch_buf;
        uint32_t lba = g_fetch_lba;

        if (ide_hook_sd_read_sector(lba, g_buf[b])) {
            g_buf_ready[b] = true;
            if (g_dir == DIR_READ && g_active_buf == b && (g_reg_status & IDE_STATUS_BSY)) {
                arm_dma_b_from_buffer(b);   // DMA antes do DRQ
                set_drq();
            }
        } else {
            g_reg_error_features = 0x04;
            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
            g_dir = DIR_NONE;
        }
    }

    if (g_need_flush) {
        g_need_flush = false;
        int b = g_flush_buf;
        uint32_t lba = g_flush_lba;

        bool ok = ide_hook_sd_write_sector(lba, g_buf[b]);
        g_buf_ready[b] = false;

        if (!ok) {
            g_reg_error_features = 0x04;
            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
            g_dir = DIR_NONE;
        } else if (--g_remaining_sectors == 0) {
            set_ready_idle();               // so agora o 68k ve "pronto"
            g_dir = DIR_NONE;
        } else {
            g_lba = lba + 1;
            arm_dma_c_into_buffer(0);       // DMA antes do DRQ
            set_drq();                      // proximo setor
        }
    }
}

// -----------------------------------------------------------------------
// Inicializacao
// -----------------------------------------------------------------------
// Substitua a funcao ide_interface_init() inteira no seu ide_interface.c
// por esta. Acrescente tambem as duas variaveis static e a funcao de
// depuracao no final (ide_interface_debug_print_pc).
/*
O que ainda está errado:

1. Bytes trocados em cada word. O Pico é little-endian, e o primeiro byte do setor 
    sai em D0-D7, mas o 68k espera o primeiro byte em D15-D8. A correção é 
    channel_config_set_bswap(&c, true) nos DMAs, digo de memória, então confira no SDK. 
    Faça nos dois, arm_dma_b_from_buffer (leitura) e arm_dma_c_into_buffer (escrita). 
    Não escreva no cartão antes de corrigir os dois: uma escrita com os bytes trocados 
    corrompe o sistema de arquivos. O task file (D0-D7, endereço ímpar) não é afetado.

2. LBA limitado a 24 bits. A tabela de partições mostra uma partição tipo 0C (FAT32 LBA) 
    com cerca de 62 milhões de setores, ou seja, um cartão de uns 32 GB. Com 24 bits só 
    se alcançam 8 GB. Os bits 24 a 27 do LBA vêm do nibble baixo do registrador Device/Head 
    (0xE0 | LBA[27:24]), e o current_lba_from_regs() os ignora. Qualquer setor acima de 8 GB 
    leria o lugar errado. Dá para tratar no Pico em duas linhas. Meu teste também usa só 24 
    bits e precisa do mesmo ajuste.

3. Leitura de mais de um setor trava, pelo prefetch errado no dma_b_irq_handler, que apontei 
    lá atrás. O teste não passa por isso, mas um driver de disco real vai passar.

4. Falta o IDENTIFY DEVICE (0xEC), que a maioria dos drivers IDE usa primeiro. O OrionDOS 
    pode não depender dele, mas eu não sei como o driver MIDE dele inicia.

*/


static uint g_off_data;
static uint g_off_taskfile;

void ide_interface_init(void) {
    printf("ide_interface_init:\n");

    // ------------------------------------------------------------------
    // 1) Buffers de estagio
    // ------------------------------------------------------------------
    for (int i = 0; i < IDE_NUM_BUFFERS; i++) {
        g_buf[i] = ide_hook_psram_buffer(i);
        g_buf_ready[i] = false;
    }

    // ------------------------------------------------------------------
    // 2) Reserva as duas state machines e carrega os programas PIO
    // ------------------------------------------------------------------
    g_pio = pio0;
    g_sm_data     = pio_claim_unused_sm(g_pio, true);
    g_sm_taskfile = pio_claim_unused_sm(g_pio, true);

    g_off_data     = pio_add_program(g_pio, &ide_data_program);
    g_off_taskfile = pio_add_program(g_pio, &ide_taskfile_program);
    printf("ide_interface_init: off_data=%u off_taskfile=%u\n", g_off_data, g_off_taskfile);

    // ------------------------------------------------------------------
    // 3) Entrega TODOS os pinos usados a PIO.
    //    Antes faltavam CS_DATA, CS_TASKFILE, DS e RW (22 a 25): sem isso
    //    a PIO nao enxerga esses sinais e fica presa esperando.
    // ------------------------------------------------------------------
    for (int p = PIN_DBUS_BASE; p < PIN_DBUS_BASE + 16; p++) pio_gpio_init(g_pio, p);  // D0..D15
    for (int p = PIN_AREG_BASE; p < PIN_AREG_BASE + 3;  p++) pio_gpio_init(g_pio, p);  // A1..A3
    pio_gpio_init(g_pio, PIN_CS_DATA);
    pio_gpio_init(g_pio, PIN_CS_TASKFILE);
    pio_gpio_init(g_pio, PIN_DS);
    pio_gpio_init(g_pio, PIN_RW);
    pio_gpio_init(g_pio, PIN_DTACK);

    // ------------------------------------------------------------------
    // 4) SM Data (caminho rapido: registrador Data, 16 bits, via DMA)
    // ------------------------------------------------------------------
    pio_sm_config cfg = ide_data_program_get_default_config(g_off_data);
    sm_config_set_in_pins(&cfg, PIN_DBUS_BASE);
    sm_config_set_out_pins(&cfg, PIN_DBUS_BASE, 16);
    sm_config_set_jmp_pin(&cfg, PIN_RW);
    sm_config_set_sideset_pins(&cfg, PIN_DTACK);
    // Entrada com shift para a ESQUERDA: o pino 0 cai no bit 0 do dado
    // que a SM empurra para a FIFO. (Com shift para a direita o dado
    // caia nos bits altos e o DMA de 16 bits lia zero.)
    sm_config_set_in_shift(&cfg, false, false, 32);
    sm_config_set_out_shift(&cfg, true, false, 16);   // saida: shift direita, sem autopull
    pio_sm_init(g_pio, g_sm_data, g_off_data, &cfg);

    // ------------------------------------------------------------------
    // 5) SM Taskfile (caminho lento: os outros 7 registradores)
    // ------------------------------------------------------------------
    pio_sm_config cfg2 = ide_taskfile_program_get_default_config(g_off_taskfile);
    sm_config_set_in_pins(&cfg2, PIN_DBUS_BASE);      // janela de 22 pinos: D0..D15 + A1..A6
    sm_config_set_out_pins(&cfg2, PIN_DBUS_BASE, 8);  // resposta so em D0..D7
    sm_config_set_jmp_pin(&cfg2, PIN_RW);
    sm_config_set_sideset_pins(&cfg2, PIN_DTACK);
    // Shift para a ESQUERDA: dado em raw[7:0], endereco em raw[18:16],
    // exatamente como decode_taskfile_write() e o handler de leitura esperam.
    sm_config_set_in_shift(&cfg2, false, false, 32);
    sm_config_set_out_shift(&cfg2, true, false, 8);
    pio_sm_init(g_pio, g_sm_taskfile, g_off_taskfile, &cfg2);

    // ------------------------------------------------------------------
    // 6) /DTACK: forca nivel ALTO (solto) ANTES de virar saida.
    //    Se o pino virasse saida antes, ficaria em 0 (assertado).
    // ------------------------------------------------------------------
    pio_sm_set_pins_with_mask(g_pio, g_sm_data,     1u << PIN_DTACK, 1u << PIN_DTACK);
    pio_sm_set_pins_with_mask(g_pio, g_sm_taskfile, 1u << PIN_DTACK, 1u << PIN_DTACK);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_data,     PIN_DTACK, 1, true);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_taskfile, PIN_DTACK, 1, true);

    // ------------------------------------------------------------------
    // 7) Interrupcoes das SMs (registradores do task file)
    // ------------------------------------------------------------------
    pio_interrupt_clear(g_pio, 0);
    pio_interrupt_clear(g_pio, 1);
    pio_set_irq0_source_enabled(g_pio, pis_interrupt0, true); // escrita pendente
    pio_set_irq1_source_enabled(g_pio, pis_interrupt1, true); // leitura pedida
    irq_set_exclusive_handler(PIO0_IRQ_0, pio_taskfile_write_irq_handler);
    irq_set_exclusive_handler(PIO0_IRQ_1, pio_taskfile_read_irq_handler);
    irq_set_enabled(PIO0_IRQ_0, true);
    irq_set_enabled(PIO0_IRQ_1, true);

    // ------------------------------------------------------------------
    // 8) Canais de DMA do caminho rapido (Data)
    // ------------------------------------------------------------------
    g_dma_b = dma_claim_unused_channel(true);
    g_dma_c = dma_claim_unused_channel(true);

    dma_channel_set_irq0_enabled(g_dma_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_b_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_set_irq1_enabled(g_dma_c, true);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_c_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    set_ready_idle();

    // ------------------------------------------------------------------
    // 9) POR ULTIMO: liga as state machines.
    //    Assim, quando o 68k puder falar com elas, as ISRs e os DMAs
    //    ja existem. Antes elas ligavam primeiro, e um ciclo de
    //    barramento no meio da inicializacao nao tinha quem atendesse.
    // ------------------------------------------------------------------
    pio_sm_set_enabled(g_pio, g_sm_data,     true);
    pio_sm_set_enabled(g_pio, g_sm_taskfile, true);
}

// Depuracao: mostra em que instrucao cada SM esta parada (relativo ao
// inicio do programa). Chame de vez em quando a partir do laco do core0.
void ide_interface_debug_print_pc(void) {
    printf("pc data=%u task=%u\n",
           pio_sm_get_pc(g_pio, g_sm_data)     - g_off_data,
           pio_sm_get_pc(g_pio, g_sm_taskfile) - g_off_taskfile);
}


// -----------------------------------------------------------------------
// Ponto de entrada do core1 — chame via:
//   ide_interface_init();
//   multicore_launch_core1(ide_interface_core1_entry);
// no seu main(), rodando no core0. As ISRs de barramento continuam no
// core0 (foram registradas ali dentro de ide_interface_init); so o
// trabalho bloqueante de SD fica isolado no core1.
// -----------------------------------------------------------------------

//void ide_interface_core1_entry(void) 
void __not_in_flash_func(ide_interface_core1_entry)(void) {  
    printf("core1: waiting commands:\n");  
    while (true) {
        ide_interface_task();
        tight_loop_contents();
    }
}
