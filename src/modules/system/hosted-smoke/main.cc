/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "modules/Module.h"
#include "pedigree/kernel/Log.h"

extern void system_reset();
extern bool runHostedWaitRegressions();

static bool entry()
{
    if (!runHostedWaitRegressions())
    {
        system_reset();
        return true;
    }

    NOTICE("HOSTED-SMOKE: populated initrd executed");
    system_reset();
    return true;
}

static void exit()
{
}

MODULE_INFO(
    "hosted-smoke", &entry, &exit, "config", "fat", "rawfs", "usb");
