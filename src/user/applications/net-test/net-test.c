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

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

int main(int argc, char** argv) {
  int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sock == -1) {
    printf("Couldn't get a socket: %d [%s]\n", errno, strerror(errno));
    return 1;
  }

  char tmp[2048];
  while (1) {
    // select modifies both arguments, so each iteration needs fresh state.
    struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
    fd_set readfd;
    FD_ZERO(&readfd);
    FD_SET(sock, &readfd);

    int ready = select(sock + 1, &readfd, 0, 0, &timeout);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      printf("select failed: %d [%s]\n", errno, strerror(errno));
      close(sock);
      return 1;
    }
    if (!ready)
      continue;

    int n = read(sock, tmp, sizeof(tmp));
    if (n > 0)
      printf("interface received %d bytes\n", n);
  }

  return 0;
}
