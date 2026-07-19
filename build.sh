#!/bin/sh
# Build SCP55GO.COM (NASM flat binary; also builds on-box with C:\NASM)
nasm -f bin SCP55GO.ASM -o SCP55GO.COM && ls -l SCP55GO.COM
