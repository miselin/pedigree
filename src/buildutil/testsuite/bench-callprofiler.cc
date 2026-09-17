/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace {

using Address = std::uintptr_t;
using Tick = std::uint64_t;

struct FunctionStats {
  Tick inclusive = 0;
  Tick self = 0;
};

struct EdgeKey {
  Address caller = 0;
  Address callee = 0;

  bool operator==(const EdgeKey& other) const {
    return caller == other.caller && callee == other.callee;
  }
};

struct EdgeStats {
  std::uint64_t calls = 0;
  Tick inclusive = 0;
};

struct FunctionRecord {
  Address address = 0;
  FunctionStats stats;
};

struct EdgeRecord {
  EdgeKey key;
  EdgeStats stats;
};

struct Frame {
  Address function = 0;
  Tick start = 0;
  Tick child = 0;
};

constexpr std::size_t kMaxStackDepth = 256;
constexpr std::size_t kMaxFunctions = 1024;
constexpr std::size_t kMaxEdges = 4096;

volatile bool g_Active = false;
volatile bool g_Paused = false;
// Entry/exit hooks must not allocate or contend with the code they measure.
FunctionRecord g_Functions[kMaxFunctions];
std::size_t g_FunctionCount = 0;
EdgeRecord g_Edges[kMaxEdges];
std::size_t g_EdgeCount = 0;
std::string g_OutputPath;
bool g_OutputRegistered = false;

thread_local Frame g_Stack[kMaxStackDepth];
thread_local std::size_t g_StackDepth = 0;
thread_local std::size_t g_DroppedDepth = 0;
thread_local bool g_InHook = false;

std::size_t hash_address(Address address) {
  address ^= address >> 30;
  address *= static_cast<Address>(0xbf58476d1ce4e5b9ULL);
  address ^= address >> 27;
  address *= static_cast<Address>(0x94d049bb133111ebULL);
  return static_cast<std::size_t>(address ^ (address >> 31));
}

FunctionRecord* find_function(Address address) {
  std::size_t index = hash_address(address) & (kMaxFunctions - 1);
  for (std::size_t probe = 0; probe < kMaxFunctions; ++probe) {
    FunctionRecord& record = g_Functions[index];
    if (record.address == address) {
      return &record;
    }
    if (record.address == 0) {
      record = {address, {}};
      ++g_FunctionCount;
      return &record;
    }
    index = (index + 1) & (kMaxFunctions - 1);
  }
  return nullptr;
}

EdgeRecord* find_edge(Address caller, Address callee) {
  const std::size_t hash =
      hash_address(caller) ^ (hash_address(callee) + static_cast<std::size_t>(0x9e3779b9));
  std::size_t index = hash & (kMaxEdges - 1);
  for (std::size_t probe = 0; probe < kMaxEdges; ++probe) {
    EdgeRecord& record = g_Edges[index];
    if (record.key.caller == caller && record.key.callee == callee) {
      return &record;
    }
    if (record.key.caller == 0) {
      record = {{caller, callee}, {}};
      ++g_EdgeCount;
      return &record;
    }
    index = (index + 1) & (kMaxEdges - 1);
  }
  return nullptr;
}

Tick ticks_now() {
#if defined(__APPLE__)
  return mach_absolute_time();
#else
  timespec value{};
  clock_gettime(CLOCK_MONOTONIC, &value);
  return static_cast<Tick>(value.tv_sec) * 1000000000ULL + static_cast<Tick>(value.tv_nsec);
#endif
}

std::string symbol_name(Address address) {
#if defined(__APPLE__) || defined(__linux__)
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_sname) {
    int status = 0;
    char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
      std::string result(demangled);
      std::free(demangled);
      return result;
    }
    std::free(demangled);
    return info.dli_sname;
  }
#endif
  return "0x" + std::to_string(address);
}

void dump_profile() {
  if (g_OutputPath.empty()) {
    return;
  }
  __atomic_store_n(&g_Active, false, __ATOMIC_RELEASE);

  std::vector<FunctionRecord> functions;
  functions.reserve(g_FunctionCount);
  Tick summary = 0;
  for (std::size_t i = 0; i < kMaxFunctions; ++i) {
    if (g_Functions[i].address != 0) {
      functions.push_back(g_Functions[i]);
      summary += g_Functions[i].stats.self;
    }
  }
  std::sort(functions.begin(), functions.end(),
            [](const FunctionRecord& left, const FunctionRecord& right) {
              return left.stats.inclusive > right.stats.inclusive;
            });

  std::vector<EdgeRecord> edges;
  edges.reserve(g_EdgeCount);
  for (std::size_t i = 0; i < kMaxEdges; ++i) {
    if (g_Edges[i].key.caller != 0) {
      edges.push_back(g_Edges[i]);
    }
  }

  std::ofstream output(g_OutputPath);
  if (!output) {
    return;
  }

  struct NamedAddress {
    Address address;
    std::string name;
  };
  std::vector<NamedAddress> names;
  names.reserve(functions.size());
  const auto name_for = [&names](Address address) -> const std::string& {
    for (const NamedAddress& named : names) {
      if (named.address == address) {
        return named.name;
      }
    }
    names.push_back({address, symbol_name(address)});
    return names.back().name;
  };

  output << "version: 1\n"
         << "creator: pedigree benchmark call instrumentation\n"
         << "events: Ticks\n"
         << "summary: " << summary << "\n\n";

  for (const FunctionRecord& function : functions) {
    output << "fl=instrumented\n"
           << "fn=" << name_for(function.address) << "\n"
           << "0 " << function.stats.self << "\n";

    std::vector<EdgeRecord> function_edges;
    for (const EdgeRecord& edge : edges) {
      if (edge.key.caller == function.address) {
        function_edges.push_back(edge);
      }
    }
    std::sort(function_edges.begin(), function_edges.end(),
              [](const EdgeRecord& left, const EdgeRecord& right) {
                return left.stats.inclusive > right.stats.inclusive;
              });
    for (const EdgeRecord& edge : function_edges) {
      output << "cfn=" << name_for(edge.key.callee) << "\n"
             << "calls=" << edge.stats.calls << " 0\n"
             << "0 " << edge.stats.inclusive << "\n";
    }
    output << "\n";
  }
}

}  // namespace

extern "C" void pedigree_benchmark_profile_start() {
  if (!g_OutputRegistered) {
    const char* configured_path = std::getenv("PEDIGREE_PROFILE_OUTPUT");
    g_OutputPath = configured_path ? configured_path : "scheduler.callgrind";
    std::atexit(dump_profile);
    g_OutputRegistered = true;
  }

  for (FunctionRecord& function : g_Functions) {
    function = {};
  }
  for (EdgeRecord& edge : g_Edges) {
    edge = {};
  }
  g_FunctionCount = 0;
  g_EdgeCount = 0;
  g_StackDepth = 0;
  g_DroppedDepth = 0;
  __atomic_store_n(&g_Paused, false, __ATOMIC_RELAXED);
  __atomic_store_n(&g_Active, true, __ATOMIC_RELEASE);
}

extern "C" void pedigree_benchmark_profile_pause() {
  __atomic_store_n(&g_Paused, true, __ATOMIC_RELEASE);
}

extern "C" void pedigree_benchmark_profile_resume() {
  __atomic_store_n(&g_Paused, false, __ATOMIC_RELEASE);
}

extern "C" void __cyg_profile_func_enter(void* function, void*) {
  if (g_InHook) {
    return;
  }
  g_InHook = true;

  if (!__atomic_load_n(&g_Active, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&g_Paused, __ATOMIC_ACQUIRE)) {
    g_InHook = false;
    return;
  }

  if (g_DroppedDepth != 0 || g_StackDepth == kMaxStackDepth) {
    ++g_DroppedDepth;
    g_InHook = false;
    return;
  }

  const Address address = reinterpret_cast<Address>(function);
  if (g_StackDepth != 0) {
    const Address caller = g_Stack[g_StackDepth - 1].function;
    if (EdgeRecord* edge = find_edge(caller, address)) {
      ++edge->stats.calls;
    }
  }
  g_Stack[g_StackDepth++] = {address, ticks_now(), 0};
  g_InHook = false;
}

extern "C" void __cyg_profile_func_exit(void* function, void*) {
  if (g_InHook) {
    return;
  }
  g_InHook = true;

  if (!__atomic_load_n(&g_Active, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&g_Paused, __ATOMIC_ACQUIRE)) {
    g_InHook = false;
    return;
  }

  if (g_DroppedDepth != 0) {
    --g_DroppedDepth;
    g_InHook = false;
    return;
  }
  if (g_StackDepth == 0) {
    g_InHook = false;
    return;
  }

  Frame frame = g_Stack[--g_StackDepth];
  const Tick inclusive = ticks_now() - frame.start;
  const Tick self = inclusive - frame.child;
  if (FunctionRecord* function_record = find_function(frame.function)) {
    function_record->stats.inclusive += inclusive;
    function_record->stats.self += self;
  }

  if (g_StackDepth != 0) {
    Frame& parent = g_Stack[g_StackDepth - 1];
    parent.child += inclusive;
    if (EdgeRecord* edge = find_edge(parent.function, frame.function)) {
      edge->stats.inclusive += inclusive;
    }
  }

  (void)function;
  g_InHook = false;
}
