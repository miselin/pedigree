/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *stage = argc > 1 ? argv[1] : "shutdown";

    klog(LOG_INFO, "HOSTED-SMOKE: simple userspace command ran");
    if (!strcmp(stage, "shutdown"))
    {
        klog(LOG_INFO, "HOSTED-SMOKE: requesting clean shutdown");
    }

    if (reboot(0) != 0)
    {
        klog(LOG_ERR, "HOSTED-SMOKE: shutdown request failed: %d", errno);
        return 1;
    }

    return 0;
}
