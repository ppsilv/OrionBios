/*
 * elfloader.c -- versao final, OrionDOS
 *
 * Usa a lib elfload (Owen Shepherd) so' pro parsing generico
 * (el_init/el_load/el_relocate) -- a parte chata de ler header e
 * program headers.
 *
 * A parte de GOT/A5 (que a lib NAO sabe fazer, ela so' entende
 * program headers, nao secoes) foi PORTADA DIRETO do seu
 * loader_pic.c, que voce ja testou funcionando.
 *
 * CORRECAO (nesta versao): find_got_section() e apply_data_relocations()
 * antes ficavam dando flseek/fread pra tras e pra frente no arquivo,
 * um shdr de cada vez, e depois um nome de cada vez. Isso causava um
 * drift de leitura (cada leitura saindo 1 byte mais adiantada que a
 * anterior) no driver de FAT, e por isso a secao .got nunca era
 * encontrada. A correcao le a tabela de section headers INTEIRA e a
 * .shstrtab INTEIRA de uma vez so' cada, guarda em buffers na
 * memoria, e o resto do trabalho (achar nomes, achar .got,
 * .rela.data) e' feito soh em cima desses buffers, sem tocar mais no
 * arquivo. Ver load_shdr_table(), shdr_name(), shdr_at() abaixo.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include "slots.h"
#include "elfload.h"
#include <fileio.h>

#define kprintf(...) printf(__VA_ARGS__)

/* ajuste estes dois se algum dia tiver ELF com mais secoes ou
 * .shstrtab maior que isso */
#define MAX_SHDRS   32
#define MAX_STRTAB  1024

FIL fd;
void *buf;

extern int call_with_a5(uint32_t entry_addr, uint32_t a5_value, int argc, char *argv[]);

/* --- callback de leitura que a lib elfload exige --- */
static bool fpread(el_ctx *ctx, void *dest, size_t nb, size_t offset)
{
    (void) ctx;
    unsigned int bytesRead;
    if (flseek(&fd, offset))
        return false;
    if (fread(&fd, dest, nb, &bytesRead) != FR_OK || bytesRead != nb)
        return false;
    return true;
}

/* --- callback de alocacao: devolve o endereco dentro do nosso slot --- */
static void *alloccb(el_ctx *ctx, Elf_Addr phys, Elf_Addr virt, Elf_Addr size)
{
    (void) ctx;
    (void) phys;
    (void) size;
    return (void *) virt;
}

static void check(el_status stat, const char *expln)
{
    if (stat) {
        kprintf("%s: erro %d\n", expln, stat);
    }
}

/* =========================================================================
 * Leitura da tabela de section headers + .shstrtab, TUDO DE UMA VEZ.
 *
 * Motivo: o codigo original fazia um fpread() pequeno pra cada shdr
 * (40 bytes) e depois outro fpread() pequeno pra cada nome dentro da
 * .shstrtab -- ficando o tempo todo saltando pra frente e pra tras no
 * arquivo (a tabela de shdrs fica no fim do arquivo, a .shstrtab fica
 * um pouco antes dela). O teste mostrou os nomes saindo embaralhados
 * e cortados (ex: ".symtab" virando "symtab", ".strtab" virando
 * "trtab"), com um drift de 1 byte crescendo a cada leitura -- sinal
 * classico de bug no driver de FAT quando alterna muitos seeks
 * pequenos pra tras/frente no mesmo arquivo.
 *
 * A solucao e' simplesmente parar de fazer isso: ler a tabela de
 * shdrs inteira (shnum * shentsize bytes) em UM fpread, e a
 * .shstrtab inteira em outro UM fpread, guardar tudo em buffers, e
 * depois so' indexar esses buffers em memoria -- sem nenhum novo
 * acesso ao arquivo.
 * ========================================================================= */

typedef struct {
    uint8_t  shdrs[MAX_SHDRS * 40];
    uint16_t shnum;
    uint16_t shentsize;
    uint8_t  strtab[MAX_STRTAB];
    uint32_t strtab_size;
} shdr_table_t;

static bool load_shdr_table(el_ctx *ctx, shdr_table_t *t)
{
    uint32_t shoff     = ctx->ehdr.e_shoff;
    uint16_t shnum     = ctx->ehdr.e_shnum;
    uint16_t shentsize = ctx->ehdr.e_shentsize;
    uint16_t shstrndx  = ctx->ehdr.e_shstrndx;

    if (shnum > MAX_SHDRS) {
        kprintf("elfloader: shnum=%u maior que MAX_SHDRS=%d\n", shnum, MAX_SHDRS);
        return false;
    }
    if (shentsize != 40) {
        kprintf("elfloader: shentsize=%u inesperado (esperado 40)\n", shentsize);
        return false;
    }
    if (shstrndx >= shnum) {
        kprintf("elfloader: shstrndx=%u fora da faixa (shnum=%u)\n", shstrndx, shnum);
        return false;
    }

    /* UMA leitura so' pra tabela de section headers inteira */
    if (!fpread(ctx, t->shdrs, (size_t)shnum * shentsize, shoff)) {
        kprintf("elfloader: falha lendo tabela de section headers\n");
        return false;
    }

    t->shnum     = shnum;
    t->shentsize = shentsize;

    /* pega o shdr da .shstrtab de dentro do buffer que acabamos de
     * ler -- nenhum acesso novo ao arquivo aqui */
    uint8_t *strtab_hdr = t->shdrs + (size_t)shstrndx * shentsize;
    uint32_t strtab_off  = ((uint32_t)strtab_hdr[0x10] << 24) | ((uint32_t)strtab_hdr[0x11] << 16) |
                            ((uint32_t)strtab_hdr[0x12] << 8)  |  (uint32_t)strtab_hdr[0x13];
    uint32_t strtab_size = ((uint32_t)strtab_hdr[0x14] << 24) | ((uint32_t)strtab_hdr[0x15] << 16) |
                            ((uint32_t)strtab_hdr[0x16] << 8)  |  (uint32_t)strtab_hdr[0x17];

    if (strtab_size > MAX_STRTAB) {
        kprintf("elfloader: .shstrtab tem %lu bytes, maior que MAX_STRTAB=%d\n",
                (unsigned long)strtab_size, MAX_STRTAB);
        return false;
    }

    /* UMA leitura so' pra .shstrtab inteira */
    if (!fpread(ctx, t->strtab, strtab_size, strtab_off)) {
        kprintf("elfloader: falha lendo .shstrtab\n");
        return false;
    }

    t->strtab_size = strtab_size;
    return true;
}

/* nome da secao i, direto do buffer em memoria (sem tocar no arquivo) */
static const char *shdr_name(shdr_table_t *t, uint16_t i)
{
    uint8_t *h = t->shdrs + (size_t)i * t->shentsize;
    uint32_t name_idx = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                         ((uint32_t)h[2] << 8)  |  (uint32_t)h[3];
    if (name_idx >= t->strtab_size)
        return "";
    return (const char *)(t->strtab + name_idx);
}

/* ponteiro pro shdr bruto (40 bytes) da secao i, dentro do buffer */
static uint8_t *shdr_at(shdr_table_t *t, uint16_t i)
{
    return t->shdrs + (size_t)i * t->shentsize;
}

/*
 * == do loader_pic.c (adaptado) ==
 * Acha a(s) secao(oes) cujo nome comeca com ".got" (isso pega tanto
 * .got quanto .got.plt -- no seu binario elas sao contiguas, entao
 * tratar as duas juntas nao e' problema, ver observacao anterior).
 * Devolve o endereco/tamanho DENTRO do binario (sem somar
 * task_base ainda -- isso quem soma e' quem chama esta funcao).
 */
static int find_got_section(shdr_table_t *t, uint32_t *got_addr, uint32_t *got_size)
{
    uint32_t min_addr = 0xFFFFFFFF, max_end = 0;
    int found = 0;

    for (uint16_t i = 0; i < t->shnum; i++) {
        const char *name = shdr_name(t, i);

        if (name[0] == '.' && name[1] == 'g' && name[2] == 'o' && name[3] == 't') {
            uint8_t *h = shdr_at(t, i);
            uint32_t addr = ((uint32_t)h[0x0C] << 24) | ((uint32_t)h[0x0D] << 16) |
                             ((uint32_t)h[0x0E] << 8)  |  (uint32_t)h[0x0F];
            uint32_t size = ((uint32_t)h[0x14] << 24) | ((uint32_t)h[0x15] << 16) |
                             ((uint32_t)h[0x16] << 8)  |  (uint32_t)h[0x17];
            if (addr < min_addr) min_addr = addr;
            if (addr + size > max_end) max_end = addr + size;
            found = 1;
        }
    }

    if (!found) return 0;
    *got_addr = min_addr;
    *got_size = max_end - min_addr;
    return 1;
}

/*
 * == do loader_pic.c ==
 * Percorre o array da GOT (agora ja' em memoria, carregado pelo
 * el_load() da lib) somando task_base em cada entrada de 4 bytes.
 */
static void patch_got(uint32_t task_base, uint32_t got_addr, uint32_t got_size)
{
    uint32_t *got = (uint32_t *)(task_base + got_addr);
    uint32_t got_words = got_size / 4;
    for (uint32_t w = 0; w < got_words; w++)
        got[w] += task_base;
}

/*
 * == do loader_pic.c (adaptado) ==
 * Acha ".rela.data" na tabela de shdrs (ja em memoria) e aplica os
 * relocs R_68K_32 nela. O CONTEUDO da .rela.data em si (que pode ser
 * grande) ainda e' lido do arquivo, mas em UM fpread so' pro bloco
 * inteiro -- nao mais um fpread de 12 bytes por entrada.
 */
static void apply_data_relocations(el_ctx *ctx, shdr_table_t *t, uint32_t task_base)
{
    for (uint16_t i = 0; i < t->shnum; i++) {
        const char *name = shdr_name(t, i);

        static const char target[] = ".rela.data";
        int match = 1;
        for (int k = 0; target[k]; k++) {
            if (name[k] != target[k]) { match = 0; break; }
        }
        if (!match)
            continue;

        uint8_t *h = shdr_at(t, i);
        uint32_t rela_offset = ((uint32_t)h[0x10] << 24) | ((uint32_t)h[0x11] << 16) |
                                ((uint32_t)h[0x12] << 8)  |  (uint32_t)h[0x13];
        uint32_t rela_size   = ((uint32_t)h[0x14] << 24) | ((uint32_t)h[0x15] << 16) |
                                ((uint32_t)h[0x16] << 8)  |  (uint32_t)h[0x17];

        if (rela_size == 0)
            return;

        /* le o bloco .rela.data inteiro de uma vez (ate' 256 entradas
         * de 12 bytes = 3072 bytes; ajuste o tamanho se precisar de
         * mais) */
        #define MAX_RELA_BYTES (256 * 12)
        static uint8_t rela_buf[MAX_RELA_BYTES];

        if (rela_size > MAX_RELA_BYTES) {
            kprintf("elfloader: .rela.data tem %lu bytes, maior que o buffer (%d)\n",
                    (unsigned long)rela_size, MAX_RELA_BYTES);
            return;
        }

        if (!fpread(ctx, rela_buf, rela_size, rela_offset)) {
            kprintf("elfloader: falha lendo .rela.data\n");
            return;
        }

        uint32_t num_relas = rela_size / 12;
        for (uint32_t r = 0; r < num_relas; r++) {
            uint8_t *e = rela_buf + r * 12;

            uint32_t r_offset = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) |
                                 ((uint32_t)e[2] << 8)  |  (uint32_t)e[3];
            uint32_t r_info   = ((uint32_t)e[4] << 24) | ((uint32_t)e[5] << 16) |
                                 ((uint32_t)e[6] << 8)  |  (uint32_t)e[7];
            uint32_t r_type = r_info & 0xFF;

            if (r_type != 1) /* so' R_68K_32 */
                continue;

            uint32_t *patch_addr = (uint32_t *)(task_base + r_offset);
            *patch_addr += task_base;
        }
        return; /* achou .rela.data, nao precisa continuar procurando */
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("uso: %s [elf-a-carregar]\n", argv[0]);
        return 1;
    }
    printf("Arquivo[%s]\n", argv[1]);

    if (fopen(&fd, argv[1], FA_READ) != FR_OK) {
        kprintf("erro abrindo arquivo\n");
        return 1;
    }

    el_ctx ctx;
    ctx.pread = fpread;
    check(el_init(&ctx), "inicializando");

    int slot_index = Slots_Alloc();
    if (slot_index < 0) {
        kprintf("sem slots de memoria livres\n");
        fclose(&fd);
        return 1;
    }
    uint32_t task_base = Slots_BaseAddr(slot_index);
    buf = (void *) task_base;

    ctx.base_load_vaddr = ctx.base_load_paddr = (uintptr_t) task_base;

    int ret = 1;

    check(el_load(&ctx, alloccb), "carregando segmentos");
    check(el_relocate(&ctx), "aplicando relocacoes dinamicas"); /* so' faz algo se for ET_DYN;
                                                                     pro seu ET_EXEC estatico,
                                                                     retorna OK sem fazer nada */

    /* le a tabela de section headers + .shstrtab de uma vez so' */
    shdr_table_t shdrt;
    if (!load_shdr_table(&ctx, &shdrt)) {
        kprintf("erro carregando tabela de secoes\n");
        goto fail;
    }

    uint32_t got_addr = 0, got_size = 0;
    if (find_got_section(&shdrt, &got_addr, &got_size)) {
        patch_got(task_base, got_addr, got_size);
    } else {
        kprintf("aviso: secao .got nao encontrada, pulando patch\n");
        goto fail;
    }

    apply_data_relocations(&ctx, &shdrt, task_base);

    uint32_t entry_addr = task_base + ctx.ehdr.e_entry;
    uint32_t a5_value    = task_base + got_addr;

    kprintf("slot=%d task_base=0x%08lx entry=0x%08lx a5=0x%08lx\n",
            slot_index, (unsigned long)task_base,
            (unsigned long)entry_addr, (unsigned long)a5_value);

    ret = call_with_a5(entry_addr, a5_value, argc, argv);
    kprintf("programa retornou %d\n", ret);

fail:
    Slots_Free(slot_index);
    fclose(&fd);
    return ret;
}
