#!/bin/bash

if [ -e /dev/fb ]; then
    /usr/bin/gfxcon || /usr/bin/ttyterm
else
    /usr/bin/ttyterm
fi
