#ifndef _BOARDS_ORION_RP2350B_H
#define _BOARDS_ORION_RP2350B_H

// herda o resto da pico2 (LED, flash, etc.)
#include "boards/pico2.h"

// o pico2.h define RP2350A=1, entao sobrescrevemos depois
#undef PICO_RP2350A
#define PICO_RP2350A 0

#endif