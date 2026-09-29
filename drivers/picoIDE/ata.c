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


