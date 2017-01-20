#!/bin/sh

# Debug build:
g++ -Wno-write-strings -g -o ../build/debug/ctime ctime.c

# Release build:
g++ -Wno-write-strings -Ofast -o ../build/opt/ctime ctime.c
