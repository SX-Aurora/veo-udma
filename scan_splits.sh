#!/bin/bash

SIZE=$1

SPLITS="1 2 3 4 8 16 32 64"
SPLITKBS="32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536"

printf "\n=== RECV $SIZE ===\n"
printf "%8s   %7s  %7s  %7s  %7s  %7s  %7s  %7s  %7s\n" "split_size" . . . num split buffs . .
printf "%8s   %7s  %7s  %7s  %7s  %7s  %7s  %7s  %7s\n" "[kB]" $SPLITS
printf "=------------------------------------------------------------------------------------------\n"
bwmax=0; splitmax=0; sizemax=0
for KB in $SPLITKBS; do
    R="${KB}"
    for SPLIT in $SPLITS; do
        export UDMA_SPLIT_RECV=$SPLIT
        export UDMA_SPLIT_SIZE_RECV=$((KB * 1024))
        if [ $((KB * SPLIT / 1024)) -gt 191 ]; then
            bw="0"
        else
            bw=`./hello recv $SIZE 2>&1 | grep "bw=" | sed -e 's,^.*bw=,,' -e 's,\..*$,,'`
            if [ $bw -gt $bwmax ]; then
                bwmax=$bw
                splitmax=$SPLIT
                sizemax=$((KB*1024))
            fi
        fi
        R="$R $bw"
    done
    printf "%8d:  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f\n" $R
done
printf "Max BW: %d MB/s for split=%d split_size=%d\n" $bwmax $splitmax $sizemax

printf "\n=== SEND $SIZE ===\n"
printf "%8s   %7s  %7s  %7s  %7s  %7s  %7s  %7s  %7s\n" "split_size" . . . num split buffs . .
printf "%8s   %7s  %7s  %7s  %7s  %7s  %7s  %7s  %7s\n" kB $SPLITS
printf "=------------------------------------------------------------------------------------------\n"
bwmax=0; splitmax=0; sizemax=0
for KB in $SPLITKBS; do
    R="${KB}"
    for SPLIT in $SPLITS; do
        export UDMA_SPLIT_SEND=$SPLIT
        export UDMA_SPLIT_SIZE_SEND=$((KB * 1024))
        if [ $((KB * SPLIT / 1024)) -gt 191 ]; then
            bw="0"
        else
            bw=`./hello send $SIZE 2>&1 | grep "bw=" | sed -e 's,^.*bw=,,' -e 's,\..*$,,'`
            if [ $bw -gt $bwmax ]; then
                bwmax=$bw
                splitmax=$SPLIT
                sizemax=$((KB*1024))
            fi
        fi
        R="$R $bw"
    done
    printf "%8d:  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f  %7.0f\n" $R
done
printf "Max BW: %d MB/s for split=%d split_size=%d\n" $bwmax $splitmax $sizemax

