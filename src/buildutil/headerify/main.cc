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

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *inFile = nullptr;
    const char *outFile = nullptr;
    const char *varName = nullptr;

    // Load options.
    int c;
    while ((c = getopt(argc, argv, "i:o:v:")) != -1)
    {
        switch (c)
        {
            case 'i':
                inFile = optarg;
                break;
            case 'o':
                outFile = optarg;
                break;
            case 'v':
                varName = optarg;
                break;
            case '?':
                std::cerr << "Option -" << optopt << " is unknown."
                          << std::endl;
                break;
            default:
                return 1;
        }
    }

    if (!(inFile && outFile))
    {
        std::cerr << "Both an input and output file must be specified." << std::endl;
        return 1;
    }

    if (!varName)
    {
        varName = "autogen";
    }

    // build the file
    FILE *ifp = fopen(inFile, "rb");
    FILE *ofp = fopen(outFile, "w+");

    fwrite("const char ", 11, 1, ofp);
    fwrite(varName, strlen(varName), 1, ofp);
    fwrite("[] = {\n", 7, 1, ofp);

    size_t fileLength = 0;

    while (!feof(ifp))
    {
        unsigned char buffer[10];
        ssize_t n = fread(buffer, 1, 10, ifp);
        if (n < 0)
        {
            std::cerr << "Read failed: " << strerror(errno) << std::endl;
            return 1;
        }
        else if (n == 0)
        {
            break;
        }

        fileLength += n;

        for (ssize_t i = 0; i < n; ++i)
        {
            char formatted[16];
            size_t len = sprintf(formatted, "0x%02x, ", buffer[i]);
            fwrite(formatted, len, 1, ofp);
        }

        fwrite("\n", 1, 1, ofp);
    }

    fwrite("};\n", 3, 1, ofp);

    char formatted[512];
    size_t len = sprintf(formatted, "unsigned long %s_length = %zd;\n", varName, fileLength);
    fwrite(formatted, len, 1, ofp);

    fclose(ifp);
    fclose(ofp);

    return 0;
}
