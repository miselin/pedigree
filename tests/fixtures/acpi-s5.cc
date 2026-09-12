/* Copyright (c) 2026, Pedigree Developers. */
#include <cassert>
#include <vector>

#include "src/system/kernel/machine/mach_pc/AcpiS5.h"

using Bytes = std::vector<uint8_t>;

static Bytes s5(Bytes values, uint8_t count = 2) {
  Bytes result = {0x08, '\\', '_', 'S', '5', '_', 0x12, uint8_t(values.size() + 2), count};
  result.insert(result.end(), values.begin(), values.end());
  return result;
}

static bool parse(const Bytes& bytes, uint8_t expectedA = 5, uint8_t expectedB = 6) {
  uint8_t a = 255, b = 255;
  bool result = AcpiS5::find(bytes.data(), bytes.data() + bytes.size(), a, b);
  if (result)
    assert(a == expectedA && b == expectedB);
  return result;
}

int main() {
  const Bytes good = s5({0x0a, 5, 0x0a, 6});
  assert(!AcpiS5::mayHaveSleepHooks(good.data(), good.size()));
  for (uint8_t first : {'P', 'T', 'G'}) {
    const Bytes hook = {'_', first, 'T', 'S'};
    assert(AcpiS5::mayHaveSleepHooks(hook.data(), hook.size()));
    for (size_t length = 0; length < hook.size(); ++length)
      assert(!AcpiS5::mayHaveSleepHooks(hook.data(), length));
  }
  assert(parse(good));
  assert(parse(s5({0, 1}), 0, 1));
  assert(parse(s5({0x0b, 5, 0, 0x0c, 6, 0, 0, 0})));
  assert(parse(s5({0x0e, 5, 0, 0, 0, 0, 0, 0, 0, 0x0a, 6})));
  assert(!parse(s5({0x0a, 8, 0x0a, 6})));
  assert(!parse(s5({0xff, 0x0a, 6})));
  assert(!parse(s5({0, 1}, 1)));
  assert(!parse(s5({0x0a, 5, 0x0a, 6}, 4)));
  assert(!parse(s5({0x0a, 5, 0x0a, 6, 0, 0}, 2)));
  assert(parse(s5({0x0a, 5, 0x0a, 6, 0, 0}, 4)));
  assert(!parse(s5({0x0a, 5})));
  assert(!parse(s5({0x60, 0x61})));
  for (size_t length = 1; length < good.size(); ++length)
    assert(!parse(Bytes(good.begin(), good.begin() + length)));
  Bytes invalid = good;
  invalid[7] = 0x7f;
  assert(!parse(invalid));
  invalid = good;
  invalid[7] = 0;
  assert(!parse(invalid));
  Bytes multi = good;
  multi[7] = 0x47;
  multi.insert(multi.begin() + 8, 0);
  assert(parse(multi));
  Bytes root = {0x10, uint8_t(good.size() + 3), '\\', 0};
  root.insert(root.end(), good.begin(), good.end());
  assert(parse(root));
  Bytes local = good;
  local.erase(local.begin() + 1);
  assert(parse(local));
  Bytes nested = {0x10, uint8_t(local.size() + 5), '_', 'S', 'B', '_'};
  nested.insert(nested.end(), local.begin(), local.end());
  assert(!parse(nested));
  // A byte pattern inside a buffer or method is not a root namespace object.
  Bytes buffer = {0x08, 'B', 'U', 'F', '_', 0x11, uint8_t(good.size() + 2), 0};
  buffer.insert(buffer.end(), good.begin(), good.end());
  assert(!parse(buffer));
  buffer.insert(buffer.end(), good.begin(), good.end());
  assert(parse(buffer));
  Bytes method = {0x14, uint8_t(good.size() + 6), 'M', 'T', 'H', '_', 0};
  method.insert(method.end(), good.begin(), good.end());
  assert(!parse(method));
  // Reject arbitrary AML bytes without running any firmware instructions.
  for (unsigned op = 0; op < 256; ++op) {
    Bytes bytes(32, uint8_t(op));
    uint8_t a, b;
    (void)AcpiS5::find(bytes.data(), bytes.data() + bytes.size(), a, b);
  }
}
