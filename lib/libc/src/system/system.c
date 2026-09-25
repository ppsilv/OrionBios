

void system_init(void) {
    // Inicialização básica
}

extern volatile long systemTick; 

unsigned long get_system_tick(void) {
    unsigned long tick;
    
    tick = systemTick;                     
    
    return tick;
}