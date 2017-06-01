#!/bin/bash

VALUES=$(egrep -o ":	[0-9]+" build/serial.txt | tr -d ': \t' | egrep '^[0-9]+$' | sort | uniq)

mkdir -p build/process_logs
for pid in $VALUES; do
    egrep ":	$pid" build/serial.txt >build/process_logs/$pid.txt
done
