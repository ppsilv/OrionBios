/*
 * test_sector_dump.c -- dumpa o setor 30 inteiro (512 bytes, offset
 * de arquivo 15360) DUAS VEZES seguidas, cada uma com fopen/fclose
 * novos.
 *
 * Objetivo: ver exatamente onde comeca a corrupcao dentro do setor,
 * e se o padrao e' IDENTICO nas duas rodadas (aponta pra bug
 * deterministico -- endereco/cluster errado, ou sempre a mesma area
 * "vazia" do disco sendo lida) ou DIFERENTE (aponta pra race de
 * timing no driver ATA).
 *
 * uso: tstsecdump duart.elf
 */
#include <stdio.h>
#include <inttypes.h>
#include <fileio.h>

static void dump_sector(const char *fname, uint32_t off, int run_num)
{
    FIL f;
    if (fopen(&f, fname, FA_READ) != FR_OK) {
        printf("erro abrindo %s (run %d)\n", fname, run_num);
        return;
    }

    uint8_t buf[512];
    unsigned int n;

    if (flseek(&f, off)) {
        printf("flseek(%lu) falhou (run %d)\n", (unsigned long)off, run_num);
        fclose(&f);
        return;
    }
    if (fread(&f, buf, 512, &n) != FR_OK) {
        printf("fread(%lu) falhou (run %d)\n", (unsigned long)off, run_num);
        fclose(&f);
        return;
    }

    printf("=== RUN %d: setor em offset %lu, n=%u ===\n", run_num, (unsigned long)off, n);
    for (int row = 0; row < 512; row += 16) {
        printf("%4d: ", row);
        for (int c = 0; c < 16; c++)
            printf("%02x ", buf[row + c]);
        printf(" | ");
        for (int c = 0; c < 16; c++) {
            uint8_t b = buf[row + c];
            printf("%c", (b >= 32 && b < 127) ? b : '.');
        }
        printf("\n");
    }

    fclose(&f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("uso: %s arquivo.elf\n", argv[0]);
        return 1;
    }

    /* offset 15360 = 30 * 512, ultimo setor tocado pelo arquivo
     * (arquivo termina em 15408, entao so' os primeiros 48 bytes
     * deste setor sao dados de verdade -- o resto do dump anterior
     * era lixo de pilha, ignorem) */
    dump_sector(argv[1], 15360, 1);
    dump_sector(argv[1], 15360, 2);

    /* NOVO: mesmo teste, mas numa posicao BEM LONGE do fim do
     * arquivo (setor 4, offset 2048), pra ver se o mesmo tipo de
     * corrupcao aparece longe do EOF ou se e' especifico do ultimo
     * setor */
    dump_sector(argv[1], 2048, 3);
    dump_sector(argv[1], 2048, 4);

    return 0;
}