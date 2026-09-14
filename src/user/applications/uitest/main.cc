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

#include <cerrno>
#include <cstdio>
#include <optional>
#include <poll.h>

#include "demo-app.h"
#include "libui/types.h"

int main(int argc, char** argv) {
  const std::optional<demo::Options> options = demo::parseOptions(argc, argv);
  if (!options)
    return 2;

  libui::client::Client client = libui::client::Client::connect(options->socketPath);
  if (!client.valid()) {
    std::fprintf(stderr, "uitest could not connect to compositor\n");
    return 1;
  }

  demo::Options redOptions = *options;
  redOptions.title = "UI Test A";
  redOptions.background = libui::packColor({255, 0, 0, 255});
  std::optional<libui::client::Window> redResult = demo::createWindow(client, redOptions);
  if (!redResult) {
    std::fprintf(stderr, "uitest could not create red window\n");
    return 1;
  }

  demo::Options greenOptions = *options;
  greenOptions.title = "UI Test B";
  greenOptions.x += 40;
  greenOptions.y += 40;
  greenOptions.background = libui::packColor({0, 255, 0, 255});
  std::optional<libui::client::Window> greenResult = demo::createWindow(client, greenOptions);
  if (!greenResult) {
    std::fprintf(stderr, "uitest could not create green window\n");
    return 1;
  }

  libui::client::Window& redWindow = redResult.value();
  libui::client::Window& greenWindow = greenResult.value();
  bool redClosed = false;
  bool greenClosed = false;

  redWindow.setWindowProc([&redClosed](libui::client::Window&, const libui::client::Event& event) {
    if (event.type() == libui::client::Event::Type::Close) {
      redClosed = true;
      return true;
    }
    return false;
  });
  greenWindow.setWindowProc(
      [&greenClosed](libui::client::Window&, const libui::client::Event& event) {
        if (event.type() == libui::client::Event::Type::Close) {
          greenClosed = true;
          return true;
        }
        return false;
      });

  auto dispatchPending = [&] {
    for (;;) {
      bool progressed = false;
      if (client.dispatch(redWindow, false, false))
        progressed = true;
      if (client.dispatch(greenWindow, false, false))
        progressed = true;
      if (!progressed || !client.hasPendingMessages())
        break;
    }
  };

  if (!client.setNonBlocking(true) || !client.paint(redWindow) || !client.paint(greenWindow))
    return 1;

  while ((!redClosed || !greenClosed) && client.valid()) {
    if ((!redClosed && !client.paintDirty(redWindow)) ||
        (!greenClosed && !client.paintDirty(greenWindow)))
      return 1;

    if (client.hasPendingMessages()) {
      dispatchPending();
      continue;
    }

    pollfd descriptor{client.descriptor(), POLLIN, 0};
    const int result = ::poll(&descriptor, 1, 50);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (result == 0)
      continue;
    if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))
      break;
    if (descriptor.revents & POLLIN)
      dispatchPending();
  }

  return (redClosed && greenClosed) ? 0 : 1;
}
