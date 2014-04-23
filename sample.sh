#!/usr/bin/env bash

if [ -z "$1" ]; then
    echo "usage: $0 sample_name"
    exit 1
fi

mkdir -p samples/$1

PROG=./htmref
COMMANDS="addq atomic spinlock tsx"
ITER=1000000
THREADS="1 2 4 8 10 12 16 20"
GRAPH=./graph

for CMD in $COMMANDS; do
    fname=samples/$1/${CMD}.txt
    rm -f $fname
    touch $fname

    for THREAD in $THREADS; do
        echo $CMD $THREAD $ITER
        ./htmref $CMD $THREAD $ITER >> $fname
    done
done

$GRAPH $1
