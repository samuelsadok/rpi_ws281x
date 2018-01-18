#!/bin/bash

gcc -c lightctl.c -o lightctl.o
gcc lightctl.o libws2811.a -o lightctl

