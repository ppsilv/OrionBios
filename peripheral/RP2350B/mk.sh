#!/bin/bash

clear

# Pode ser feito isso
# set(picotool_DIR $ENV{HOME}/.pico-sdk/picotool/2.3.0/lib/cmake/picotool)
# ai pode usar cmake ..

cd ~/Projects/Orion/OrionBios/peripheral/RP2350B
rm -rf build && mkdir build && cd build
cmake -Dpicotool_DIR=$HOME/.pico-sdk/picotool/2.3.0/lib/cmake/picotool ..
make
