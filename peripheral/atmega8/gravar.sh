#!/bin/bash

clear

avrdude -c usbasp -p m8 -U flash:w:interruptCntrl.ino.hex:i
