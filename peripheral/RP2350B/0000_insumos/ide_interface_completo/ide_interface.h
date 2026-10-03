#ifndef IDE_INTERFACE_H
#define IDE_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>

// Chamar uma vez no boot, depois de inicializar clocks/PSRAM/SD.
void ide_interface_init(void);

// Loop de trabalho "pesado" (chama os hooks de SD, que podem bloquear).
// Chame isso repetidamente — o jeito recomendado e passar esta funcao
// pra multicore_launch_core1() logo apos ide_interface_init(), assim
// o core0 fica livre e as chamadas de SD nunca atrasam as ISRs de
// barramento (que continuam rodando no core0).
void ide_interface_core1_entry(void);

// -----------------------------------------------------------------------
// Hooks que voce precisa implementar em outro arquivo, ligando isso ao
// seu driver de SD e ao seu driver de PSRAM ja existentes.
// Sao chamadas fora do caminho critico de tempo (nao seguram /DTACK),
// entao podem ser bloqueantes.
// -----------------------------------------------------------------------

// Le 1 setor (512 bytes) do cartao SD para o buffer indicado.
// Retorna true em sucesso.
bool ide_hook_sd_read_sector(uint32_t lba, uint8_t *dst512);

// Escreve 1 setor (512 bytes) do buffer indicado no cartao SD.
bool ide_hook_sd_write_sector(uint32_t lba, const uint8_t *src512);

// Endereco (ponteiro) de um dos IDE_NUM_BUFFERS buffers de 512 bytes em
// PSRAM, usados como stage entre o SD e a PIO. Se sua PSRAM for acessada
// via ponteiro mapeado (XIP/QMI), isso pode ser so um array estatico
// alocado nessa regiao; ajuste conforme seu driver de PSRAM.
uint8_t *ide_hook_psram_buffer(unsigned buffer_index);

#endif // IDE_INTERFACE_H
