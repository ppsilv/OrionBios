#!/bin/bash


avrdude -c usbasp -p m8 -F -U lfuse:r:-:h -U hfuse:r:-:h
