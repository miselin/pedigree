#!/bin/bash

PRUNES="-path src/user/applications/nyancat"
PRUNES="${PRUNES} -o -path src/user/applications/fire"
PRUNES="${PRUNES} -o -path src/user/applications/gears"
PRUNES="${PRUNES} -o -path src/user/applications/chess"
PRUNES="${PRUNES} -o -path src/modules/drivers/cdi"
PRUNES="${PRUNES} -o -path src/modules/drivers/common/cdi"
PRUNES="${PRUNES} -o -path src/modules/system/config/sqlite3"
PRUNES="${PRUNES} -o -path src/lgpl"
PRUNES="${PRUNES} -o -path src/subsys/posix/musl"
PRUNES="${PRUNES} -o -path src/subsys/posix/include"
PRUNES="${PRUNES} -o -path src/subsys/posix/newlib"
PRUNES="${PRUNES} -o -path src/system/kernel/debugger/libudis86"

FILEPRUNES="-path src/subsys/posix/glue-dlmalloc.c"

find src -type d \( ${PRUNES} \) -prune -o -type f \( ${FILEPRUNES} \) -prune -o \( -name '*.cc' -o -name '*.h' -o -name '*.c' \) -print0 | xargs -0 clang-format --style=file -i
