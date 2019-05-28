#!/bin/bash

kbscan="32 64 128 256 512"
mbscan="1 2 4 8 16 32 64 128 256 512 1024"

for mb in $mbscan; do
    kbscan="$kbscan $((mb*1024))"
done
for kb in $kbscan; do
    ./scan_splits.sh $((kb*1024)) | tee -a RESULTS.splits
done
