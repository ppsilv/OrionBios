/*
 * test_isolated.c -- le, LOGO APOS abrir o arquivo, sem nenhum outro
 * flseek/fread antes, os 40 bytes do section header da .shstrtab
 * (indice 10, offset 15368 no duart.elf).
 *
 * Se isso sair CERTO (sh_offset=14900, os 4 bytes em 0x10..0x13
 * devem ser 00 00 3A 34), mas a mesma leitura dá ERRADO depois de
 * uma sequencia de ~20 outros seeks (como em find_got_section),
 * confirma que o bug e' de ESTADO ACUMULADO (cache de setor nao
 * trocando direito apos varios seeks alternados), nao um bug simples
 * de leitura isolada.
 */
#include <stdio.h>
#include <inttypes.h>
#include <fileio.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: %s arquivo.elf\n", argv[0]);
        return 1;
    }

    FIL f;
    if (fopen(&f, argv[1], FA_READ) != FR_OK) {
        printf("erro abrindo %s\n", argv[1]);
        return 1;
    }

    uint8_t hdr[40];
    unsigned int n;

    /* leitura UNICA, primeira coisa que fazemos no arquivo */
    if (flseek(&f, 15368)) { printf("flseek falhou\n"); return 1; }
    if (fread(&f, hdr, 40, &n) != FR_OK) { printf("fread falhou\n"); return 1; }

    uint32_t sh_name   = ((uint32_t)hdr[0]<<24)|((uint32_t)hdr[1]<<16)|((uint32_t)hdr[2]<<8)|hdr[3];
    uint32_t sh_offset = ((uint32_t)hdr[0x10]<<24)|((uint32_t)hdr[0x11]<<16)|((uint32_t)hdr[0x12]<<8)|hdr[0x13];
    uint32_t sh_size   = ((uint32_t)hdr[0x14]<<24)|((uint32_t)hdr[0x15]<<16)|((uint32_t)hdr[0x16]<<8)|hdr[0x17];

    printf("n=%u\n", n);
    printf("sh_name=%lu sh_offset=%lu (esperado 14900) sh_size=%lu (esperado 68)\n",
           (unsigned long)sh_name, (unsigned long)sh_offset, (unsigned long)sh_size);
    printf("bytes 0x10..0x13 (sh_offset): %02x %02x %02x %02x\n",
           hdr[0x10], hdr[0x11], hdr[0x12], hdr[0x13]);

    fclose(&f);
    return 0;
}