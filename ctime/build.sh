#!/bin/sh

mkdir -p ../build/debug

# Debug build:
g++ -Wno-write-strings -g -o ../build/debug/ctime ctime.c

mkdir -p ../build/opt

# Release build:
g++ -Wno-write-strings -Ofast -o ../build/opt/ctime ctime.c
