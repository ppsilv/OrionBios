/*
 * test_seek.c -- teste ISOLADO, sem nada de ELF, so' pra provar (ou
 * descartar) se o bug esta' em flseek/fread quando volta pra um
 * offset menor que o ja lido antes.
 *
 * Uso: test_seek duart.elf
 *
 * O que ele faz: le 8 bytes em offset 0 (deve ser o magic ELF
 * 7f 45 4c 46 ...), depois le 8 bytes em offset 14968 (comeco da
 * tabela de section headers), depois VOLTA e le 8 bytes em offset
 * 14900 (comeco da .shstrtab -- string table de nomes de secao), e
 * por fim le de novo em 14968 pra ver se depois de ter voltado a
 * leitura pra frente continua certa.
 *
 * Se os bytes de off=14900 saírem errados (nao parecerem nomes de
 * secao tipo ".text" etc), ou se o off=14968 da segunda vez sair
 * diferente do da primeira vez, confirma bug de seek pra tras no
 * fileio.c -- e ai' o proximo passo e' olhar o codigo de flseek()
 * la' dentro, nao mais o elfloader.
 */
#include <stdio.h>
#include <inttypes.h>
#include <fileio.h>

static void dump8(uint8_t *b)
{
    for (int i = 0; i < 8; i++)
        printf("%02x ", b[i]);
    printf(" | ");
    for (int i = 0; i < 8; i++)
        printf("%c", (b[i] >= 32 && b[i] < 127) ? b[i] : '.');
    printf("\n");
}

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

    uint8_t buf[8];
    unsigned int n;

    printf("--- 1) off=0 (esperado: 7f 45 4c 46 = magic ELF) ---\n");
    if (flseek(&f, 0)) { printf("flseek falhou\n"); return 1; }
    if (fread(&f, buf, 8, &n) != FR_OK) { printf("fread falhou\n"); return 1; }
    printf("n=%u  ", n);
    dump8(buf);

    printf("--- 2) off=14968 (comeco da tabela de section headers) ---\n");
    if (flseek(&f, 14968)) { printf("flseek falhou\n"); return 1; }
    if (fread(&f, buf, 8, &n) != FR_OK) { printf("fread falhou\n"); return 1; }
    printf("n=%u  ", n);
    dump8(buf);

    printf("--- 3) off=14900, VOLTANDO (comeco da .shstrtab, deve comecar com 0x00 e depois nomes tipo .text .data etc) ---\n");
    if (flseek(&f, 14900)) { printf("flseek falhou\n"); return 1; }
    if (fread(&f, buf, 8, &n) != FR_OK) { printf("fread falhou\n"); return 1; }
    printf("n=%u  ", n);
    dump8(buf);

    printf("--- 4) off=14968 DE NOVO, pra frente outra vez -- deve ser IDENTICO ao passo 2 ---\n");
    if (flseek(&f, 14968)) { printf("flseek falhou\n"); return 1; }
    if (fread(&f, buf, 8, &n) != FR_OK) { printf("fread falhou\n"); return 1; }
    printf("n=%u  ", n);
    dump8(buf);

    fclose(&f);
    return 0;
}