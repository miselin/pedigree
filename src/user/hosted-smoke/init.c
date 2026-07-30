/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <sys/klog.h>
#include <sys/reboot.h>

int main(void)
{
    klog(LOG_INFO, "HOSTED-SMOKE: init launched");

    if (reboot(0) != 0)
    {
        klog(LOG_ERR, "HOSTED-SMOKE: shutdown request failed: %d", errno);
        return 1;
    }

    return 0;
}
