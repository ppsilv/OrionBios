/*
Depende de onde o seu código chama a inicialização hoje. Não vi essa parte, então descrevo os dois casos.

**Caso 1: você só usa o FatFs.** É o mais simples e o recomendado. Não precisa chamar o `ata_init()` nem o `ata_select_device()` na mão. Cada montagem dispara o `disk_initialize()` da unidade certa:

```c
FATFS fs0, fs1;

FRESULT r0 = f_mount(&fs0, "0:", 1);   /* disk_initialize(0): MIDE / CF   */
FRESULT r1 = f_mount(&fs1, "1:", 1);   /* disk_initialize(1): picoIDE / SD */
```

O `1` no fim força a montagem na hora. Isso faz o `disk_initialize()` rodar já, 
e cada unidade lê o seu MBR e imprime as partições. Se uma unidade falhar, a 
outra continua funcionando, então trate os dois retornos separadamente.

**Caso 2: você chama o `ata_init()` direto no boot.** Antes dos arquivos novos 
isso inicializava o único disco. Agora o `ata_init()` inicializa o dispositivo 
que estiver selecionado, e o padrão é o MIDE (`ATA_DEFAULT_DEVICE`). Se mantiver 
a chamada, selecione antes:

```c
ata_select_device(ATA_DEV_MIDE);    
ata_init();
ata_select_device(ATA_DEV_PICOIDE); 
ata_init();
```

Depois disso o `f_mount` chama o `disk_initialize()` de novo e lê a tabela outra 
vez. É redundante, mas inofensivo. Se quiser, apague essas chamadas diretas e 
fique só com os `f_mount`.

**Ordem e detalhes:**
- Se você usa `set_ide_bus_mode()` para o MIDE, chame-o antes de montar o `0:`. 
O picoIDE ignora esse modo e sempre usa 16 bits.
- Se uma das placas não estiver na máquina, ponha `PDRV_CF_ENABLED` ou 
`PDRV_SD_ENABLED` em 0 no `diskio.c`. Assim o `f_mount` dessa unidade retorna 
erro sem tocar no barramento.
- Os caminhos passam a ter o prefixo da unidade: `"0:/arquivo"` e `"1:/arquivo"`. 
Para trabalhar em uma delas sem repetir o prefixo, use `f_chdrive("1:")`. Isso 
depende do seu `ffconf.h` ter `FF_VOLUMES 2` e `FF_FS_RPATH` ligado para o 
`f_chdrive`.

Se me mostrar o trecho do `main` ou do shell que hoje inicializa e monta o disco, 
adapto exatamente essas linhas.

Está certo do jeito que escreveu. Não precisa mudar nada nessa função: o f_mount 
de cada unidade dispara o disk_initialize() da unidade certa, e uma falha numa 
não impede a outra.

Antes de rodar, confira estes pontos de configuração:

FF_VOLUMES está em 2 no ffconf.h. Em 1, o "1:" volta FR_INVALID_DRIVE sem chegar 
ao diskio.c.
ATA_MAX_DRIVES no ata.h é pelo menos 2. O _Static_assert do ata.c avisa se não 
for.
PDRV_CF_ENABLED e PDRV_SD_ENABLED estão em 1 só para as placas que estão na máquina.
O firmware do Pico está com a correção do ERR (versão nova na serial).

O que deve aparecer no boot com as duas placas:

Para o disco 0, as linhas das partições do MIDE e depois disk 0: success mounted!.
Para o disco 1, ata: picoIDE, 62333952 setores (30436 MB), a partição do SD e 
disk 1: success mounted!.

Um detalhe cosmético: o ata.c imprime ata0: com o número da partição, não do disco. 
As partições do MIDE e do SD vão aparecer as duas como ata0:. Se quiser, mudo as 
mensagens para algo como ata[CF] part0 e ata[SD] part0.

Se o disco 1 falhar, me mande as linhas que apareceram antes do ERROR. As mais 
úteis são ata: no device detected, picoIDE sem IDENTIFY e timeout esperando DRQ, 
porque cada uma aponta para um lugar diferente.
*/
#include <fatfs/ff.h>

FATFS fs0, fs1;

void ide_init(){
    FRESULT fr;
    fr = f_mount(&fs0, "0:", 1);   /* disk_initialize(0): MIDE / CF   */
    if (fr != FR_OK) {
        printf("ERROR: Erro ao montar FAT No disk 0: available\n");
    }else{
        printf(" disk 0: success mounted!\n");
    }
    fr = f_mount(&fs1, "1:", 1);   /* disk_initialize(1): picoIDE / SD */
    if (fr != FR_OK) {
        printf("ERROR: Erro ao montar FAT No disk 1: available\n");
    }else{
        printf(" disk 1: success mounted!\n");
    }
}