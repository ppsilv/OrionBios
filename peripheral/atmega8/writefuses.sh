#!/bin/bash

avrdude -c usbasp  -p m8  -U lfuse:w:0xa4:m -U hfuse:w:0xc7:m
