/*
    Adaptei o seu `ata.c` para atender o MIDE/CF e o picoIDE no mesmo arquivo. 
    Só conferi a sintaxe compilando no PC, não rodei no 68k nem no hardware.

    O que preservei.** O código do MIDE/CF está igual ao seu. O driver só desvia 
    para o código novo quando você escolhe o picoIDE. Os macros de registrador 
    continuam os mesmos, mas agora leem o endereço base de uma variável, 
    `ata_base`.

    Como escolher o dispositivo.** Chame `ata_select_device(ATA_DEV_PICOIDE)` 
    antes de `ata_init()`, ou compile com `-DATA_DEFAULT_DEVICE=1`. O padrão 
    continua sendo o MIDE. Não pus detecção automática de propósito. Ler um 
    endereço sem placa pode travar a CPU por falta de /DTACK. Se quiser, 
    declare `ata_select_device` no `ata.h`.

    Endereços.** O picoIDE fica em `0x00FF4000` e o MIDE em `0x00FF4400`. O 
    espaçamento entre registradores é o mesmo nos dois.

    O que muda no picoIDE:**
    - Dados só em 16 bits, no endereço par.** Acesso de 8 bits não funciona, 
      porque cada leitura do registrador de dados consome uma word inteira. 
      Por isso não uso o `SET FEATURES` de 8 bits.
    - Sem o `rol.w #8`.** O firmware já entrega os bytes na ordem do disco.
    - Atraso curto depois do comando.** O Pico leva alguns microssegundos para 
      assumir o comando, e até lá o status ainda é o antigo. O atraso é 
      `PICOIDE_CMD_DELAY` (50). Ajuste ao clock do seu 68k.
    - Esperas por DRQ e por fim de comando, com timeout.** Uma gravação só 
      vale como pronta quando BSY=0 e DRQ=0. As esperas contam voltas de laço 
      e não ticks, porque dentro do `LOCK` as interrupções estão desligadas e 
      o `get_tick_count()` não avançaria.
    - Buffer em endereço ímpar.** O driver copia por um buffer de apoio, já 
      que o 68000 não aceita acesso de word em endereço ímpar.
    - Transferência pendente na detecção.** Se a detecção encontrar uma 
      transferência pendente (o `0x48` parado que você viu), ela é descartada 
      com uma leitura do setor 0.

    IDENTIFY.** O `ata_init` agora chama o IDENTIFY do picoIDE só para informar 
    a capacidade. Se o firmware ainda não tiver o patch, o driver apenas avisa 
    e continua. Também desviei `ata_read_identity()` para o IDENTIFY do picoIDE.

    Uma correção no código do MIDE.** A mensagem de tamanho da partição fazia 
    `size*512/1000000` em 32 bits, o que estoura num cartão de 32 GB. Troquei 
    por `size/1953`, que dá o mesmo valor sem estourar. No CF de 4 GB o número 
    é praticamente o mesmo.

    Atenção.** O `LOCK` continua cobrindo a transferência inteira. Uma gravação 
    de setor no picoIDE leva o tempo de escrita no SD, então o relógio do sistema 
    pode perder alguns ticks durante ela. Se isso incomodar, dá para soltar o 
    `LOCK` antes da espera final.  
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "interrupt.h"
#include "endian.h"
#include "ata.h"

struct ata_drive drives[ATA_MAX_DRIVES];

extern uint32_t get_tick_count();

/* ---------------------------------------------------------------------------
 * Dispositivos suportados por este driver
 *   ATA_DEV_MIDE     : placa MIDE + Compact Flash (comportamento original)
 *   ATA_DEV_PICOIDE  : picoIDE (RP2350B emulando um disco IDE, SD no Pico)
 * Os dois usam os MESMOS offsets de registrador (passo 2, byte no endereco
 * +1); so muda o endereco base e a forma de transferir os dados.
 * Escolha o dispositivo com ata_select_device() ANTES de ata_init(), ou mude
 * ATA_DEFAULT_DEVICE abaixo. NAO ha deteccao automatica de proposito: ler um
 * endereco onde nao ha placa pode travar a CPU (sem /DTACK).
 * --------------------------------------------------------------------------- */
#define ATA_DEV_MIDE         0
#define ATA_DEV_PICOIDE      1
#define ATA_BASE_MIDE        0x00FF4400UL
#define ATA_BASE_PICOIDE     0x00FF4000UL

#ifndef ATA_DEFAULT_DEVICE
#define ATA_DEFAULT_DEVICE   ATA_DEV_MIDE
#endif

/* drives[ata_dev] guarda a tabela de particoes de cada dispositivo
 * (MIDE/CF em drives[0], picoIDE em drives[1]); exige ATA_MAX_DRIVES >= 2. */
_Static_assert(ATA_MAX_DRIVES >= 2, "ata.h: ATA_MAX_DRIVES precisa ser pelo menos 2");

static int      ata_dev  = ATA_DEFAULT_DEVICE;
static uint32_t ata_base = (ATA_DEFAULT_DEVICE == ATA_DEV_PICOIDE) ? ATA_BASE_PICOIDE : ATA_BASE_MIDE;
static uint32_t ata_total_sectors = 0;   /* capacidade informada pelo picoIDE (0 = desconhecida) */

#define ATA_REG_BASE (ata_base)          /* os macros de registrador abaixo continuam iguais */

/* nome do dispositivo selecionado, para as mensagens: "ata[MIDE/CF]: ..." */
static const char *ata_name(void)
{
	return (ata_dev == ATA_DEV_PICOIDE) ? "picoIDE/SD" : "MIDE/CF";
}

#define ATA_DELAY(x)		{ for (int delay = 0; delay < (x); delay++) { asm volatile(""); } }
#define ATA_WAIT_FOR_DATA()	{ while (!((*ATA_REG_STATUS) & ATA_ST_DATA_READY)) { } }
#define ATA_WAIT()		{ ATA_DELAY(4); while (*ATA_REG_STATUS & ATA_ST_BUSY) { } }
#define LOCK(saved) {                   \
    asm("move.w %%sr, %0\n" : "=dm" ((saved))); \
    m68k_disable_all_interrupts();                 \
}

#define UNLOCK(saved) {                     \
    m68k_enable_all_interrupts(); \
}


#define ATA_REG_DEV_CONTROL	((volatile uint8_t *) (ATA_REG_BASE + 0x1c + 1))
#define ATA_REG_DEV_ADDRESS	((volatile uint8_t *) (ATA_REG_BASE + 0x1e + 1))

#define ATA_REG_DATA		((volatile uint16_t *) (ATA_REG_BASE + 0x0 + 1))
#define ATA_REG_DATA_BYTE	((volatile uint8_t *) (ATA_REG_BASE + 0x0 + 1))
#define ATA_REG_FEATURE		((volatile uint8_t *) (ATA_REG_BASE + 0x2 + 1))
#define ATA_REG_ERROR		((volatile uint8_t *) (ATA_REG_BASE + 0x2 + 1))
#define ATA_REG_SECTOR_COUNT	((volatile uint8_t *) (ATA_REG_BASE + 0x4 + 1))
#define ATA_REG_SECTOR_NUM	((volatile uint8_t *) (ATA_REG_BASE + 0x6 + 1))
#define ATA_REG_CYL_LOW		((volatile uint8_t *) (ATA_REG_BASE + 0x8 + 1))
#define ATA_REG_CYL_HIGH	((volatile uint8_t *) (ATA_REG_BASE + 0xa + 1))
#define ATA_REG_DRIVE_HEAD	((volatile uint8_t *) (ATA_REG_BASE + 0xc + 1))
#define ATA_REG_STATUS		((volatile uint8_t *) (ATA_REG_BASE + 0xe + 1))
#define ATA_REG_COMMAND		((volatile uint8_t *) (ATA_REG_BASE + 0xe + 1))

#define ATA_CMD_READ_SECTORS	0x20
#define ATA_CMD_WRITE_SECTORS	0x30
#define ATA_CMD_IDENTIFY	0xEC
#define ATA_CMD_SET_FEATURE	0xEF

#define ATA_ST_BUSY     	0x80    // BSY
#define ATA_ST_DRDY     	0x40    // Drive Ready
#define ATA_ST_DF       	0x20    // Device Fault
#define ATA_ST_DSC      	0x10    // Seek Complete (obsoleto, mas alguns CF ainda setam)
#define ATA_ST_DATA_READY	0x08    // Data Request (tem dado pronto pra transferir)
#define ATA_ST_ERROR    	0x01    // Error

#define log_notice printf
#define log_info   printf
#define log_error  printf

char ide_bus_mode=0;

void set_ide_bus_mode(char mode){
	if(mode > 1 || mode <0){
		printf("This mode does not exist...[%d]\n",mode);
		return;
	}
	ide_bus_mode=mode;
}
void go_8bits_mode()
{
	if(ide_bus_mode == 0){
		// Set 8-bit mode
		(*ATA_REG_FEATURE) = 0x01;
		(*ATA_REG_COMMAND) = ATA_CMD_SET_FEATURE;
		ATA_WAIT();
	}
	if(ide_bus_mode == 1){ // Ou a lógica que você usar para mudar para 16-bit
		// Set 16-bit mode (Disable 8-bit PIO)
		(*ATA_REG_FEATURE) = 0x81; 
		(*ATA_REG_COMMAND) = ATA_CMD_SET_FEATURE;
		ATA_WAIT();
	}	
}
/* Ajuste esse valor conforme a frequência da sua rotina de delay.
 * Datasheets de CF tipicamente pedem até 30s no pior caso (spin-up),
 * mas CF de estado sólido normalmente responde em poucos ms. 
 */
#define ATA_TIMEOUT_LOOPS   1000000UL
/*
static int ata_wait_busy_clear(void){
    unsigned long timeout = ATA_TIMEOUT_LOOPS;
    uint8_t status;
    do {
        status = *ATA_REG_STATUS;
        if (!(status & ATA_ST_BUSY)) {
            return 1;   // saiu de BUSY, sucesso 
        }
        timeout--;
    } while (timeout > 0);
    return 0;   // timeout - disco não respondeu a tempo 
}*/
static int ata_wait_busy_clear_timed(uint32_t timeout_ms){
    uint32_t start = get_tick_count();   /* ajuste ao nome real da sua API de RTC */
    uint8_t status;
    do {
        status = *ATA_REG_STATUS;
        if (!(status & ATA_ST_BUSY)) {
            return 1;
        }
    } while ((get_tick_count() - start) < (timeout_ms/10));
    return 0;
}
static int has_master_disk4(void){
    uint8_t status;
    *ATA_REG_DRIVE_HEAD = 0xA0;
    for (volatile int i = 0; i < 100; i++);
    /* Teste de assinatura */
    *ATA_REG_SECTOR_COUNT = 0x55;
    *ATA_REG_SECTOR_NUM   = 0xAA;
    if (*ATA_REG_SECTOR_COUNT != 0x55 || *ATA_REG_SECTOR_NUM != 0xAA) {
        return 0;
    }
    *ATA_REG_SECTOR_COUNT = 0xAA;
    *ATA_REG_SECTOR_NUM   = 0x55;
    if (*ATA_REG_SECTOR_COUNT != 0xAA || *ATA_REG_SECTOR_NUM != 0x55) {
        return 0;
    }
    status = *ATA_REG_STATUS;
    if (status == 0xFF) {
        return 0;   /* barramento flutuando, nada respondendo */
    }
    /* Disco parece presente (respondeu à assinatura), mas pode
     * ainda estar em power-on / self-test. Espera sair de BUSY. */
    if (!ata_wait_busy_clear_timed(1000)) {
        return 0;   /* presente mas nunca ficou pronto -> trate como falha */
    }
    return 1;   /* disco presente e pronto */
}
static int has_master_disk3(void){
    uint8_t status;
    /* Seleciona drive 0 (master) antes de testar - alguns
     * controladores só respondem no barramento se DRIVE_HEAD
     * estiver setado corretamente (bit 5 e 7 sempre em 1 por
     * compatibilidade, LBA=0, drive=0) */
    *ATA_REG_DRIVE_HEAD = 0xA0;
    /* pequeno delay pro barramento estabilizar após troca de drive */
    for (volatile int i = 0; i < 100; i++);
    /* Teste de assinatura: escreve e relê SECTOR_COUNT */
    *ATA_REG_SECTOR_COUNT = 0x55;
    *ATA_REG_SECTOR_NUM   = 0xAA;
    if (*ATA_REG_SECTOR_COUNT != 0x55 || *ATA_REG_SECTOR_NUM != 0xAA) {
        return 0;   /* não gravou -> barramento flutuando, sem disco */
    }
    *ATA_REG_SECTOR_COUNT = 0xAA;
    *ATA_REG_SECTOR_NUM   = 0x55;
    if (*ATA_REG_SECTOR_COUNT != 0xAA || *ATA_REG_SECTOR_NUM != 0x55) {
        return 0;
    }
    /* Confirma que STATUS não está com o barramento flutuando */
    status = *ATA_REG_STATUS;
    if (status == 0xFF) {
        return 0;
    }
    return 1;   /* dispositivo presente */
}
static int has_master_disk(void){
    // escreve um valor de teste em um registrador gravável
    *ATA_REG_SECTOR_COUNT = 0x55;
    if (*ATA_REG_SECTOR_COUNT != 0x55) {
        return 0;  // não gravou = sem disco
    }
    *ATA_REG_SECTOR_COUNT = 0xAA;
    if (*ATA_REG_SECTOR_COUNT != 0xAA) {
        return 0;
    }
    // registrador responde normalmente, dispositivo presente
    return 1;
}
static int has_master_disk2(void){
    uint8_t status;
    // teste de assinatura
    *ATA_REG_SECTOR_COUNT = 0x55;
    *ATA_REG_SECTOR_COUNT = 0xAA;
    if (*ATA_REG_SECTOR_COUNT != 0xAA) {
        return 0;
    }
    // confirma que status não está "flutuando"
    status = *ATA_REG_STATUS;
    if (status == 0xFF) {
        return 0;
    }
    return 1;
}
/* ===========================================================================
 *  picoIDE
 * ===========================================================================
 * Diferencas em relacao ao MIDE/CF:
 *   - Dados SO em 16 bits, no endereco PAR (base+0). Acesso de 8 bits nao
 *     serve: cada acesso ao registrador de dados consome uma word inteira.
 *   - O firmware ja entrega os bytes na ordem do disco (bswap no DMA), entao
 *     NAO ha o "rol.w #8" do modo 16 bits do MIDE.
 *   - Nao se usa SET FEATURES (8 bits) nem se consulta o status a cada word.
 *   - Logo depois de escrever um comando, o status ainda pode ser o ANTIGO por
 *     alguns microssegundos (o Pico leva um tempo para ver o comando). Por
 *     isso ha um pequeno atraso antes de olhar o status, e a espera so
 *     considera "pronto" quando BSY=0 e DRQ=0 (fim de escrita) ou BSY=0 e
 *     DRQ=1 (pedido de dados).
 *   - As esperas contam voltas de laco, nao ticks: dentro de LOCK as
 *     interrupcoes estao desligadas e get_tick_count() nao andaria.
 */
#define PICOIDE_DATA16          ((volatile uint16_t *) (ata_base + 0))
#define PICOIDE_TIMEOUT_LOOPS   2000000UL    /* ~ alguns segundos; cobre a escrita no SD */
#define PICOIDE_CMD_DELAY       50           /* ajuste ao clock do seu 68k (~50 a 100 us) */

/* espera BSY=0 e (DRQ ou ERR). Devolve 1 se DRQ sem erro, 0 se erro/timeout */
static int picoide_wait_drq(void)
{
    unsigned long t = PICOIDE_TIMEOUT_LOOPS;
    uint8_t st;
    while (t--) {
        st = *ATA_REG_STATUS;
        if (st & ATA_ST_BUSY) continue;
        if (st & ATA_ST_ERROR)      return 0;
        if (st & ATA_ST_DATA_READY) return 1;
    }
    printf("ata[%s]: timeout waiting for DRQ\n", ata_name());
    return 0;
}

/* espera BSY=0 e DRQ=0. Devolve 1 se terminou sem erro, 0 se erro/timeout */
static int picoide_wait_idle(void)
{
    unsigned long t = PICOIDE_TIMEOUT_LOOPS;
    uint8_t st;
    while (t--) {
        st = *ATA_REG_STATUS;
        if (st & (ATA_ST_BUSY | ATA_ST_DATA_READY)) continue;
        return (st & ATA_ST_ERROR) ? 0 : 1;
    }
    printf("ata[%s]: timeout waiting for command completion\n", ata_name());
    return 0;
}

static void picoide_command(uint8_t cmd)
{
    *ATA_REG_COMMAND = cmd;
    ATA_DELAY(PICOIDE_CMD_DELAY);   /* deixa o Pico assumir o comando antes de ler o status */
}

static void picoide_set_lba(int sector)
{
    *ATA_REG_DRIVE_HEAD   = 0xE0 | (uint8_t) ((sector >> 24) & 0x0F);
    *ATA_REG_CYL_HIGH     = (uint8_t) (sector >> 16);
    *ATA_REG_CYL_LOW      = (uint8_t) (sector >> 8);
    *ATA_REG_SECTOR_NUM   = (uint8_t) sector;
    *ATA_REG_SECTOR_COUNT = 1;
}

/* buffer de apoio para destinos/origens em endereco IMPAR (68000 nao aceita
 * acesso de word em endereco impar) */
static uint16_t picoide_bounce[256];

static int picoide_read_sector(int sector, char *buffer)
{
    short saved_status;
    uint16_t *dst = ((uintptr_t) buffer & 1) ? picoide_bounce : (uint16_t *) buffer;

    LOCK(saved_status);

    picoide_set_lba(sector);
    picoide_command(ATA_CMD_READ_SECTORS);

    if (!picoide_wait_drq()) {
        log_error("ata[%s]: error reading sector %d: status=%x error=%x\n", ata_name(), sector, *ATA_REG_STATUS, *ATA_REG_ERROR);
        UNLOCK(saved_status);
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        dst[i] = *PICOIDE_DATA16;
    }
    if (!picoide_wait_idle()) {
        log_error("ata[%s]: error finishing read of sector %d: status=%x error=%x\n", ata_name(), sector, *ATA_REG_STATUS, *ATA_REG_ERROR);
        UNLOCK(saved_status);
        return 0;
    }
    UNLOCK(saved_status);

    if (dst == picoide_bounce) memcpy(buffer, picoide_bounce, 512);
    return 512;
}

static int picoide_write_sector(int sector, const char *buffer)
{
    short saved_status;
    const uint16_t *src = (const uint16_t *) buffer;

    if ((uintptr_t) buffer & 1) {
        memcpy(picoide_bounce, buffer, 512);
        src = picoide_bounce;
    }

    LOCK(saved_status);

    picoide_set_lba(sector);
    picoide_command(ATA_CMD_WRITE_SECTORS);

    if (!picoide_wait_drq()) {
        log_error("ata[%s]: error starting write of sector %d: status=%x error=%x\n", ata_name(), sector, *ATA_REG_STATUS, *ATA_REG_ERROR);
        UNLOCK(saved_status);
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        *PICOIDE_DATA16 = src[i];
    }
    /* so vale como "gravado" quando o Pico termina no SD: BSY=0 e DRQ=0 */
    if (!picoide_wait_idle()) {
        log_error("ata[%s]: error writing sector %d: status=%x error=%x\n", ata_name(), sector, *ATA_REG_STATUS, *ATA_REG_ERROR);
        UNLOCK(saved_status);
        return 0;
    }
    UNLOCK(saved_status);
    return 512;
}

/* IDENTIFY DEVICE do picoIDE: confere o bloco e guarda a capacidade.
 * Devolve 1 se ok, 0 se o firmware nao suporta (nao e fatal para o resto). */
static int picoide_identify(void)
{
    uint16_t id[256];
    const uint8_t *b = (const uint8_t *) id;
    uint8_t sum = 0;

    *ATA_REG_DRIVE_HEAD = 0xE0;
    picoide_command(ATA_CMD_IDENTIFY);
    if (!picoide_wait_drq()) {
        printf("ata[%s]: IDENTIFY not supported (old firmware?)\n", ata_name());
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        id[i] = *PICOIDE_DATA16;
    }
    if (!picoide_wait_idle()) return 0;

    /* os bytes chegam como num PC: cada word ATA com o byte baixo primeiro */
    for (int i = 0; i < 512; i++) sum += b[i];
    if (b[510] != 0xA5 || sum != 0) {
        printf("ata[%s]: IDENTIFY block has invalid checksum\n", ata_name());
        return 0;
    }
    ata_total_sectors = (uint32_t) b[120] | ((uint32_t) b[121] << 8) |
                        ((uint32_t) b[122] << 16) | ((uint32_t) b[123] << 24);   /* words 60 e 61 */
    log_info("ata[%s]: %d sectors (%d MB)\n", ata_name(), (int) ata_total_sectors, (int) (ata_total_sectors / 2048));
    return 1;
}

/* a placa responde? (registradores gravaveis + status plausivel). Se uma
 * transferencia ficou pendente (DRQ), descarta-a com uma leitura do setor 0. */
static int picoide_detect(void)
{
    uint8_t st;

    if (!has_master_disk()) return 0;
    st = *ATA_REG_STATUS;
    if (st == 0xFF) return 0;

    if (st & ATA_ST_DATA_READY) {
        static char scratch[512];
        printf("ata[%s]: transfer left pending, clearing\n", ata_name());
        picoide_read_sector(0, scratch);
        st = *ATA_REG_STATUS;
    }
    return (st & ATA_ST_BUSY) ? 0 : 1;
}

/* Escolhe o dispositivo ANTES de ata_init(). Devolve 1 se valido. */
int ata_select_device(int dev)
{
    if (dev == ATA_DEV_MIDE)    { ata_dev = dev; ata_base = ATA_BASE_MIDE;    return 1; }
    if (dev == ATA_DEV_PICOIDE) { ata_dev = dev; ata_base = ATA_BASE_PICOIDE; return 1; }
    printf("ata: unknown device [%d] (0=MIDE/CF, 1=picoIDE/SD)\n", dev);
    return 0;
}

int ata_detect(void){
	uint8_t status;

	if (ata_dev == ATA_DEV_PICOIDE) {
		return picoide_detect();
	}

	if( ! has_master_disk() ){
		return 0;
	}
	ATA_DELAY(10);

	// Reset the IDE bus
	(*ATA_REG_COMMAND) = ATA_CMD_IDENTIFY;

	for (int i = 0; i < 1000; i++) {
		ATA_DELAY(10);
		status = *ATA_REG_STATUS;
		// If it becomes unbusy within the timeout then a drive is connected
		if (!(status & ATA_ST_BUSY)) {
			if (status & ATA_ST_DATA_READY) {
				ATA_DELAY(100);
				return 1;
			} else {
				printf("ata[%s]: data not ready\n", ata_name());
				return 0;
			}
		}
	}
	printf("ata[%s]: timeout waiting for identify command\n", ata_name());
	return 0;
}

int ata_read_sector(int sector, char *buffer)
{
	short saved_status;

	if (ata_dev == ATA_DEV_PICOIDE) {
		return picoide_read_sector(sector, buffer);
	}

	//printf("ata.c: Reading sector[%d] to buffer[%ld]\n",sector,(long *)buffer);

	LOCK(saved_status);

	
	(*ATA_REG_DRIVE_HEAD) = 0xE0 | (uint8_t) ((sector >> 24) & 0x0F);

	go_8bits_mode();
	/*
	// Set 8-bit mode
	(*ATA_REG_FEATURE) = 0x01;
	(*ATA_REG_COMMAND) = ATA_CMD_SET_FEATURE;
	ATA_WAIT();
	*/

	// Read a sector
	(*ATA_REG_CYL_HIGH) = (uint8_t) (sector >> 16);
	(*ATA_REG_CYL_LOW) = (uint8_t) (sector >> 8);
	(*ATA_REG_SECTOR_NUM) = (uint8_t) sector;
	(*ATA_REG_SECTOR_COUNT) = 1;
	(*ATA_REG_COMMAND) = ATA_CMD_READ_SECTORS;
	ATA_WAIT();

	char status = (*ATA_REG_STATUS);
	if (status & 0x01) {
		log_error("ata[%s]: error reading sector %d: error=%x\n", ata_name(), sector, (*ATA_REG_ERROR));
		UNLOCK(saved_status);
		return 0;
	}

	ATA_WAIT();
	ATA_WAIT_FOR_DATA();

	if( ide_bus_mode == 0 ){
		for (int i = 0; i < 512; i++ ) {
			buffer[i] = (*ATA_REG_DATA_BYTE);

			ATA_WAIT();
		}
	}else{
		for (int i = 0; i < 256; i++ ) {
			((uint16_t *) buffer)[i] = (*ATA_REG_DATA);
			asm volatile("rol.w	#8, %0\n" : "+g" (((uint16_t *) buffer)[i]));

			ATA_WAIT();
		}
	}
	UNLOCK(saved_status);
	return 512;
}

int ata_write_sector(int sector, const char *buffer)
{
	short saved_status;

	if (ata_dev == ATA_DEV_PICOIDE) {
		return picoide_write_sector(sector, buffer);
	}

	LOCK(saved_status);

	go_8bits_mode();
	/*
	// Set 8-bit mode
	(*ATA_REG_FEATURE) = 0x01;
	(*ATA_REG_COMMAND) = ATA_CMD_SET_FEATURE;
	ATA_WAIT();
	*/

	// Read a sector
	(*ATA_REG_DRIVE_HEAD) = 0xE0 | (uint8_t) ((sector >> 24) & 0x0F);
	(*ATA_REG_CYL_HIGH) = (uint8_t) (sector >> 16);
	(*ATA_REG_CYL_LOW) = (uint8_t) (sector >> 8);
	(*ATA_REG_SECTOR_NUM) = (uint8_t) sector;
	(*ATA_REG_SECTOR_COUNT) = 1;
	(*ATA_REG_COMMAND) = ATA_CMD_WRITE_SECTORS;
	ATA_WAIT();

	char status = (*ATA_REG_STATUS);
	//printk("IDE: %x\n", status);
	if (status & 0x01) {
		log_error("ata[%s]: error starting write of sector %d: error=%x\n", ata_name(), sector, (*ATA_REG_ERROR));
		UNLOCK(saved_status);
		return 0;
	}

	ATA_DELAY(100);
	ATA_WAIT();
	ATA_WAIT_FOR_DATA();

	for (int i = 0; i < 512; i++) {
		ATA_WAIT();
		//while (((*ATA_REG_STATUS) & ATA_ST_BUSY) || !((*ATA_REG_STATUS) & ATA_ST_DATA_READY)) { }

		//((uint16_t *) buffer)[i] = (*ATA_REG_DATA);
		//asm volatile("rol.w	#8, %0\n" : "+g" (((uint16_t *) buffer)[i]));
		(*ATA_REG_DATA_BYTE) = buffer[i];
	}

	ATA_WAIT();

	if (*ATA_REG_STATUS & ATA_ST_ERROR) {
		log_error("ata[%s]: error writing sector %d: error=%x\n", ata_name(), sector, *ATA_REG_ERROR);
	}

	UNLOCK(saved_status);

	return 512;
}



struct partition_entry {
    uint8_t status;
    uint8_t chs_start[3];
    uint8_t fstype;
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t lba_sectors;
};


int read_partition_table(char *buffer, struct partition *devices)
{
    struct partition_entry *table;

    table = (struct partition_entry *) &buffer[0x1BE];

    for (short i = 0; i < 4; i++) {
        devices[i].base = le32toh(table[i].lba_start);
        devices[i].size = le32toh(table[i].lba_sectors);
        devices[i].fstype = table[i].fstype;
        devices[i].flags = table[i].status;
    }

    return 0;
}

int ata_init(void)
{

	for (short i = 0; i < PARTITION_MAX; i++) {
		drives[ata_dev].parts[i].base = 0;
		drives[ata_dev].parts[i].size = 0;
	}

	// TODO this doesn't work very well
	if (!ata_detect()) {
		log_info("ata[%s]: no device detected\n", ata_name());
		return 0;
	}else{
		//log_info("ata: device detected\n");
	}

	if (ata_dev == ATA_DEV_PICOIDE) {
		picoide_identify();   /* informativo; nao e fatal se o firmware nao tiver */
	}

	char *buffer=( char *)0xA2000;

	ata_read_sector(0, buffer);
	read_partition_table(buffer, drives[ata_dev].parts);

	for (short i = 0; i < PARTITION_MAX; i++) {
		if (drives[ata_dev].parts[i].size) {
			log_notice("ata[%s]: partition %d: %d sectors\n", ata_name(), i, drives[ata_dev].parts[i].size);
			log_notice("ata[%s]: partition %d: %d Mb\n", ata_name(), i, drives[ata_dev].parts[i].size/1953);   /* = size*512/1000000 sem estourar 32 bits */
		}
	}
	return 1;
}

int ata_disk_status(){
	if( ata_detect() ){
		return 0; //RES_OK
	}
	return 1; //RES_ERROR
}

int ata_disk_initialize(){
	if( ata_init() ){
		return 0; //RES_OK
	}
	return 1; //RES_ERROR
}

void dump_memory(void * addr,int size);
#define ATA_TIMEOUT             100000

int ata_wait_not_busy()
{
    volatile uint32_t timeout = ATA_TIMEOUT;
    do
    {
        uint8_t status = (*ATA_REG_STATUS);
        if ((status & ATA_ST_BUSY) == 0)
            return 0;
    }
    while (timeout--);
    printf("ata_wait_not_busy: timeout\n");
    return 1;
}

int ata_wait_for_data()
{
    volatile uint32_t timeout = ATA_TIMEOUT;
    do
    {
        uint8_t status = (*ATA_REG_STATUS);
        if ((status & ATA_ST_BUSY) == 0)
            return 0;
    }
    while (timeout--);

    printf("ata_wait_for_data: timeout\n");
    return 1;
}


int ata_read_identity(void)
{

    printf("ata_read_identity: function entry\n");

    if (ata_dev == ATA_DEV_PICOIDE) {
        return picoide_identify() ? 0 : 1;   /* 0 = ok, como no original */
    }

	if (ata_wait_not_busy())
    {
        printf("ata_read_identity: ata_wait_for_data (1) error\n");
        return 1;
    }

	(*ATA_REG_SECTOR_COUNT) = 1;
	(*ATA_REG_COMMAND) = ATA_CMD_IDENTIFY;
	if (ata_wait_for_data())
    {
        printf("ata_read_identity: ata_wait_for_data (1) error\n");
        return 1;
    }

	uint8_t status = (*ATA_REG_STATUS);
	if (status & ATA_ST_ERROR)
    {
        printf("ata_read_identity: disk status error - status = %02X\n", status);
        return 1;
    }

	if (ata_wait_not_busy())
    {
        printf("ata_read_identity: ata_wait_not_busy (2) error\n");
        return 1;
    }

	printf("ata_read_identity: reading data....");
	uint8_t drives[1024];
    uint8_t *ptr8 = (uint8_t *)&drives;

    for (int i = 0; i < 512; i++)
    {
        ptr8[i] = (*ATA_REG_DATA_BYTE);
        ATA_WAIT();
    }
    for (int i = 0; i < 512; i += 2)
    {
        uint8_t tmp = ptr8[i];
        ptr8[i]     = ptr8[i + 1];
        ptr8[i + 1] = tmp;
    }

    
    //dump_memory((void *)ptr8, 512);
    printf("done\n");
	return 0;
}