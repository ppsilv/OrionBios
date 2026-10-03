#ifndef IDE_CONFIG_H
#define IDE_CONFIG_H
/*
    GPIO0 -> DO
    GPIO1 -> D1
    GPIO2 -> D2
    GPIO3 -> D3
    GPIO4 -> D4
    GPIO5 -> D5
    GPIO6 -> D6
    GPIO7 -> D7
    GPIO8 -> D8
    GPIO9 -> D9
    GPIO10 -> D1O
    GPIO11 -> D11
    GPIO12 -> D12
    GPIO13 -> D13
    GPIO14 -> D14
    GPIO15 -> D15
    GPIO16 -> A1
    GPIO17 -> A2
    GPIO18 -> A3
    GPIO19 -> A4
    GPIO20 -> A5
    GPIO21 -> A6

*/
// -----------------------------------------------------------------------
// Pin mapping — AJUSTE ESTES NUMEROS conforme o seu esquematico real.
// O barramento de dados de 16 bits e assumido ligado diretamente em
// GPIOs contiguos do RP2350B (sem transceiver externo), no mesmo
// padrao que voce ja usa hoje no link PicoW<->PIO.
// -----------------------------------------------------------------------

#define PIN_GPIO00  0x00
#define PIN_GPIO22  0x22
#define PIN_GPIO23  0x23
#define PIN_GPIO24  0x24
#define PIN_GPIO25  0x25
#define PIN_GPIO26  0x26
#define PIN_GPIO27  0x27
#define PIN_GPIO28  0x28




// Base do barramento de dados (16 pinos contiguos: D0..D15)
#define PIN_DBUS_BASE   PIN_GPIO00

// 3 pinos contiguos, LOGO APOS o barramento de dados, usados para
// selecionar o registrador (A1,A2,A3). Ficam adjacentes ao barramento
// de proposito: assim uma unica "in pins, N" na PIO consegue capturar
// endereco+dado juntos numa so instrucao (ver ide_pio.pio).
// Data = 000, Error/Features = 001, Sector Count = 010,
// LBA Low = 011, LBA Mid = 100, LBA High = 101,
// Device/Head = 110, Status/Command = 111
#define PIN_AREG_BASE   (PIN_DBUS_BASE + 16)   // ocupa 16,17,18

// /CS ja desmembrado em DOIS sinais pelo seu decodificador externo
// (74HC138 + uma porta extra, ou uma segunda saida do 138), um para o
// registrador Data e outro pro grupo dos outros 7 — isso evita ter que
// decodificar endereco dentro da PIO so pra saber qual SM deve responder.
#define PIN_CS_DATA      PIN_GPIO22   // /CS so do registrador Data (indice 0)
#define PIN_CS_TASKFILE  PIN_GPIO23   // /CS dos outros 7 registradores (indices 1..7)

#define PIN_DS          PIN_GPIO24   // strobe de dados combinado (/UDS | /LDS), ativo baixo
#define PIN_RW          PIN_GPIO25   // 1 = leitura pelo m68k, 0 = escrita pelo m68k
#define PIN_DTACK       PIN_GPIO26   // /DTACK, gerado pela PIO

// Pino de IRQ pro m68k (reservado, nao usado ainda — sera IRQ real no futuro)
#define PIN_M68K_IRQ    PIN_GPIO27

// -----------------------------------------------------------------------
// Indices de registrador (A1..A3), casando com o mapa que voce definiu
// -----------------------------------------------------------------------
enum {
    IDE_REG_DATA          = 0,
    IDE_REG_ERROR_FEATURES = 1,
    IDE_REG_SECTOR_COUNT  = 2,
    IDE_REG_LBA_LOW       = 3,
    IDE_REG_LBA_MID       = 4,
    IDE_REG_LBA_HIGH      = 5,
    IDE_REG_DEVICE_HEAD   = 6,
    IDE_REG_STATUS_CMD    = 7,
};

// Bits do registrador de Status (subset classico ATA)
#define IDE_STATUS_BSY   (1u << 7)
#define IDE_STATUS_DRDY  (1u << 6)
#define IDE_STATUS_DRQ   (1u << 3)
#define IDE_STATUS_ERR   (1u << 0)

// Comandos ATA minimos que vamos tratar
#define IDE_CMD_READ_SECTORS   0x20
#define IDE_CMD_WRITE_SECTORS  0x30

#define IDE_SECTOR_SIZE_BYTES  512
#define IDE_SECTOR_SIZE_WORDS  (IDE_SECTOR_SIZE_BYTES / 2)

// Quantos setores manter em voo (buffer duplo = 2)
#define IDE_NUM_BUFFERS  2

#endif // IDE_CONFIG_H
