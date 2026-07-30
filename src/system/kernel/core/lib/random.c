/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/lib.h"

static uint64_t g_Seed = 1;

#if X64
static volatile int g_FeaturesChecked = 0;
static int g_HasRdseed = 0;
static int g_HasRdrand = 0;

static void random_cpuid(
    uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
    uint32_t *ecx, uint32_t *edx)
{
    uint32_t a = leaf;
    uint32_t b = 0;
    uint32_t c = subleaf;
    uint32_t d = 0;
    __asm__ volatile(
        "cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) : : "memory");
    *eax = a;
    *ebx = b;
    *ecx = c;
    *edx = d;
}

static void random_check_features(void)
{
    if (g_FeaturesChecked)
    {
        return;
    }

    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    random_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    const uint32_t maximumLeaf = eax;

    if (maximumLeaf >= 1)
    {
        random_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        g_HasRdrand = (ecx & (1U << 30U)) != 0;
    }
    if (maximumLeaf >= 7)
    {
        random_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
        g_HasRdseed = (ebx & (1U << 18U)) != 0;
    }

    __sync_synchronize();
    g_FeaturesChecked = 1;
}

static int random_hardware_word(uint64_t *value)
{
    random_check_features();

    if (g_HasRdrand)
    {
        for (size_t attempt = 0; attempt < 10; ++attempt)
        {
            unsigned char ok = 0;
            uint64_t candidate = 0;
            __asm__ volatile(
                "rdrand %0; setc %1" : "=r"(candidate), "=qm"(ok) : : "cc");
            if (ok)
            {
                *value = candidate;
                return 1;
            }
            __asm__ volatile("pause");
        }
    }

    if (g_HasRdseed)
    {
        for (size_t attempt = 0; attempt < 64; ++attempt)
        {
            unsigned char ok = 0;
            uint64_t candidate = 0;
            __asm__ volatile(
                "rdseed %0; setc %1" : "=r"(candidate), "=qm"(ok) : : "cc");
            if (ok)
            {
                *value = candidate;
                return 1;
            }
            __asm__ volatile("pause");
        }
    }

    return 0;
}
#endif

void random_seed(uint64_t seed)
{
    g_Seed = seed;
}

uint64_t random_next()
{
    // This is a http://en.wikipedia.org/wiki/Linear_congruential_generator.
    g_Seed = (g_Seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    return g_Seed;
}

size_t hardware_random_bytes(void *buffer, size_t length)
{
#if X64
    uint8_t *output = (uint8_t *) buffer;
    size_t produced = 0;
    while (produced < length)
    {
        uint64_t value = 0;
        if (!random_hardware_word(&value))
        {
            break;
        }

        for (
            size_t byte = 0;
            byte < sizeof(value) && produced < length;
            ++byte)
        {
            output[produced++] = (uint8_t) value;
            value >>= 8U;
        }
    }
    return produced;
#else
    (void) buffer;
    (void) length;
    return 0;
#endif
}
