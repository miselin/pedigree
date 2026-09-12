#!/bin/bash

if [ -e /dev/fb ]; then
    /usr/bin/ttyterm || /usr/bin/gfxcon
else
    /usr/bin/ttyterm
fi
