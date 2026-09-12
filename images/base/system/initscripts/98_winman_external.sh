#!/bin/bash

if [ ! -e /etc/winman-external ]; then
    exit 0
fi

exec /usr/bin/winman-external
