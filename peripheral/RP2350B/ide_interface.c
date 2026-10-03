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

// -----------------------------------------------------------------------
// Config dos canais de DMA usados pela SM Data (caminho rapido)
// -----------------------------------------------------------------------

static void arm_dma_b_from_buffer(int buf_index) {
    // Envia 1 setor (256 words) do PSRAM para o TX FIFO da SM Data.
    dma_channel_config c = dma_channel_get_default_config(g_dma_b);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, true /* TX */));
    dma_channel_configure(
        g_dma_b, &c,
        &g_pio->txf[g_sm_data],           // destino: TX FIFO da SM Data
        g_buf[buf_index],                  // origem: buffer em PSRAM
        IDE_SECTOR_SIZE_WORDS,
        true                                // dispara imediatamente
    );
}

static void arm_dma_c_into_buffer(int buf_index) {
    // Recebe 1 setor (256 words) do RX FIFO da SM Data para o PSRAM.
    dma_channel_config c = dma_channel_get_default_config(g_dma_c);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
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

static uint32_t current_lba_from_regs(void) {
    return ((uint32_t)g_reg_lba_high << 16) |
           ((uint32_t)g_reg_lba_mid  << 8)  |
            (uint32_t)g_reg_lba_low;
}

static uint16_t sector_count_from_reg(void) {
    // Convencao ATA classica: 0 no registrador significa 256 setores.
    return g_reg_sector_count == 0 ? 256 : g_reg_sector_count;
}

static void begin_read_sequence(void) {
    g_dir = DIR_READ;
    g_lba = current_lba_from_regs();
    g_remaining_sectors = sector_count_from_reg();
    g_active_buf = 0;
    g_buf_ready[0] = g_buf_ready[1] = false;

    set_busy();
    g_need_fetch = true;
    g_fetch_buf  = 0;
    g_fetch_lba  = g_lba;
}

static void begin_write_sequence(void) {
    g_dir = DIR_WRITE;
    g_lba = current_lba_from_regs();
    g_remaining_sectors = sector_count_from_reg();
    g_active_buf = 0;
    g_buf_ready[0] = g_buf_ready[1] = false;

    // Pronto pra aceitar o primeiro setor vindo do m68k.
    set_drq();
    arm_dma_c_into_buffer(0);
}

static void dispatch_command(uint8_t cmd) {
    switch (cmd) {
        case IDE_CMD_READ_SECTORS:
            begin_read_sequence();
            break;
        case IDE_CMD_WRITE_SECTORS:
            begin_write_sequence();
            break;
        default:
            // comando nao implementado: sinaliza erro, nao trava
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

static void pio_taskfile_read_irq_handler(void) {
    while (!pio_sm_is_rx_fifo_empty(g_pio, g_sm_taskfile)) {
        uint32_t raw = pio_sm_get(g_pio, g_sm_taskfile);
        uint8_t addr = (uint8_t)((raw >> 16) & 0x7);   // TODO: confirmar deslocamento real
        uint8_t value;

        switch (addr) {
            case IDE_REG_ERROR_FEATURES: value = g_reg_error_features; break;
            case IDE_REG_SECTOR_COUNT:   value = g_reg_sector_count;   break;
            case IDE_REG_LBA_LOW:        value = g_reg_lba_low;        break;
            case IDE_REG_LBA_MID:        value = g_reg_lba_mid;        break;
            case IDE_REG_LBA_HIGH:       value = g_reg_lba_high;       break;
            case IDE_REG_DEVICE_HEAD:    value = g_reg_device_head;    break;
            case IDE_REG_STATUS_CMD:     value = g_reg_status;         break;
            default:                     value = 0xFF;                 break;
        }
        pio_sm_put(g_pio, g_sm_taskfile, value);  // libera o "pull" que estava esperando
    }
    pio_interrupt_clear(g_pio, 1);
}

// -----------------------------------------------------------------------
// IRQ: DMA_B terminou de mandar 1 setor pro m68k (leitura)
// -----------------------------------------------------------------------

static void dma_b_irq_handler(void) {
    dma_hw->ints0 = 1u << g_dma_b; // limpa flag

    clear_drq();
    g_buf_ready[g_active_buf] = false;
    g_lba++;

    if (--g_remaining_sectors == 0) {
        set_ready_idle();
        g_dir = DIR_NONE;
        return;
    }

    int next_buf = g_active_buf ^ 1;
    g_active_buf = next_buf;

    if (g_buf_ready[next_buf]) {
        set_drq();
        arm_dma_b_from_buffer(next_buf);
    } else {
        set_busy();  // ainda esperando o SD — ide_interface_task() libera quando pronto
    }

    // pede o proximo prefetch pro outro buffer, se ainda faltar mais setor
    g_need_fetch = true;
    g_fetch_buf  = next_buf ^ 1;
    g_fetch_lba  = g_lba + 1;
}

// -----------------------------------------------------------------------
// IRQ: DMA_C terminou de receber 1 setor do m68k (escrita)
// -----------------------------------------------------------------------

static void dma_c_irq_handler(void) {
    dma_hw->ints1 = 1u << g_dma_c; // limpa flag (canal C no grupo de IRQ1, ver init)

    clear_drq();
    g_buf_ready[g_active_buf] = true;   // pronto pra ser gravado no SD

    g_need_flush = true;
    g_flush_buf  = g_active_buf;
    g_flush_lba  = g_lba;
    g_lba++;

    if (--g_remaining_sectors == 0) {
        // ultimo setor: so falta o flush, feito em ide_interface_task()
        return;
    }

    int next_buf = g_active_buf ^ 1;
    g_active_buf = next_buf;

    if (!g_buf_ready[next_buf]) {
        set_drq();
        arm_dma_c_into_buffer(next_buf);
    } else {
        set_busy(); // outro buffer ainda nao foi esvaziado (gravado no SD)
    }
}

// -----------------------------------------------------------------------
// Trabalho "pesado" (bloqueante) — chamar em loop a partir de main/core1
// -----------------------------------------------------------------------

static void ide_interface_task(void) {
    if (g_need_fetch) {
        g_need_fetch = false;
        int b = g_fetch_buf;
        uint32_t lba = g_fetch_lba;

        if (ide_hook_sd_read_sector(lba, g_buf[b])) {
            g_buf_ready[b] = true;
            if (g_dir == DIR_READ && g_active_buf == b && (g_reg_status & IDE_STATUS_BSY)) {
                set_drq();
                arm_dma_b_from_buffer(b);
            }
        } else {
            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
        }
    }

    if (g_need_flush) {
        g_need_flush = false;
        int b = g_flush_buf;
        uint32_t lba = g_flush_lba;

        if (!ide_hook_sd_write_sector(lba, g_buf[b])) {
            g_reg_status = IDE_STATUS_DRDY | IDE_STATUS_ERR;
        }
        g_buf_ready[b] = false; // buffer livre de novo

        if (g_dir == DIR_WRITE && (g_reg_status & IDE_STATUS_BSY)) {
            set_drq();
            arm_dma_c_into_buffer(g_active_buf);
        }
        if (g_remaining_sectors == 0 && g_dir == DIR_WRITE) {
            set_ready_idle();
            g_dir = DIR_NONE;
        }
    }
}

// -----------------------------------------------------------------------
// Inicializacao
// -----------------------------------------------------------------------
// Substitua a funcao ide_interface_init() inteira no seu ide_interface.c
// por esta. Acrescente tambem as duas variaveis static e a funcao de
// depuracao no final (ide_interface_debug_print_pc).

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
