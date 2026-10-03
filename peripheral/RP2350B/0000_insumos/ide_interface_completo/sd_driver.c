// sd_driver.c
//
// Driver simples de cartao SD em modo SPI, do zero — so o essencial pra
// inicializar o cartao e ler/escrever blocos de 512 bytes (comandos SPI
// classicos: CMD0, CMD8, CMD55/ACMD41, CMD58, CMD17, CMD24).
//
// NAO TESTADO EM HARDWARE — segue o protocolo documentado (referencia
// classica: especificacao SD em modo SPI / elm-chan.org/docs/mmc), mas
// como qualquer coisa de baixo nivel, valide com um cartao real e vá
// isolando problema por problema (voce ja tem prática nisso com o
// oriondos). Pontos mais prováveis de dar trabalho na primeira tentativa:
//   - timing/nivel logico do SPI (cartao SD e 3.3V — confirme que nao
//     esta em 5V em lugar nenhum do caminho)
//   - cartoes SDHC/SDXC vs SDSC antigos (endereçamento por bloco vs byte
//     — ja tratado abaixo, mas e o ponto classico de bug)
//   - clock inicial tem que ser baixo (obrigatorio pela especificacao,
//     ~100-400kHz) antes do cartao estar pronto para o clock normal
//
// Só usa a API padrao "SPI mode" — sem SDIO, sem CRC de dados (a maioria
// dos cartoes aceita CRC de dados desligado no modo SPI; CMD0 e CMD8 SAO
// as duas excecoes que sempre exigem CRC valido, por isso os valores
// fixos 0x95 e 0x87 abaixo).

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "ide_config.h"

// -----------------------------------------------------------------------
// Comandos SD (modo SPI)
// -----------------------------------------------------------------------
#define CMD0    0   // GO_IDLE_STATE
#define CMD8    8   // SEND_IF_COND
#define CMD9    9   // SEND_CSD
#define CMD12   12  // STOP_TRANSMISSION
#define CMD16   16  // SET_BLOCKLEN
#define CMD17   17  // READ_SINGLE_BLOCK
#define CMD24   24  // WRITE_BLOCK
#define CMD55   55  // APP_CMD (prefixo pra comandos ACMD)
#define CMD58   58  // READ_OCR
#define ACMD41  41  // SD_SEND_OP_COND (enviado apos CMD55)

#define DATA_TOKEN_SINGLE_BLOCK 0xFE

static bool g_sd_is_sdhc = false;  // true = endereçamento por bloco (SDHC/SDXC)
static bool g_sd_ready   = false;

// -----------------------------------------------------------------------
// Primitivas de baixo nivel
// -----------------------------------------------------------------------

static inline void sd_cs_select(void) {
    gpio_put(PIN_SD_SPI_CS, 0);
}

static inline void sd_cs_deselect(void) {
    gpio_put(PIN_SD_SPI_CS, 1);
    // 1 byte de clock extra com CS alto, exigido pela especificacao
    // pra o cartao terminar de processar o ultimo comando
    uint8_t dummy = 0xFF;
    spi_write_blocking(SD_SPI_PORT, &dummy, 1);
}

static uint8_t sd_xfer_byte(uint8_t out) {
    uint8_t in = 0xFF;
    spi_write_read_blocking(SD_SPI_PORT, &out, &in, 1);
    return in;
}

// Espera o cartao ficar livre (0xFF repetido). Retorna false em timeout.
static bool sd_wait_ready(uint32_t max_iters) {
    for (uint32_t i = 0; i < max_iters; i++) {
        if (sd_xfer_byte(0xFF) == 0xFF) return true;
    }
    return false;
}

// Envia um comando e devolve o byte de resposta R1.
// Deixa o CS selecionado (quem chamou decide quando desselecionar).
static uint8_t sd_send_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_cs_deselect();
    sd_cs_select();
    if (!sd_wait_ready(50000)) {
        // segue mesmo assim — alguns cartoes aceitam CMD0 mesmo "ocupados"
    }

    uint8_t frame[6] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24),
        (uint8_t)(arg >> 16),
        (uint8_t)(arg >> 8),
        (uint8_t)(arg),
        crc,
    };
    spi_write_blocking(SD_SPI_PORT, frame, sizeof(frame));

    // R1 e o primeiro byte com bit7 == 0; pode levar ate ~8 bytes de clock
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10; i++) {
        r1 = sd_xfer_byte(0xFF);
        if ((r1 & 0x80) == 0) break;
    }
    return r1;
}

// -----------------------------------------------------------------------
// Inicializacao
// -----------------------------------------------------------------------

bool your_sd_driver_init(void) {
    g_sd_ready = false;

    spi_init(SD_SPI_PORT, SD_SPI_INIT_HZ);
    gpio_set_function(PIN_SD_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_SPI_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_SD_SPI_CS);
    gpio_set_dir(PIN_SD_SPI_CS, GPIO_OUT);
    gpio_put(PIN_SD_SPI_CS, 1);

    // >=74 pulsos de clock com CS alto e MOSI alto, antes de qualquer
    // comando — exigido pela especificacao pro cartao "acordar".
    sleep_ms(10);
    for (int i = 0; i < 10; i++) sd_xfer_byte(0xFF);

    // CMD0: GO_IDLE_STATE — precisa CRC valido (0x95), unico caso alem do CMD8
    uint8_t r1 = sd_send_command(CMD0, 0, 0x95);
    if (r1 != 0x01) {
        sd_cs_deselect();
        return false; // cartao nao respondeu / nao entrou em modo SPI
    }

    // CMD8: SEND_IF_COND — detecta cartoes SDv2 (SDHC/SDXC); arg 0x1AA =
    // padrao de tensao 2.7-3.6V + padrao de verificacao 0xAA
    r1 = sd_send_command(CMD8, 0x1AA, 0x87);
    bool is_v2 = false;
    if (r1 == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = sd_xfer_byte(0xFF);
        if (r7[2] == 0x01 && r7[3] == 0xAA) is_v2 = true;
    }
    // se r1 tiver bit ilegal (0x05), e um cartao SDv1 ou MMC antigo —
    // seguimos sem HCS, tratado como SDSC.

    // ACMD41 em loop ate o cartao sair do estado idle (R1 == 0x00).
    // HCS (bit30) sinaliza que aceitamos cartoes de alta capacidade.
    bool inited = false;
    for (int tries = 0; tries < 20000; tries++) {
        sd_send_command(CMD55, 0, 0x01);
        r1 = sd_send_command(ACMD41, is_v2 ? (1UL << 30) : 0, 0x01);
        if (r1 == 0x00) { inited = true; break; }
        sleep_ms(1);
    }
    if (!inited) {
        sd_cs_deselect();
        return false; // cartao nao saiu do idle — nao inicializou
    }

    // CMD58: le OCR pra saber se e SDHC/SDXC (bit CCS = bit30 do OCR)
    if (is_v2) {
        r1 = sd_send_command(CMD58, 0, 0x01);
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = sd_xfer_byte(0xFF);
        if (r1 == 0x00) {
            g_sd_is_sdhc = (ocr[0] & 0x40) != 0; // bit CCS
        }
    } else {
        g_sd_is_sdhc = false;
        // cartoes SDSC precisam do tamanho de bloco fixado explicitamente
        sd_send_command(CMD16, IDE_SECTOR_SIZE_BYTES, 0x01);
    }

    sd_cs_deselect();

    // acelera o SPI agora que o cartao ja esta inicializado
    spi_set_baudrate(SD_SPI_PORT, SD_SPI_OPERATE_HZ);

    g_sd_ready = true;
    return true;
}

// -----------------------------------------------------------------------
// Leitura / escrita de 1 bloco (512 bytes)
// -----------------------------------------------------------------------

static inline uint32_t block_address(uint32_t lba) {
    // SDHC/SDXC enderecam por numero de bloco; SDSC enderecam por byte.
    return g_sd_is_sdhc ? lba : (lba * IDE_SECTOR_SIZE_BYTES);
}

bool your_sd_driver_read_block(uint32_t lba, uint8_t *dst512) {
    if (!g_sd_ready) return false;

    uint8_t r1 = sd_send_command(CMD17, block_address(lba), 0x01);
    if (r1 != 0x00) {
        sd_cs_deselect();
        return false;
    }

    // espera o token de inicio de dados (0xFE), com timeout generoso
    uint8_t token = 0xFF;
    for (int i = 0; i < 200000; i++) {
        token = sd_xfer_byte(0xFF);
        if (token == DATA_TOKEN_SINGLE_BLOCK) break;
    }
    if (token != DATA_TOKEN_SINGLE_BLOCK) {
        sd_cs_deselect();
        return false;
    }

    for (int i = 0; i < IDE_SECTOR_SIZE_BYTES; i++) {
        dst512[i] = sd_xfer_byte(0xFF);
    }
    sd_xfer_byte(0xFF); // CRC (2 bytes) — ignorado, CRC de dados desligado
    sd_xfer_byte(0xFF);

    sd_cs_deselect();
    return true;
}

bool your_sd_driver_write_block(uint32_t lba, const uint8_t *src512) {
    if (!g_sd_ready) return false;

    uint8_t r1 = sd_send_command(CMD24, block_address(lba), 0x01);
    if (r1 != 0x00) {
        sd_cs_deselect();
        return false;
    }

    sd_xfer_byte(DATA_TOKEN_SINGLE_BLOCK);
    for (int i = 0; i < IDE_SECTOR_SIZE_BYTES; i++) {
        sd_xfer_byte(src512[i]);
    }
    sd_xfer_byte(0xFF); // CRC dummy (2 bytes) — CRC de dados desligado
    sd_xfer_byte(0xFF);

    uint8_t resp = sd_xfer_byte(0xFF);
    if ((resp & 0x1F) != 0x05) { // 0x05 = "data accepted"
        sd_cs_deselect();
        return false;
    }

    // espera o cartao terminar de programar a flash interna (fica
    // devolvendo 0x00 enquanto ocupado)
    if (!sd_wait_ready(1000000)) {
        sd_cs_deselect();
        return false;
    }

    sd_cs_deselect();
    return true;
}
