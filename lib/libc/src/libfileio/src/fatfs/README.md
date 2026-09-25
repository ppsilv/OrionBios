+-----------+-----------------+------------------------+------------------------+
|fattrib                                                                        |
+-----------+-----------------+------------------------+------------------------+
|Bits 7:6   |Tipo	          |Conteúdo do arquivo 	   |  Ação                  | 
+-----------+-----------------+------------------------+------------------------+
|00	        |Arquivo normal	  |Dados	               |  f_open                |  
|01 (0x40)	|Link simbólico	  |Caminho do alvo	       |  Resolve e redireciona |
|10 (0x80)	|Dispositivo	  |Nome do driver + params |  Abre driver           |  
|11 (0xC0)	|Reservado	—	  |Futuro                  |  (ex: link duro, pipe) |
+-----------+-----------------+------------------------+------------------------+

Detectar o recurso

FRESULT abrir_recurso(const char *path, FIL *fp, BYTE mode) {
    FILINFO fno;
    FRESULT fr;

    fr = f_stat(path, &fno);
    if (fr != FR_OK) return fr;

    if (fno.fattrib & 0x40) {
        // É um dispositivo — NÃO abrir como arquivo
        return FR_INT_ERR; // ou um código customizado seu
    }

    // É arquivo normal — abre normalmente
    return f_open(fp, path, mode);
}

Redirecionar para o driver
if (fno.fattrib & 0x40) {
    // Extrai o nome do dispositivo do caminho
    // Ex: "0:/dev/tty" → "tty"
    const char *dev = strrchr(path, '/') + 1;
    return driver_open(dev, ...); // sua função de driver
}

Listar dispositivos
while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    if (fno.fattrib & 0x40) {
        printf("[DEV] %s\n", fno.fname);
    } else {
        printf("[FILE] %s\n", fno.fname);
    }
}

/dev/tty        → bit 6 setado, conteúdo: "/dev/serial0"  (link simbólico)
/dev/disk       → bit 6 setado, conteúdo: "0:/system/disk.img"  (link para arquivo)

Dois tipos de link com os 2 bits:
Bit 6 (0x40) → link simbólico "duro" (redireciona)
Bit 7 (0x80) → link de dispositivo (abre driver)
Bits 6+7 (0xC0) → link para link, ou outro tipo futuro

// Cria o arquivo-âncora com o caminho do alvo
FIL f;
f_open(&f, "0:/dev/tty", FA_CREATE_NEW | FA_WRITE);
f_write(&f, "/dev/serial0", 12, &bw);  // conteúdo = caminho do alvo
f_close(&f);

// Marca como link simbólico (bit 6)
f_chmod("0:/dev/tty", 0x40, 0xC0);

Como resolver o link 
#define ATTR_LINK_SYM   0x40
#define ATTR_LINK_DEV   0x80

FRESULT resolver_link(const char *path, char *destino, size_t tam) {
    FILINFO fno;
    FIL f;
    FRESULT fr;
    UINT br;

    fr = f_stat(path, &fno);
    if (fr != FR_OK) return fr;

    if (!(fno.fattrib & ATTR_LINK_SYM)) {
        return FR_NO_FILE;  // não é link
    }

    // Lê o conteúdo (caminho do alvo)
    fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) return fr;

    if (fno.fsize >= tam) {
        f_close(&f);
        return FR_INVALID_NAME;  // caminho muito longo
    }

    fr = f_read(&f, destino, fno.fsize, &br);
    f_close(&f);

    if (fr != FR_OK) return fr;
    destino[br] = '\0';  // termina a string

    return FR_OK;
}

Abertura com resolução de link (com proteção contra loops):

#define MAX_LINKS  8   // limite de redirecionamentos encadeados
FRESULT abrir_recurso(const char *path, FIL *fp, BYTE mode) {
    char atual[256];
    strncpy(atual, path, sizeof(atual) - 1);
    atual[sizeof(atual) - 1] = '\0';

    for (int i = 0; i < MAX_LINKS; i++) {
        FILINFO fno;
        FRESULT fr = f_stat(atual, &fno);
        if (fr != FR_OK) return fr;

        if (fno.fattrib & ATTR_LINK_DEV) {
            // É dispositivo — redireciona para driver
            return driver_open(atual, fp, mode);
        }

        if (fno.fattrib & ATTR_LINK_SYM) {
            char alvo[256];
            fr = resolver_link(atual, alvo, sizeof(alvo));
            if (fr != FR_OK) return fr;
            strncpy(atual, alvo, sizeof(atual) - 1);
            atual[sizeof(atual) - 1] = '\0';
            continue;  // resolve o próximo
        }

        // É arquivo normal — abre
        return f_open(fp, atual, mode);
    }

    return FR_TOO_MANY_OPEN_FILES;  // loop detectado
}

