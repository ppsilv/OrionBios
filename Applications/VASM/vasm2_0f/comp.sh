#!/bin/bash
clear
make clean
rm -Rf obj
mkdir obj
make CPU=m68k SYNTAX=mot

