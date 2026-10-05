/*
Adaptei o `diskio.c` para duas unidades ao mesmo tempo no FatFs. 
Também atualizei o `ata.c`, porque ele precisava guardar a tabela 
de partições de cada dispositivo. Os dois passam na verificação de 
sintaxe no PC, mas não rodei no 68k.

**Como ficou o mapa de unidades:**
- Unidade `0:` é o MIDE ou Compact Flash.
- Unidade `1:` é o picoIDE com o cartão SD.
- A cada chamada, o `diskio.c` aponta o `ata.c` para o dispositivo 
certo e usa a tabela de partições dele. Antes, tudo usava `drives[0]`, 
que agora é só o MIDE/CF.
- Com um só dispositivo montado, o comportamento é o mesmo de antes.

**O que você precisa fazer:**
1. **`ffconf.h`:** coloque `FF_VOLUMES 2`. Se estiver em 1, o `1:` 
nem chega ao `diskio.c`.
2. **`ata.h`:** `ATA_MAX_DRIVES` precisa ser pelo menos 2. O `ata.c`
 agora tem um `_Static_assert` que acusa isso na compilação. As 
 constantes `ATA_DEV_MIDE` e `ATA_DEV_PICOIDE` estão no `ata.c`; se 
 quiser, mova-as para o `ata.h` e declare lá o `ata_select_device()`. 
 Por enquanto o `diskio.c` as define se faltarem.
3. **No código que monta os volumes:** monte cada um separadamente, 
por exemplo `f_mount(&fs0, "0:", 1)` e `f_mount(&fs1, "1:", 1)`.

**Chaves de segurança no começo do `diskio.c`:** `PDRV_CF_ENABLED` e 
`PDRV_SD_ENABLED`. Deixe em 0 o que não estiver na máquina. Ler uma 
placa ausente pode travar a CPU por falta de /DTACK. Uma unidade 
desligada nunca é acessada e o FatFs só recebe erro.

**Mudanças que fiz além do pedido:**
- `disk_ioctl` agora funciona para as duas unidades. Antes só aceitava 
a unidade 0, e `GET_SECTOR_COUNT` devolvia sempre 0 porque nada preenchia 
o `Stat`. Agora ele é preenchido na inicialização, com o tamanho da 
partição 0 da unidade e setor de 512 bytes.
- A mensagem de erro dentro do `disk_read` dizia `disk_write:`. Corrigi 
o texto.
- Mantive os tipos de retorno e os parâmetros como estavam no seu arquivo 
(por exemplo `DRESULT disk_initialize`), para não conflitar com o seu 
`diskio.h`.

**Uma ressalva:** o `disk_status` continua chamando a detecção do dispositivo 
a cada chamada, e o FatFs chama essa função em quase toda operação de arquivo. 
No MIDE isso já era assim. No picoIDE a detecção é leve, só confere os registradores 
e o status. Se um dia achar lento, dá para guardar o resultado da inicialização 
e responder o `disk_status` sem tocar no barramento.

*/

/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2025        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <fatfs/ff.h>			/* Basic definitions of FatFs */
#include "diskio.h"		/* Declarations FatFs MAI */
#include "ata.h"

typedef unsigned long sector_t;
extern struct ata_drive drives[];
extern int ata_read_sector(int sector, char *buffer);

/* Example: Declarations of the platform and disk functions in the project */
#include "platform.h"
#include "storage.h"

/* ---------------------------------------------------------------------------
 * Unidades fisicas (pdrv) deste sistema. O numero e o do FatFs: "0:" e "1:".
 *   pdrv 0 = MIDE / Compact Flash   (ata.c, ATA_DEV_MIDE)
 *   pdrv 1 = picoIDE / cartao SD    (ata.c, ATA_DEV_PICOIDE)
 *
 * ATENCAO: ligue (1) so o que existe de verdade. Tocar num dispositivo que
 * nao esta na maquina pode travar a CPU (sem /DTACK). Uma unidade desligada
 * aqui nunca e acessada: o FatFs recebe erro sem ler o barramento.
 * --------------------------------------------------------------------------- */
#define DEV_CF		0	/* MIDE / Compact Flash */
#define DEV_SD		1	/* picoIDE / SD card */

#define PDRV_CF_ENABLED		1
#define PDRV_SD_ENABLED		1

#define NUM_PDRV	2

/* Estes dois ficam em ata.h na sua arvore, se preferir (aqui so se faltarem) */
#ifndef ATA_DEV_MIDE
#define ATA_DEV_MIDE		0
#define ATA_DEV_PICOIDE		1
#endif
extern int ata_select_device(int dev);

static const unsigned char pdrv_enabled[NUM_PDRV] = { PDRV_CF_ENABLED, PDRV_SD_ENABLED };
static const unsigned char pdrv_ata_dev[NUM_PDRV] = { ATA_DEV_MIDE,    ATA_DEV_PICOIDE };

#define MAX_DRIVES	10		/* Max number of physical drives to be used */
#define	SZ_BLOCK	128		/* Erase block size to be returned by GET_BLOCK_SIZE command */

typedef struct {
	DSTATUS	status;
	WORD sz_sector;
	LBA_t n_sectors;

} STAT;

static volatile STAT Stat[MAX_DRIVES];

/* Aponta o driver ata.c para o dispositivo desta unidade.
 * Devolve 0 se a unidade nao existe ou esta desligada. */
static int select_pdrv(BYTE pdrv)
{
	if (pdrv >= NUM_PDRV || !pdrv_enabled[pdrv]) {
		return 0;
	}
	return ata_select_device(pdrv_ata_dev[pdrv]);
}

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (BYTE pdrv)		/* Physical drive nmuber to identify the drive */
{
	if (!select_pdrv(pdrv)) {
		return STA_NOINIT;
	}
	return ata_disk_status();	/* 0 = ok, 1 = STA_NOINIT */
}



/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DRESULT disk_initialize ( BYTE pdrv	)			/* Physical drive nmuber to identify the drive */
{
	if (!select_pdrv(pdrv)) {
		return STA_NOINIT;
	}

	DRESULT res = ata_disk_initialize();
	if (res == 0) {
		/* a particao 0 e o que o FatFs enxerga como "o disco" */
		int dev = pdrv_ata_dev[pdrv];
		Stat[pdrv].sz_sector = 512;
		Stat[pdrv].n_sectors = drives[dev].parts[0].size;
		Stat[pdrv].status = 0;
	}
	return res;
}

/* * ESTA É A PONTE: A FatFs vai chamar essa função sempre que precisar de dados.
 * Você deve plugar isso dentro do arquivo 'diskio.c' da FatFs ou mapear adequadamente.
 */
DRESULT disk_read (
    BYTE pdrv,    /* Physical drive nmuber to identify the drive */
    BYTE *buff,   /* Data buffer to store read data */
    LBA_t sector, /* Start sector number (LBA) */
    UINT count    /* Number of sectors to read */
) {
    if (!select_pdrv(pdrv)) {
        printf("disk_read: [%d] this drive does not exist.\n",pdrv);
        return RES_PARERR;
    }

    // Soma a base da partição ativa que o seu MBR leu (da unidade certa)
    sector_t physical_sector = drives[pdrv_ata_dev[pdrv]].parts[0].base + sector;

    // Lê quantos setores a FatFs pedir
    for (UINT i = 0; i < count; i++) {
        if (!ata_read_sector(physical_sector + i, (char *)buff + (i * 512))) {
            return RES_ERROR; // Erro físico de leitura
        }
    }
    return RES_OK;
}


/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0
DRESULT disk_write (
    BYTE pdrv,          /* Physical drive number to identify the drive */
    const char *buff,   /* Data to be written */
    LBA_t sector,       /* Start sector in LBA */
    UINT count          /* Number of sectors to write */
)
{
    if (!select_pdrv(pdrv)) {
        printf("disk_write: [%d] this drive does not exist.\n",pdrv);
        return RES_PARERR;
    }

    // Soma a base da partição ativa na escrita também!
    sector_t physical_sector = drives[pdrv_ata_dev[pdrv]].parts[0].base + sector;

    // Grava quantos setores a FatFs pedir (suportando o count)
    for (UINT i = 0; i < count; i++) {
        if (!ata_write_sector(physical_sector + i, buff + (i * 512))) {
            return RES_ERROR; // Erro físico na escrita
        }
    }

    return RES_OK;
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv >= NUM_PDRV || !pdrv_enabled[pdrv]) {
        return RES_PARERR;  // drive não suportado -- explícito, não silencioso
    }

    DRESULT res = RES_PARERR;  // default: comando desconhecido = erro, não OK

    switch (cmd) {
    case CTRL_SYNC:
        res = RES_OK;       // o picoIDE so devolve "pronto" depois de gravar no SD
        break;
    case GET_SECTOR_COUNT:
        *(LBA_t*)buff = Stat[pdrv].n_sectors;
        res = RES_OK;
        break;
    case GET_SECTOR_SIZE:
        *(WORD*)buff = Stat[pdrv].sz_sector;
        res = RES_OK;
        break;
    case GET_BLOCK_SIZE:
        *(DWORD*)buff = SZ_BLOCK;
        res = RES_OK;
        break;
    }

    return res;
}