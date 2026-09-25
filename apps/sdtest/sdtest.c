/* ============================================================
 * ls.c - Lista arquivos do diretorio (FAT) para o oriondos
 * Projeto Orion68
 *
 * Uso:
 *   ls              -> lista o diretorio atual, sem ocultos/sistema
 *   ls -a           -> lista tudo, incluindo HID e SYS
 *   ls -w           -> lista so os nomes, em colunas (estilo DOS /w)
 *   ls caminho      -> lista o diretorio informado
 *   ls -a -w caminho -> combina as opcoes
 * ============================================================ */

#include <stdint.h>
#include <stdio.h>
#include <string.h>



#define PICO_SD0_REG0    (*(volatile uint8_t *)0xFF9141)
#define PICO_SD0_REG1    (*(volatile uint8_t *)0xFF9143)


int main(int argc, char *argv[])
{
    uint8_t ch = 0;
    uint8_t buffer[17];
    int j=0;
    //ch = PICO_SD0_REG0;
    printf("Sector 0:\n");
    printf("00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F|0123456789ABCDEF\n");
    printf("-----------------------------------------------|----------------\n");
    for (int i = 0; i < 512; i++)
    {
       // ch = PICO_SD0_REG1;
        printf("%02x ", ch);
        buffer[j++] = ch;
        if ((i % 16) == 15){
            for(j=0;j<=15;j++){
                if (buffer[j] > 0x20 && buffer[j] < 0x80)
                    printf("%c", buffer[j] );
                else    
                    printf("." );
            }
            j=0;
            printf("\n");
        }
    }

    return 0;
}
