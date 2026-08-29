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

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/Version.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/AdmittedThread.h"
#include "pedigree/kernel/process/Completion.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/OwnedThread.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/UniqueResource.h"

#include "modules/Module.h"
#include "modules/system/lwip/include/lwip/api.h"
#include "modules/system/lwip/include/lwip/ip_addr.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"

#define LISTEN_PORT 1234

static Tree<struct netconn*, Completion*> g_Netconns;
static Spinlock g_NetconnsLock;
static OperationBarrier* g_pClientWork = nullptr;

static Atomic<bool> g_Running(false);

struct NetconnReleaser {
  static void release(struct netconn* connection) {
    if (netconn_delete(connection) != ERR_OK) {
      panic("Status Server could not delete an owned lwIP connection.");
    }
  }
};

struct NetbufReleaser {
  static void release(struct netbuf* buffer) {
    netbuf_delete(buffer);
  }
};

using NetconnOwner = UniqueResource<struct netconn, NetconnReleaser>;
using NetbufOwner = UniqueResource<struct netbuf, NetbufReleaser>;

// lwIP deletion can wait for its core thread, so these owners belong only in
// ordinary thread context and must not be destroyed while holding a spinlock.

static OwnedThread g_ServerThread;

static bool currentThreadIsTerminating() {
  Thread* thread = Processor::information().getCurrentThread();
  return thread && thread->getUnwindState() != Thread::Continue;
}

struct ClientContext {
  explicit ClientContext(NetconnOwner&& connection) : connection(pedigree_std::move(connection)) {}

  NetconnOwner connection;

  ClientContext(const ClientContext&) = delete;
  ClientContext& operator=(const ClientContext&) = delete;
  ClientContext(ClientContext&&) = delete;
  ClientContext& operator=(ClientContext&&) = delete;
};

static void cancelClientThread(void* parameter) {
  delete reinterpret_cast<ClientContext*>(parameter);
}

struct NetconnCompletionRegistration {
  struct netconn* connection;
  bool registered;
};

static void removeNetconnCompletion(void* context) {
  NetconnCompletionRegistration* registration =
      reinterpret_cast<NetconnCompletionRegistration*>(context);
  if (!registration->registered) {
    return;
  }

  registration->registered = false;
  LockGuard<Spinlock> guard(g_NetconnsLock);
  g_Netconns.remove(registration->connection);
}

static void netconnCallback(struct netconn* conn, enum netconn_evt evt, u16_t len) {
  LockGuard<Spinlock> guard(g_NetconnsLock);
  Completion* completion = g_Netconns.lookup(conn);
  if (completion &&
      (evt == NETCONN_EVT_RCVPLUS || evt == NETCONN_EVT_SENDPLUS || evt == NETCONN_EVT_ERROR)) {
    completion->complete();
  }
}

static int clientThread(void* p) {
  if (!p)
    return 0;

  ClientContext* context = reinterpret_cast<ClientContext*>(p);
  NetconnOwner connection = pedigree_std::move(context->connection);
  delete context;

  connection->callback = netconnCallback;
  netconn_set_recvtimeout(connection.get(), 500);

  bool stillOk = true;
  bool requestComplete = false;

  String httpRequest;
  String httpResponse;
  err_t err;
  while (g_Running && !requestComplete) {
    struct netbuf* received = nullptr;
    if ((err = netconn_recv(connection.get(), &received)) != ERR_OK) {
      if (currentThreadIsTerminating()) {
        stillOk = false;
        break;
      }
      if (err == ERR_RST || err == ERR_CLSD) {
        WARNING("Unexpected disconnection from remote client.");
        stillOk = false;
        break;
      } else if (err == ERR_TIMEOUT) {
        continue;
      } else {
        ERROR("error in recv: " << lwip_strerr(err));
      }
      continue;
    }
    NetbufOwner buffer = NetbufOwner::adopt(received);

    do {
      void* data = nullptr;
      u16_t len = 0;
      netbuf_data(buffer.get(), &data, &len);

      if (stillOk && len) {
        httpRequest += String(reinterpret_cast<char*>(data), len);

        if (httpRequest.length() >= 4) {
          if (!(httpRequest.startswith("GET") || httpRequest.startswith("HEAD"))) {
            // We really don't want to deal with this.
            httpResponse.assign(
                "HTTP/1.1 400 Bad Request\r\nAllow: GET, "
                "HEAD\r\nContent-Type: text/plain; "
                "charset=utf-8\r\n\r\nThe Pedigree built-in status "
                "server only accepts GET and HEAD requests.");
            stillOk = false;
          }
        }

        if (stillOk) {
          if (StringContains(static_cast<const char*>(httpRequest), "\r\n\r\n")) {
            // no more data needed, we have the full request
            requestComplete = true;
          }
        }
      }
    } while (netbuf_next(buffer.get()) >= 0);
  }

  // no longer needing to RX any data
  netconn_shutdown(connection.get(), 1, 0);

  if (!g_Running || currentThreadIsTerminating()) {
    netconn_close(connection.get());
    return 0;
  }

  if (!stillOk) {
    if (httpResponse.length()) {
      netconn_write(connection.get(), static_cast<const char*>(httpResponse), httpResponse.length(),
                    NETCONN_COPY);
      netconn_shutdown(connection.get(), 1, 1);
    }

    netconn_close(connection.get());
    return 0;
  }

  // Build the response.
  bool bHeadRequest = !httpRequest.startswith("GET");
  bool bNotFound = false;  /// \todo add path parsing

  // Got a heap of information now - prepare to return
  size_t code = bNotFound ? 404 : 200;
  NormalStaticString statusLine;
  statusLine = "HTTP/1.1 ";
  statusLine += code;
  statusLine += " ";
  statusLine += bNotFound ? "Not Found" : "OK";

  // Build up the reply.
  String responseContent;
  if (bNotFound) {
    responseContent += "Error 404: Page not found.";
  } else {
    responseContent +=
        "<html><head><title>Pedigree - Live System Status "
        "Report</title></head><body>";
    responseContent += "<h1>Pedigree Live Status Report</h1>";
    responseContent +=
        "<p>This is a live status report from a running "
        "Pedigree system.</p>";
    responseContent += "<h3>Current Build</h3><pre>";

    {
      HugeStaticString str;
      str += "Pedigree - revision ";
      str += g_pBuildRevision;
      str += "<br />===========================<br />Built at ";
      str += g_pBuildTime;
      str += " by ";
      str += g_pBuildUser;
      str += " on ";
      str += g_pBuildMachine;
      str += "<br />Build flags: ";
      str += g_pBuildFlags;
      str += "<br />";
      responseContent += str;
    }

    responseContent += "</pre>";

    responseContent += "<h3>Network Interfaces</h3>";
    responseContent +=
        "<table border='1'><tr><th>Interface</th><th>IP "
        "Addresses</th><th>Subnet "
        "Mask</th><th>Gateway</th><th>Driver Name</th><th>MAC "
        "address</th><th>Statistics</th></tr>";
    NetworkStack& networkStack = NetworkStack::instance();
    for (size_t i = 0;; ++i) {
      NetworkStack::DeviceLease device;
      if (!networkStack.acquireDevice(i, device)) {
        break;
      }

      /// \todo switch to using netif interface for all this
      Network* card = device.device();
      StationInfo info = card->getStationInfo();

      struct netif* iface = device.interface();

      // Interface number
      responseContent += "<tr><td>";
      NormalStaticString s;
      s.append(iface->name, 2);
      s.append(iface->num);
      responseContent += s;
      if (iface == netif_default) {
        responseContent += " <b>(default interface)</b>";
      }
      responseContent += "</td>";

      // IP address(es)
      responseContent += "<td>";
      const ip4_addr_t* ip4 = netif_ip4_addr(iface);
      responseContent += ip4addr_ntoa(ip4);
      for (size_t j = 0; j < LWIP_IPV6_NUM_ADDRESSES; ++j) {
        const ip6_addr_t* ip6 = netif_ip6_addr(iface, j);
        if (ip6_addr_isany(ip6)) {
          continue;
        }
        responseContent += "<br />";
        responseContent += ip6addr_ntoa(ip6);
      }
      responseContent += "</td>";

      const ip4_addr_t* subnet4 = netif_ip4_netmask(iface);
      const ip4_addr_t* gw4 = netif_ip4_gw(iface);

      // Subnet mask
      responseContent += "<td>";
      responseContent += ip4addr_ntoa(subnet4);
      responseContent += "</td>";

      // Gateway
      responseContent += "<td>";
      responseContent += ip4addr_ntoa(gw4);
      responseContent += "</td>";

      // Driver name
      responseContent += "<td>";
      String cardName;
      card->getName(cardName);
      responseContent += cardName;
      responseContent += "</td>";

      // MAC
      responseContent += "<td>";
      responseContent += info.mac.toString();
      responseContent += "</td>";

      // Statistics
      responseContent += "<td>";
      s.clear();
      s += "Packets: ";
      s.append(info.nPackets);
      s += "<br />Dropped: ";
      s.append(info.nDropped);
      s += "<br />RX Errors: ";
      s.append(info.nBad);
      responseContent += s;
      responseContent += "</td>";

      responseContent += "</tr>";
    }
    responseContent += "</table>";

    responseContent += "<h3>VFS</h3>";
    responseContent += "<table border='1'><tr><th>Mount Point</th><th>Disk</th></tr>";

    Vector<VFS::MountSnapshot> mounts;
    VFS::instance().getMounts(mounts);

    for (const auto& mount : mounts) {
      String diskInfo;
      if (mount.hasDisk) {
        diskInfo = mount.diskParentName;
        if (diskInfo.length() && mount.diskName.length()) {
          diskInfo += " -- ";
        }
        diskInfo += mount.diskName;
      } else {
        diskInfo.assign("(no disk)", 10);
      }

      responseContent += "<tr><td>";
      responseContent += mount.path;
      responseContent += "</td><td>";
      responseContent += diskInfo;
      responseContent += "</td></tr>";
    }

    responseContent += "</table>";

#if X86_COMMON
    responseContent += "<h3>Memory Usage (KiB)</h3>";
    responseContent +=
        "<table "
        "border='1'><tr><th>Heap</th><th>Used</th><th>Free</"
        "th></tr>";
    {
      extern size_t g_FreePages;
      extern size_t g_AllocedPages;

      NormalStaticString str;
      str += "<tr><td>";
      str += (SlamAllocator::instance().heapPageCount() * TargetInfo::getPageSize()) / 1024;
      str += "</td><td>";
      str += (g_AllocedPages * TargetInfo::getPageSize()) / 1024;
      str += "</td><td>";
      str += (g_FreePages * TargetInfo::getPageSize()) / 1024;
      str += "</td></tr>";
      responseContent += str;
    }
    responseContent += "</table>";
#endif

    responseContent += "<h3>Processes</h3>";
    responseContent +=
        "<table "
        "border='1'><tr><th>PID</th><th>Description</"
        "th><th>Virtual Memory (KiB)</th><th>Physical Memory "
        "(KiB)</th><th>Shared Memory (KiB)</th>";
    for (size_t i = 0; i < Scheduler::instance().getNumProcesses(); ++i) {
      Scheduler::ProcessLease processLease;
      if (!Scheduler::instance().acquireProcess(processLease, i)) {
        continue;
      }
      responseContent += "<tr>";
      Process* pProcess = processLease.get();
      HugeStaticString str;

      ssize_t virtK = (pProcess->getVirtualPageCount() * TargetInfo::getPageSize()) / 1024;
      ssize_t physK = (pProcess->getPhysicalPageCount() * TargetInfo::getPageSize()) / 1024;
      ssize_t shrK = (pProcess->getSharedPageCount() * TargetInfo::getPageSize()) / 1024;

      /// \todo add timing
      str.append("<td>");
      str.append(pProcess->getId());
      str.append("</td><td>");
      str.append(pProcess->description());
      str.append("</td><td>");
      str.append(virtK, 10);
      str.append("</td><td>");
      str.append(physK, 10);
      str.append("</td><td>");
      str.append(shrK, 10);
      str.append("</td>");

      responseContent += str;
      responseContent += "</tr>";
    }
    responseContent += "</table>";

    responseContent += "</body></html>";
  }

  String contentLength;
  contentLength.Format("\r\nContent-Length: %d", responseContent.length());

  httpResponse.assign(statusLine, statusLine.length());
  httpResponse += contentLength;
  httpResponse += "\r\nContent-type: text/html; charset=utf-8";
  httpResponse += "\r\nConnection: close";
  httpResponse += "\r\n\r\n";
  httpResponse += responseContent;

  // The callback retains a pointer into this stack. The outer lifetime scope
  // keeps it valid until registration is removed under the callback lock.
  Completion completion;

  NetconnCompletionRegistration completionRegistration = {connection.get(), false};
  Thread::StackDiscardScope completionRegistrationScope(&removeNetconnCompletion,
                                                        &completionRegistration);
  {
    LockGuard<Spinlock> guard(g_NetconnsLock);
    g_Netconns.insert(connection.get(), &completion);
    completionRegistration.registered = true;
  }

  const err_t writeResult = netconn_write(connection.get(), static_cast<const char*>(httpResponse),
                                          httpResponse.length(), NETCONN_COPY);
  const err_t closeResult = netconn_close(connection.get());
  const bool completed = writeResult == ERR_OK && closeResult == ERR_OK && completion.wait();

  // Serialising removal with the callback ensures it has finished using the
  // stack-backed completion before this function returns or is discarded.
  removeNetconnCompletion(&completionRegistration);

  return completed ? 0 : -1;
}

static int mainThread(void*) {
  NetconnOwner server = NetconnOwner::adopt(netconn_new(NETCONN_TCP));
  if (!server) {
    ERROR("Status Server could not allocate its listener connection.");
    return -1;
  }

  // Don't block for more than ~500 ms so we can shut down the server when
  // this module is unloaded.
  netconn_set_recvtimeout(server.get(), 500);

  ip_addr_t ipaddr;
  ByteSet(&ipaddr, 0, sizeof(ipaddr));

  if (netconn_bind(server.get(), &ipaddr, LISTEN_PORT) != ERR_OK ||
      netconn_listen(server.get()) != ERR_OK) {
    ERROR("Status Server could not bind its listener connection.");
    return -1;
  }

  while (g_Running) {
    struct netconn* accepted = nullptr;
    if (netconn_accept(server.get(), &accepted) == ERR_OK) {
      NetconnOwner connection = NetconnOwner::adopt(accepted);
      ClientContext* context = new ClientContext(pedigree_std::move(connection));
      if (!context ||
          !AdmittedThread::launchDetached(clientThread, context, cancelClientThread, *g_pClientWork,
                                          "Status Server client thread")) {
        delete context;
      }
    }
    if (currentThreadIsTerminating()) {
      break;
    }
  }

  netconn_close(server.get());

  return 0;
}

static bool init() {
  g_pClientWork = new OperationBarrier;
  if (!g_pClientWork) {
    return false;
  }
  g_Running = true;
  Thread* serverThread =
      new Thread(Processor::information().getCurrentThread()->getParent(), mainThread, nullptr);
  if (!serverThread) {
    g_Running = false;
    g_pClientWork->closeAndWait();
    delete g_pClientWork;
    g_pClientWork = nullptr;
    return false;
  }
  g_ServerThread.adopt(serverThread);
  g_ServerThread->setName("Status Server main thread");
  return true;
}

static void destroy() {
  g_Running = false;
  g_pClientWork->close();
  g_ServerThread.join();
  g_pClientWork->wait();
  delete g_pClientWork;
  g_pClientWork = nullptr;
}

MODULE_INFO("Status Server", &init, &destroy, "config", "lwip", "network-stack");
MODULE_OPTIONAL_DEPENDS("confignics");
