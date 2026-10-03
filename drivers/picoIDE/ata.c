/*
Coisas pra testar em ordem, antes de plugar no FatFS:

ata_init() sozinho — só confirma que BSY cai.
ata_identify() — se voltar ATA_OK e o buffer tiver algo que pareça 
texto (model/serial, ainda que com bytes trocados por par, isso é 
normal do próprio padrão), a decodificação de endereço e o /CSDATA 
estão corretos.
Só depois ata_read_sectors()/ata_write_sectors().

Os dois pontos que eu realmente não tenho como confirmar sem o hardware 
na sua mão, marcados como "VERIFICAR" no topo do arquivo: se a word do 
registrador de dados vem na ordem certa (tem a flag ATA_SWAP_DATA_WORD 
pra isso) e se o timeout por contagem de laço (ATA_TIMEOUT_LOOPS) é 
adequado pro seu clock — se travar no polling logo de cara, é o primeiro 
lugar pra olhar.
*/

/*
 * ata.c - Driver ATA/IDE em modo PIO (polling) para a interface picoIDE
 *
 * Mapa de registradores (conforme esquema fornecido):
 *   Base:  0xFF4000
 *   DATA            = base + 0   (word, 16 bits — exige /CSDATA: UDS+LDS juntos)
 *   ERROR/FEATURES  = base + 3   (byte, endereço ímpar — exige /CSTASKFILE: só /LDS)
 *   SECTOR_COUNT    = base + 5
 *   LBA_LOW         = base + 7
 *   LBA_MID         = base + 9
 *   LBA_HIGH        = base + 0x0B
 *   DEVICE_HEAD     = base + 0x0D
 *   STATUS/COMMAND  = base + 0x0F
 *
 * IMPORTANTE - coisas para validar no hardware real antes de confiar cegamente
 * neste driver (ver comentários "VERIFICAR" espalhados abaixo):
 *
 *   1) Não existe registrador de Alternate Status / Device Control mapeado
 *      (não há bloco de controle, só os 8 registradores de comando acima).
 *      Isso significa:
 *        - Não há soft reset via bit SRST do Device Control -> ata_init()
 *          só espera BSY cair após poder de energia, não reseta o canal.
 *        - Ler o registrador de Status (em vez de Alternate Status) tem o
 *          efeito colateral de reconhecer/limpar uma IRQ pendente do drive.
 *          Como o projeto usa polling (sem IRQ de ATA), isso não deveria
 *          importar, mas fique ciente se um dia adicionar IRQ.
 *
 *   2) Ordem de bytes do registrador DATA (word): este código assume que o
 *      *(volatile uint16_t*) na CPU 68k (big-endian) já entrega os bytes na
 *      ordem correta de um setor (byte 0 do setor no byte alto, byte 1 no
 *      byte baixo da palavra). Se os bytes vierem trocados na prática
 *      (dado que o dispositivo do outro lado é um IDE clássico, que
 *      historicamente transmite little-endian), troque ATA_SWAP_DATA_WORD
 *      abaixo para 1. Teste com um IDENTIFY: os campos de string (model,
 *      serial) do IDENTIFY são conhecidos por vir com bytes já trocados por
 *      *par* em qualquer sistema (isso é do próprio padrão ATA, não do seu
 *      hardware) — não confunda esse efeito com o problema de endianness
 *      da palavra em si.
 *
 *   3) Timeout por contagem de laço (ATA_TIMEOUT_LOOPS), não por tempo real
 *      (não usei nenhuma API de systick/RTC do projeto, pois não sei o nome
 *      das funções). Se tiver uma função de tick disponível, troque
 *      ata_wait_status() para usar tempo real em vez de contagem de laço.
 *
 *   4) Só testado/pensado para 1 drive (master, drive=0) no único canal
 *      (IDE1 = picoIDE). Não há segundo canal mapeado aqui.
 *
 *   5) Concorrência: seguindo a decisão já tomada no projeto (desabilitar
 *      interrupções durante acesso ao controlador em vez de mutex), as
 *      macros ATA_LOCK()/ATA_UNLOCK() abaixo estão vazias — troque pelas
 *      chamadas reais de desabilitar/habilitar interrupção do seu kernel/
 *      orioncore antes de usar isso com mais de uma task tocando o disco.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef ATA_DEBUG
#include <stdio.h>
#define ATA_DBG(...) printf(__VA_ARGS__)
#else
#define ATA_DBG(...) do {} while (0)
#endif

/* ---- ajuste se o teste de IDENTIFY mostrar bytes trocados dentro da word --- */
#define ATA_SWAP_DATA_WORD 0

/* ---- ganchos de exclusão mútua - troque pelas funções reais do seu SO ---- */
#define ATA_LOCK()   do {} while (0)
#define ATA_UNLOCK() do {} while (0)

#define IDE_BASE 0xFF4000UL

#define IDE_REG_DATA         (*(volatile uint16_t *)(IDE_BASE + 0x00))
#define IDE_REG_ERROR        (*(volatile uint8_t  *)(IDE_BASE + 0x03)) /* leitura */
#define IDE_REG_FEATURES     (*(volatile uint8_t  *)(IDE_BASE + 0x03)) /* escrita */
#define IDE_REG_SECTOR_COUNT (*(volatile uint8_t  *)(IDE_BASE + 0x05))
#define IDE_REG_LBA_LOW      (*(volatile uint8_t  *)(IDE_BASE + 0x07))
#define IDE_REG_LBA_MID      (*(volatile uint8_t  *)(IDE_BASE + 0x09))
#define IDE_REG_LBA_HIGH     (*(volatile uint8_t  *)(IDE_BASE + 0x0B))
#define IDE_REG_DEVICE_HEAD  (*(volatile uint8_t  *)(IDE_BASE + 0x0D))
#define IDE_REG_STATUS       (*(volatile uint8_t  *)(IDE_BASE + 0x0F)) /* leitura */
#define IDE_REG_COMMAND      (*(volatile uint8_t  *)(IDE_BASE + 0x0F)) /* escrita */

/* Bits do registrador de status */
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DSC  0x10
#define ATA_SR_DRQ  0x08
#define ATA_SR_CORR 0x04
#define ATA_SR_IDX  0x02
#define ATA_SR_ERR  0x01

/* Comandos ATA usados aqui */
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_TIMEOUT_LOOPS 1000000UL

typedef enum {
    ATA_OK = 0,
    ATA_ERR_TIMEOUT_BSY = -1,
    ATA_ERR_TIMEOUT_DRQ = -2,
    ATA_ERR_DEVICE      = -3,
    ATA_ERR_BAD_ARG     = -4
} ata_status_t;

/* 4 leituras de status "de sobra" -> equivalente ao delay de ~400ns exigido
 * apos trocar Device/Head, ja que nao ha Alternate Status para isso. */
static void ata_400ns_delay(void)
{
    volatile uint8_t dummy;
    dummy = IDE_REG_STATUS;
    dummy = IDE_REG_STATUS;
    dummy = IDE_REG_STATUS;
    dummy = IDE_REG_STATUS;
    (void)dummy;
}

static int ata_wait_clear(uint8_t mask)
{
    uint32_t timeout = ATA_TIMEOUT_LOOPS;
    while (IDE_REG_STATUS & mask) {
        if (--timeout == 0)
            return -1;
    }
    return 0;
}

/* Espera BSY=0 e depois DRQ=1 (dado pronto) ou ERR=1 (falhou) */
static int ata_wait_drq(void)
{
    uint32_t timeout;
    uint8_t status;

    if (ata_wait_clear(ATA_SR_BSY) != 0) {
        ATA_DBG("ata: timeout esperando BSY cair\n");
        return ATA_ERR_TIMEOUT_BSY;
    }

    timeout = ATA_TIMEOUT_LOOPS;
    for (;;) {
        status = IDE_REG_STATUS;
        if (status & ATA_SR_ERR) {
            ATA_DBG("ata: ERR setado, status=0x%02x, error=0x%02x\n",
                    status, IDE_REG_ERROR);
            return ATA_ERR_DEVICE;
        }
        if (status & ATA_SR_DRQ)
            return ATA_OK;
        if (--timeout == 0) {
            ATA_DBG("ata: timeout esperando DRQ, status=0x%02x\n", status);
            return ATA_ERR_TIMEOUT_DRQ;
        }
    }
}

static void ata_select_drive(uint8_t drive, uint32_t lba)
{
    /* modo LBA28: bit7=1, bit6=1 (LBA), bit5=1 (reservado, sempre 1),
     * bit4 = drive (0=master,1=slave), bits3-0 = bits 24-27 do LBA */
    uint8_t devhead = 0xE0 | ((drive & 1) << 4) | ((lba >> 24) & 0x0F);
    IDE_REG_DEVICE_HEAD = devhead;
    ata_400ns_delay();
}

static uint16_t ata_data_read16(void)
{
    uint16_t w = IDE_REG_DATA;
#if ATA_SWAP_DATA_WORD
    w = (uint16_t)((w << 8) | (w >> 8));
#endif
    return w;
}

static void ata_data_write16(uint16_t w)
{
#if ATA_SWAP_DATA_WORD
    w = (uint16_t)((w << 8) | (w >> 8));
#endif
    IDE_REG_DATA = w;
}

/*
 * Inicializacao minima: so espera o drive sair de BSY (o /CS_IDE/GAL nao tem
 * mais influencia aqui, isso e so o drive real terminando o power-on).
 * Nao ha reset via Device Control (nao mapeado nessa placa).
 */
int ata_init(void)
{
    ATA_LOCK();
    int ret = ata_wait_clear(ATA_SR_BSY);
    ATA_UNLOCK();

    if (ret != 0) {
        ATA_DBG("ata_init: drive nao saiu de BSY\n");
        return ATA_ERR_TIMEOUT_BSY;
    }
    ATA_DBG("ata_init: drive pronto, status=0x%02x\n", IDE_REG_STATUS);
    return ATA_OK;
}

/*
 * IDENTIFY DEVICE - preenche buf256 com 256 words (512 bytes) crus,
 * exatamente como definido no padrao ATA. Strings dentro do buffer
 * (model/serial) vem com bytes trocados por par - isso e do padrao,
 * nao do seu hardware.
 */
int ata_identify(uint8_t drive, uint16_t buf256[256])
{
    int ret;

    if (buf256 == NULL)
        return ATA_ERR_BAD_ARG;

    ATA_LOCK();

    ata_select_drive(drive, 0);
    IDE_REG_SECTOR_COUNT = 0;
    IDE_REG_LBA_LOW = 0;
    IDE_REG_LBA_MID = 0;
    IDE_REG_LBA_HIGH = 0;
    IDE_REG_COMMAND = ATA_CMD_IDENTIFY;

    ret = ata_wait_drq();
    if (ret == ATA_OK) {
        int i;
        for (i = 0; i < 256; i++)
            buf256[i] = ata_data_read16();
    }

    ATA_UNLOCK();
    return ret;
}

/*
 * Le 'count' setores (1-255) a partir de 'lba' (LBA28) para 'buf'
 * (buf deve ter espaco para count*512 bytes).
 */
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t count, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    int ret;
    int s;

    if (buf == NULL || count == 0)
        return ATA_ERR_BAD_ARG;
    if (lba > 0x0FFFFFFFUL)
        return ATA_ERR_BAD_ARG; /* fora do alcance do LBA28 */

    ATA_LOCK();

    ata_select_drive(drive, lba);

    ret = ata_wait_clear(ATA_SR_BSY);
    if (ret != 0) {
        ATA_UNLOCK();
        return ATA_ERR_TIMEOUT_BSY;
    }

    IDE_REG_SECTOR_COUNT = count;
    IDE_REG_LBA_LOW  = (uint8_t)(lba & 0xFF);
    IDE_REG_LBA_MID  = (uint8_t)((lba >> 8) & 0xFF);
    IDE_REG_LBA_HIGH = (uint8_t)((lba >> 16) & 0xFF);
    IDE_REG_COMMAND  = ATA_CMD_READ_SECTORS;

    for (s = 0; s < count; s++) {
        ret = ata_wait_drq();
        if (ret != ATA_OK) {
            ATA_UNLOCK();
            return ret;
        }

        int w;
        for (w = 0; w < 256; w++) {
            uint16_t data = ata_data_read16();
            p[0] = (uint8_t)(data >> 8);
            p[1] = (uint8_t)(data & 0xFF);
            p += 2;
        }
    }

    ATA_UNLOCK();
    return ATA_OK;
}

/*
 * Escreve 'count' setores (1-255) a partir de 'lba' (LBA28) vindos de 'buf'.
 */
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t count, const void *buf)
{
    const uint8_t *p = (const uint8_t *)buf;
    int ret;
    int s;

    if (buf == NULL || count == 0)
        return ATA_ERR_BAD_ARG;
    if (lba > 0x0FFFFFFFUL)
        return ATA_ERR_BAD_ARG;

    ATA_LOCK();

    ata_select_drive(drive, lba);

    ret = ata_wait_clear(ATA_SR_BSY);
    if (ret != 0) {
        ATA_UNLOCK();
        return ATA_ERR_TIMEOUT_BSY;
    }

    IDE_REG_SECTOR_COUNT = count;
    IDE_REG_LBA_LOW  = (uint8_t)(lba & 0xFF);
    IDE_REG_LBA_MID  = (uint8_t)((lba >> 8) & 0xFF);
    IDE_REG_LBA_HIGH = (uint8_t)((lba >> 16) & 0xFF);
    IDE_REG_COMMAND  = ATA_CMD_WRITE_SECTORS;

    for (s = 0; s < count; s++) {
        ret = ata_wait_drq();
        if (ret != ATA_OK) {
            ATA_UNLOCK();
            return ret;
        }

        int w;
        for (w = 0; w < 256; w++) {
            uint16_t data = (uint16_t)((p[0] << 8) | p[1]);
            ata_data_write16(data);
            p += 2;
        }
    }

    /* espera a ultima escrita realmente terminar (BSY cair de novo) antes
     * de devolver o controle - evita mandar outro comando cedo demais */
    ret = ata_wait_clear(ATA_SR_BSY);

    ATA_UNLOCK();
    return (ret == 0) ? ATA_OK : ATA_ERR_TIMEOUT_BSY;
}
