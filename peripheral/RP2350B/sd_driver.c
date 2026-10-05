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
/*
Sim, o driver precisa do cartão inserido, e sem ele o your_sd_driver_init() retorna false logo no primeiro comando.

Sem cartão, o MISO fica flutuando ou em pull-up e o CMD0 lê 0xFF. Como o código exige r1 == 0x01, ele sai no primeiro return false, e o main cai no fatal_blink(). Não há detecção de presença (card-detect): o driver descobre que não há cartão só porque ele não responde.

Isso significa que o erro que apareceu no terminal pode ser só "cartão não inserido" ou fio errado, e não necessariamente bug do driver. Para separar os casos, imprima o r1 de cada etapa dentro do init:

c
uint8_t r1 = sd_send_command(CMD0, 0, 0x95);
printf("CMD0 -> 0x%02X\n", r1);

Faça o mesmo para o CMD8 e o último ACMD41. Interpretação:

CMD0 -> 0xFF: o cartão não respondeu. Cartão ausente, MISO/CS/SCK trocados, cartão sem alimentação de 3.3V, ou módulo sem pull-up no MISO.
CMD0 -> 0x01: o cartão está vivo e o protocolo funciona. Se falhar depois, é em CMD8 ou ACMD41.
CMD0 -> 0x00: o cartão já estava fora do idle. Raro, mas dá para tratar como sucesso.

Alguns detalhes do código:

O CMD0 é tentado uma vez só. Muitos cartões precisam de 2 a 5 tentativas na primeira vez. Vale fazer um laço de umas 5 tentativas antes de desistir.
O loop de ACMD41 pode levar até 20 s (20000 × sleep_ms(1)). A especificação fala em cerca de 1 s, então limite a umas 1000 iterações.
Módulos de SD baratos costumam ter regulador e conversor de nível próprios. Confirme que o seu módulo aceita 3.3V nos sinais, já que o RP2350 não tolera 5V nos GPIOs.
O MISO precisa de pull-up (10 a 50 kΩ) se o módulo não tiver, principalmente para o teste sem cartão não ficar lendo lixo. Você também pode ativar gpio_pull_up(PIN_SD_SPI_MISO) logo depois do gpio_set_function.

Teste primeiro com o cartão inserido e com os printf dos r1, e me diga o que o CMD0 devolve. Com isso dá para saber se é fiação ou protocolo.
*/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "ide_config.h"

#include "hardware/platform_defs.h"
_Static_assert(NUM_BANK0_GPIOS == 48, "SDK compilado para 30 GPIOs");


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

    printf("your_sd_driver_init:\n");

    spi_init(SD_SPI_PORT, SD_SPI_INIT_HZ);
    gpio_set_function(PIN_SD_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_SPI_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_SD_SPI_CS);
    gpio_set_dir(PIN_SD_SPI_CS, GPIO_OUT);
    gpio_put(PIN_SD_SPI_CS, 1);

    printf("your_sd_driver_init: GPIO initialized\n");
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
    printf("your_sd_driver_init:CMD0 sent\n");

    // CMD8: SEND_IF_COND — detecta cartoes SDv2 (SDHC/SDXC); arg 0x1AA =
    // padrao de tensao 2.7-3.6V + padrao de verificacao 0xAA
    r1 = sd_send_command(CMD8, 0x1AA, 0x87);
    bool is_v2 = false;
    printf("your_sd_driver_init:CMD8 sent\n");

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
    printf("your_sd_driver_init:CMD55 tried\n");

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
    printf("your_sd_driver_init:CMD58 sent\n");

    sd_cs_deselect();

    // acelera o SPI agora que o cartao ja esta inicializado
    spi_set_baudrate(SD_SPI_PORT, SD_SPI_OPERATE_HZ);
    printf("your_sd_driver_init:baudrate seted...\n");

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

    sd_xfer_byte(0xFF);
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
// ============================================================================
// A) sd_driver.c  -  capacidade do cartao via CMD9 (le o registrador CSD)
// ============================================================================
 
// Converte os 16 bytes do CSD em numero de setores de 512 bytes (0 = invalido).
static uint32_t csd_to_sectors(const uint8_t csd[16]) {
    uint8_t ver = csd[0] >> 6;
    if (ver == 1) {                                   // CSD v2 (SDHC/SDXC)
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                          ((uint32_t)csd[8] << 8) | csd[9];
        return (c_size + 1) * 1024u;                  // (C_SIZE+1) * 512 KiB
    }
    if (ver == 0) {                                   // CSD v1 (SDSC)
        uint32_t read_bl_len = csd[5] & 0x0F;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) |
                          ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
        uint64_t bytes = (uint64_t)(c_size + 1) << (mult + 2);
        bytes <<= read_bl_len;
        return (uint32_t)(bytes / 512u);
    }
    return 0;
}
 
#ifndef CMD9
#define CMD9 (CMD24 - 15)      // mesmo estilo de CMD24 (com ou sem o bit 0x40)
#endif
 
// Devolve a capacidade em setores de 512 bytes, ou 0 se falhar.
uint32_t sd_read_sector_count(void) {
    uint8_t csd[16];
    uint32_t sectors = 0;
 
    if (sd_send_command(CMD9, 0, 0x01) == 0x00) {     // R1 = 0: aceitou
        int ok = 0;
        for (int t = 0; t < 100000; t++) {            // espera o token 0xFE
            if (sd_xfer_byte(0xFF) == 0xFE) { ok = 1; break; }
        }
        if (ok) {
            for (int i = 0; i < 16; i++) csd[i] = sd_xfer_byte(0xFF);
            sd_xfer_byte(0xFF);                       // CRC (ignorado)
            sd_xfer_byte(0xFF);
            sectors = csd_to_sectors(csd);
        }
    }
    sd_cs_deselect();
    sd_xfer_byte(0xFF);                               // 8 clocks extras
    return sectors;
}
 