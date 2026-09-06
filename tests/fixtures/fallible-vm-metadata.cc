/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RangeList.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/Vector.h"

#include <cstdio>

extern "C" void allocation_limit(long count);
extern "C" long allocation_attempts();
extern "C" long live_allocations();

// Nontrivial Vector elements use this small C-library dependency. The fixture
// needs no kernel runtime beyond the actual template definitions.
extern "C" int overlaps(const void* first, const void* second, size_t size) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(first);
  const uintptr_t b = reinterpret_cast<uintptr_t>(second);
  return a <= b ? b - a < size : a - b < size;
}

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
      return false;                                                        \
    }                                                                      \
  } while (0)

struct Value {
  inline static int alive = 0;
  inline static int constructed = 0;
  int value;

  explicit Value(int n = 0) : value(n) {
    ++alive;
    ++constructed;
  }
  Value(const Value& other) : Value(other.value) {}
  Value& operator=(const Value&) = default;
  ~Value() {
    --alive;
  }
};

struct Key {
  inline static int alive = 0;
  inline static int constructed = 0;
  unsigned value;

  explicit Key(unsigned n = 0) : value(n) {
    ++alive;
    ++constructed;
  }
  Key& operator=(const Key&) = default;
  ~Key() {
    --alive;
  }
  bool operator==(const Key& other) const {
    return value == other.value;
  }
  bool operator>(const Key& other) const {
    return value > other.value;
  }
};

static bool tree_failure() {
  const long baseline = live_allocations();
  {
    Key key(8);
    Tree<Key, unsigned> tree;
    const int constructors = Key::constructed;

    allocation_limit(0);
    CHECK(!tree.tryInsert(key, 17U));
    CHECK(allocation_attempts() == 1);
    CHECK(tree.count() == 0 && live_allocations() == baseline);
    CHECK(Key::constructed == constructors && Key::alive == 1);

    allocation_limit(1);
    CHECK(!tree.tryInsert(key, 17U));
    CHECK(allocation_attempts() == 2);
    CHECK(tree.count() == 0 && live_allocations() == baseline);
    CHECK(Key::constructed == constructors + 1 && Key::alive == 1);

    allocation_limit(-1);
    CHECK(tree.tryInsert(key, 17U));
    CHECK(allocation_attempts() == 2 && tree.count() == 1);
    allocation_limit(0);
    CHECK(tree.tryInsert(key, 29U));
    CHECK(tree.count() == 1 && tree.lookup(key) == 29U);
    CHECK(allocation_attempts() == 0);

    Key other(9);
    CHECK(!tree.tryInsert(other, 33U));
    CHECK(allocation_attempts() == 1);
    CHECK(tree.count() == 1 && tree.lookup(key) == 29U);
    allocation_limit(0);
    tree.clear();
    CHECK(tree.count() == 0 && allocation_attempts() == 0);
  }
  CHECK(allocation_attempts() == 0);
  CHECK(live_allocations() == baseline && Key::alive == 0);
  allocation_limit(-1);
  return true;
}

static bool list_failure() {
  const long baseline = live_allocations();
  {
    Value value(17);
    List<Value, 2> list;
    const int constructors = Value::constructed;

    allocation_limit(0);
    CHECK(!list.tryPushBack(value));
    CHECK(allocation_attempts() == 1);
    CHECK(list.count() == 0 && live_allocations() == baseline);
    CHECK(Value::constructed == constructors);

    allocation_limit(1);
    CHECK(!list.tryPushBack(value));
    CHECK(allocation_attempts() == 2);
    CHECK(list.count() == 0 && Value::constructed == constructors);
    CHECK(live_allocations() == baseline + 1);

    allocation_limit(-1);
    CHECK(list.tryPushBack(value));
    CHECK(allocation_attempts() == 1);
    CHECK(list.count() == 1 && (*list.begin()).value == 17);
    allocation_limit(0);
    CHECK(!list.tryPushBack(value));
    CHECK(allocation_attempts() == 1);
    CHECK(list.count() == 1 && (*list.begin()).value == 17);

    allocation_limit(0);
    CHECK(list.popBack().value == 17);
    CHECK(list.count() == 0 && allocation_attempts() == 0);
    CHECK(list.tryPushBack(value));
    CHECK(list.count() == 1 && allocation_attempts() == 0);
    list.clear();
    CHECK(list.count() == 0 && allocation_attempts() == 0);
  }
  CHECK(allocation_attempts() == 0);
  CHECK(live_allocations() == baseline && Value::alive == 0);
  allocation_limit(-1);
  return true;
}

static bool vector_failure() {
  const long baseline = live_allocations();
  {
    Vector<Value> vector;
    CHECK(vector.tryReserve(2));
    vector.pushBack(Value(7));
    vector.pushBack(Value(9));
    const int constructors = Value::constructed;
    const int alive = Value::alive;
    Value* const original = vector.begin();

    allocation_limit(0);
    CHECK(!vector.tryReserve(9));
    CHECK(allocation_attempts() == 1);
    CHECK(vector.begin() == original && vector.size() == 2 && vector.count() == 2);
    CHECK(vector.begin()[0].value == 7 && vector.begin()[1].value == 9);
    CHECK(Value::constructed == constructors && Value::alive == alive);
    CHECK(live_allocations() == baseline + 1);

    allocation_limit(0);
    CHECK(vector.tryReserve(2));
    CHECK(!vector.tryReserve(~size_t(0)));
    CHECK(allocation_attempts() == 0);
  }
  CHECK(allocation_attempts() == 0);
  CHECK(live_allocations() == baseline && Value::alive == 0);
  allocation_limit(-1);
  return true;
}

static bool range_failure() {
  const long baseline = live_allocations();
  {
    // Reversed avoids the repository's extern instantiations of these integers.
    using Ranges = RangeList<unsigned long long, true>;
    Ranges ranges;
    CHECK(ranges.tryFree(100, 100, false));
    Ranges::Range observed(0, 0);

    allocation_limit(0);
    CHECK(!ranges.allocateSpecific(140, 20));
    CHECK(allocation_attempts() == 1 && ranges.size() == 1);
    CHECK(ranges.getRange(0, observed) && observed.address == 100 && observed.length == 100);
    CHECK(live_allocations() == baseline + 2);

    allocation_limit(1);
    CHECK(!ranges.allocateSpecific(140, 20));
    CHECK(allocation_attempts() == 2 && ranges.size() == 1);
    CHECK(ranges.getRange(0, observed) && observed.address == 100 && observed.length == 100);
    CHECK(live_allocations() == baseline + 2);

    allocation_limit(0);
    CHECK(ranges.allocateSpecific(100, 20));
    CHECK(ranges.tryFree(100, 20));
    CHECK(allocation_attempts() == 0 && ranges.size() == 1);
    CHECK(ranges.getRange(0, observed) && observed.address == 100 && observed.length == 100);

    allocation_limit(-1);
    CHECK(ranges.allocateSpecific(140, 20));
    CHECK(allocation_attempts() == 1 && ranges.size() == 2);
    CHECK(ranges.getRange(0, observed) && observed.address == 100 && observed.length == 40);
    CHECK(ranges.getRange(1, observed) && observed.address == 160 && observed.length == 40);
    allocation_limit(0);
  }
  CHECK(allocation_attempts() == 0 && live_allocations() == baseline);
  allocation_limit(-1);
  return true;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const struct {
    const char* name;
    bool (*run)();
  } cases[] = {{"tree", tree_failure},
               {"list", list_failure},
               {"vector", vector_failure},
               {"ranges", range_failure}};
  for (const auto& test : cases) {
    if (!test.run())
      return 1;
    std::printf("FALLIBLE-VM-METADATA: PASS %s\n", test.name);
  }
  std::puts("FALLIBLE-VM-METADATA: END PASS");
  return 0;
}
