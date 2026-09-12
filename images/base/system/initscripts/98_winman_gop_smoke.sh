#!/bin/bash

if [ ! -e /etc/winman-gop-smoke ]; then
    exit 0
fi

exec /usr/bin/winman-gop-smoke
