// ============================================================================
// PATCH 1 de 2 - ide_interface.c: LBA de 28 bits (ate 128 GB) e bswap nos DMAs
// ============================================================================
// Substitua estas TRES funcoes no seu ide_interface.c.
// As demais ficam como estao.

// --- 1) LBA de 28 bits --------------------------------------------------------
// ATA classico: LBA[7:0]=LBA Low, [15:8]=LBA Mid, [23:16]=LBA High e
// LBA[27:24] = nibble BAIXO do registrador Device/Head.
// (Device/Head: bit7=1, bit6=LBA, bit5=1, bit4=drive, bits3..0 = LBA[27:24])
static uint32_t current_lba_from_regs(void) {
    return ((uint32_t)(g_reg_device_head & 0x0F) << 24) |
           ((uint32_t)g_reg_lba_high << 16) |
           ((uint32_t)g_reg_lba_mid  << 8)  |
            (uint32_t)g_reg_lba_low;
}

// --- 2) DMA de leitura (SD -> buffer -> 68k) com bswap ----------------------
static void arm_dma_b_from_buffer(int buf_index) {
    dma_channel_config c = dma_channel_get_default_config(g_dma_b);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_bswap(&c, true);                 // <-- NOVO: troca os 2 bytes de cada word
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, true /* TX */));
    dma_channel_configure(
        g_dma_b, &c,
        &g_pio->txf[g_sm_data],
        g_buf[buf_index],
        IDE_SECTOR_SIZE_WORDS,
        true
    );
}

// --- 3) DMA de escrita (68k -> buffer -> SD) com bswap ----------------------
// Precisa do MESMO bswap, senao a escrita grava o setor com os bytes trocados.
static void arm_dma_c_into_buffer(int buf_index) {
    dma_channel_config c = dma_channel_get_default_config(g_dma_c);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_bswap(&c, true);                 // <-- NOVO
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, false /* RX */));
    dma_channel_configure(
        g_dma_c, &c,
        g_buf[buf_index],
        &g_pio->rxf[g_sm_data],
        IDE_SECTOR_SIZE_WORDS,
        true
    );
}

// ============================================================================
// PATCH 2 de 2 - ide_test.c (aplique voce, pois voce ja alterou o arquivo)
// ============================================================================
// (a) Em read_sector(): troque
//         lba &= 0xFFFFFFUL;
//     por
//         lba &= 0x0FFFFFFFUL;                    // 28 bits
//     e troque
//         wr(R_DEVHEAD,  0xE0);
//     por
//         wr(R_DEVHEAD,  0xE0 | ((lba >> 24) & 0x0F));   // LBA[27:24]
//
// (b) Em stage3_read(), depois da comparacao das duas leituras, acrescente o
//     teste da fronteira de 8 GB. O setor 0x01000000 (exatamente 8 GB) e
//     distinto do setor 0. Se o Pico ainda truncar em 24 bits, ele le o
//     setor 0 no lugar, e os dois saem identicos.
//
//     static int stage4_lba28(void) {
//         puts_("[4] fronteira de 8 GB: LBA 0x01000000 contra LBA 0"); crlf();
//         if (!read_sector(0x01000000UL, sector_b)) return 0;
//         for (int i = 0; i < 256; i++) {
//             if (sector_a[i] != sector_b[i]) {
//                 puts_("    setores diferentes: LBA de 28 bits funcionando"); crlf();
//                 return 1;
//             }
//         }
//         puts_("    IDENTICOS: o Pico ainda trunca o LBA em 24 bits"); crlf();
//         return 0;
//     }
//
//     (sector_a ja contem o setor 0, lido na etapa 3.) Chame em main() depois
//     da etapa 3. Precisa de um cartao com mais de 8 GB: o seu tem cerca de 32 GB.