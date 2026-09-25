/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/BufferView.h"
#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/IntrusiveList.h"
#include "pedigree/kernel/utilities/IteratorAdapter.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/RingBuffer.h"
#include "pedigree/kernel/utilities/StaticCord.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Vector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace {
struct Input {
  const uint8_t* data;
  size_t size;
  size_t offset = 0;

  uint8_t next() {
    return size ? data[(offset++) % size] : 0;
  }

  std::string text() {
    constexpr char alphabet[] = "abcd /,\t\n";
    std::string result;
    const size_t length = next() % 9;
    for (size_t i = 0; i < length; ++i) {
      result += alphabet[next() % (sizeof(alphabet) - 1)];
    }
    return result;
  }
};

[[noreturn]] void fail(const char* component, size_t operation) {
  std::fprintf(stderr, "UtilitySequences mismatch in %s at operation %zu\n", component, operation);
  std::abort();
}

void checkString(const String& actual, const std::string& expected, size_t operation) {
  if (actual.length() != expected.size() ||
      (actual.length() && !std::equal(expected.begin(), expected.end(), actual.cstr()))) {
    fail("String", operation);
  }
  String copy(actual);
  if (!(copy == actual) || copy.hash() != actual.hash()) {
    fail("String copy/hash", operation);
  }
  StringView view = actual.view();
  if (view.length() != expected.size() || !(view == actual)) {
    fail("StringView", operation);
  }
}

void fuzzString(Input input) {
  String value;
  std::string model;
  for (size_t i = 0; i < 32; ++i) {
    const unsigned operation = input.next() % 12;
    const std::string part = input.text();
    const size_t amount = input.next() % (model.size() + 3);
    switch (operation) {
      case 0:
        value.assign(part.c_str());
        model = part;
        break;
      case 1:
        value += part.c_str();
        model += part;
        break;
      case 2:
        value.ltrim(amount);
        model.erase(0, std::min(amount, model.size()));
        break;
      case 3:
        value.rtrim(amount);
        model.resize(model.size() - std::min(amount, model.size()));
        break;
      case 4: {
        String back = value.split(amount);
        const std::string suffix = amount < model.size() ? model.substr(amount) : "";
        if (back.length() != suffix.size() ||
            (back.length() && !std::equal(suffix.begin(), suffix.end(), back.cstr()))) {
          fail("String split", i);
        }
        if (amount < model.size()) {
          model.resize(amount);
        }
        break;
      }
      case 5:
        value.chomp();
        if (!model.empty()) {
          model.pop_back();
        }
        break;
      case 6:
        value.lchomp();
        if (!model.empty()) {
          model.erase(0, 1);
        }
        break;
      case 7:
      case 8:
      case 9: {
        const auto whitespace = [](char c) { return c <= ' ' || c == '\x7f'; };
        if (operation == 7) {
          value.lstrip();
          while (!model.empty() && whitespace(model.front())) {
            model.erase(0, 1);
          }
        } else if (operation == 8) {
          value.rstrip();
          while (!model.empty() && whitespace(model.back())) {
            model.pop_back();
          }
        } else {
          value.strip();
          while (!model.empty() && whitespace(model.front())) {
            model.erase(0, 1);
          }
          while (!model.empty() && whitespace(model.back())) {
            model.pop_back();
          }
        }
        break;
      }
      case 10:
        value.clear();
        model.clear();
        break;
      case 11:
        value.reserve(model.size() + 16);
        value.downsize();
        break;
    }
    checkString(value, model, i);

    const StringView view = value.view();
    const size_t start = input.next() % (model.size() + 3);
    const size_t end = input.next() % (model.size() + 3);
    const StringView sub = view.substring(start, end, (input.next() & 1) != 0);
    const std::string expected = start < model.size() && start < end
                                     ? model.substr(start, std::min(end, model.size()) - start)
                                     : "";
    if (sub.length() != expected.size() ||
        (sub.length() && !std::equal(expected.begin(), expected.end(), sub.str())) ||
        sub.toString().length() != expected.size()) {
      fail("StringView substring", i);
    }
  }
}

void fuzzStaticString(Input input) {
  StaticString<17> value;
  std::string model;
  for (size_t i = 0; i < 32; ++i) {
    const unsigned operation = input.next() % 7;
    const std::string part = input.text();
    const size_t amount = input.next() % 24;
    switch (operation) {
      case 0:
        value.assign(part.c_str());
        model = part.substr(0, 16);
        break;
      case 1:
        value.append(part.c_str());
        model += part.substr(0, 16 - model.size());
        break;
      case 2:
        value.stripLast();
        if (!model.empty()) {
          model.pop_back();
        }
        break;
      case 3:
        value.stripFirst(amount);
        model.erase(0, std::min(amount, model.size()));
        break;
      case 4:
        value.truncate(amount);
        if (amount < model.size()) {
          model.resize(amount);
        }
        break;
      case 5:
        value.pad(amount, 'x');
        model.resize(std::min<size_t>(16, std::max(amount, model.size())), 'x');
        break;
      case 6:
        value.clear();
        model.clear();
        break;
    }
    if (value.length() != model.size() ||
        !std::equal(model.begin(), model.end(), static_cast<const char*>(value)) ||
        static_cast<const char*>(value)[model.size()] != '\0') {
      fail("StaticString", i);
    }
    const StaticString<17> copy(value);
    if (!(copy == value)) {
      fail("StaticString copy", i);
    }
  }
}

void fuzzCords(Input input) {
  std::array<std::string, 8> segments;
  const size_t count = 1 + (input.next() % segments.size());
  for (size_t i = 0; i < count; ++i) {
    segments[i] = input.text();
    if (segments[i].empty()) {
      segments[i] = "x";
    }
  }
  Cord cord;
  StaticCord<9> fixed;
  std::string model;
  for (size_t i = 0; i < count; ++i) {
    cord.append(segments[i].c_str());
    fixed.append(segments[i].c_str());
    model += segments[i];
  }
  if (cord.length() != model.size() || fixed.length() != model.size()) {
    fail("Cord length", 0);
  }
  size_t position = 0;
  for (auto it = cord.begin(); it != cord.end(); ++it) {
    if (position >= model.size() || *it != model[position++]) {
      fail("Cord iterator", position);
    }
  }
  if (position != model.size()) {
    fail("Cord iterator end", position);
  }
  position = 0;
  for (auto it = fixed.begin(); it != fixed.end(); ++it) {
    if (position >= model.size() || *it != model[position++]) {
      fail("StaticCord iterator", position);
    }
  }
  if (position != model.size()) {
    fail("StaticCord iterator end", position);
  }
  for (size_t i = 0; i < model.size(); ++i) {
    if (cord[i] != model[i] || fixed[i] != model[i]) {
      fail("Cord indexing", i);
    }
  }
  const String flattened = cord.toString();
  const String fixedFlattened = fixed.toString();
  checkString(flattened, model, 0);
  checkString(fixedFlattened, model, 0);
}

void checkVector(const Vector<int>& value, const std::vector<int>& model, size_t operation) {
  if (value.count() != model.size()) {
    fail("Vector count", operation);
  }
  for (size_t i = 0; i < model.size(); ++i) {
    if (value[i] != model[i] || value.begin()[i] != model[i]) {
      fail("Vector values", operation);
    }
  }
  Vector<int> copy(value);
  if (copy.count() != model.size()) {
    fail("Vector copy", operation);
  }
  for (size_t i = 0; i < model.size(); ++i) {
    if (copy[i] != model[i]) {
      fail("Vector copy values", operation);
    }
  }
}

void checkList(const List<int, 8>& value, const std::deque<int>& model, size_t operation) {
  if (value.count() != model.size()) {
    fail("List count", operation);
  }
  size_t i = 0;
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (i >= model.size() || *it != model[i++]) {
      fail("List iterator", operation);
    }
  }
  if (i != model.size()) {
    fail("List iterator end", operation);
  }
  i = 0;
  using Adapter = IteratorAdapter<const int, List<int, 8>::ConstIterator>;
  for (Adapter it(value.begin()), end(value.end()); !(it == end); ++it) {
    if (i >= model.size() || *it != model[i++]) {
      fail("IteratorAdapter", operation);
    }
  }
  if (i != model.size()) {
    fail("IteratorAdapter end", operation);
  }
  i = model.size();
  for (auto it = value.rbegin(); it != value.rend(); ++it) {
    if (!i || *it != model[--i]) {
      fail("List reverse iterator", operation);
    }
  }
  if (i) {
    fail("List reverse iterator end", operation);
  }
}

void fuzzContainers(Input input) {
  Vector<int> vector;
  std::vector<int> vectorModel;
  List<int, 8> list;
  std::deque<int> listModel;
  for (size_t i = 0; i < 48; ++i) {
    const unsigned operation = input.next() % 8;
    const int number = input.next();
    const size_t vectorIndex = input.next() % (vectorModel.size() + 1);
    const size_t listIndex = input.next() % (listModel.size() + 1);
    switch (operation) {
      case 0:
        vector.pushBack(number);
        vectorModel.push_back(number);
        list.pushBack(number);
        listModel.push_back(number);
        break;
      case 1:
        vector.pushFront(number);
        vectorModel.insert(vectorModel.begin(), number);
        list.pushFront(number);
        listModel.push_front(number);
        break;
      case 2:
        if (!vectorModel.empty()) {
          if (vector.popBack() != vectorModel.back()) {
            fail("Vector popBack", i);
          }
          vectorModel.pop_back();
        }
        if (!listModel.empty()) {
          if (list.popBack() != listModel.back()) {
            fail("List popBack", i);
          }
          listModel.pop_back();
        }
        break;
      case 3:
        if (!vectorModel.empty()) {
          if (vector.popFront() != vectorModel.front()) {
            fail("Vector popFront", i);
          }
          vectorModel.erase(vectorModel.begin());
        }
        if (!listModel.empty()) {
          if (list.popFront() != listModel.front()) {
            fail("List popFront", i);
          }
          listModel.pop_front();
        }
        break;
      case 4:
        vector.insert(vectorIndex, number);
        vectorModel.insert(vectorModel.begin() + vectorIndex, number);
        if (listIndex < listModel.size()) {
          auto it = list.begin();
          for (size_t j = 0; j < listIndex; ++j) {
            ++it;
          }
          list.erase(it);
          listModel.erase(listModel.begin() + listIndex);
        }
        break;
      case 5:
        if (vectorIndex < vectorModel.size()) {
          vector.erase(vectorIndex);
          vectorModel.erase(vectorModel.begin() + vectorIndex);
        }
        if (listIndex < listModel.size()) {
          auto it = list.begin();
          for (size_t j = 0; j < listIndex; ++j) {
            ++it;
          }
          list.erase(it);
          listModel.erase(listModel.begin() + listIndex);
        }
        break;
      case 6:
        if (vectorIndex < vectorModel.size()) {
          vector.setAt(vectorIndex, number);
          vectorModel[vectorIndex] = number;
        }
        list.tryPushBack(number);
        listModel.push_back(number);
        break;
      case 7:
        vector.clear((number & 1) != 0);
        vectorModel.clear();
        list.clear();
        listModel.clear();
        break;
    }
    checkVector(vector, vectorModel, i);
    checkList(list, listModel, i);
  }
}

struct Item {
  int id = 0;
  IntrusiveListNode<Item> link;
};
using ItemList = IntrusiveList<Item, &Item::link>;

void fuzzIntrusiveList(Input input) {
  std::array<Item, 16> items;
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].id = i;
  }
  ItemList list;
  std::deque<int> model;
  for (size_t i = 0; i < 48; ++i) {
    const unsigned operation = input.next() % 7;
    Item& item = items[input.next() % items.size()];
    switch (operation) {
      case 0:
      case 1:
        if (!list.contains(item)) {
          if (operation == 0) {
            list.pushBack(item);
            model.push_back(item.id);
          } else {
            list.pushFront(item);
            model.push_front(item.id);
          }
        }
        break;
      case 2:
        if (!model.empty()) {
          if (list.popBack() != &items[model.back()]) {
            fail("IntrusiveList popBack", i);
          }
          model.pop_back();
        }
        break;
      case 3:
        if (!model.empty()) {
          if (list.popFront() != &items[model.front()]) {
            fail("IntrusiveList popFront", i);
          }
          model.pop_front();
        }
        break;
      case 4:
        if (list.unlink(item)) {
          model.erase(std::find(model.begin(), model.end(), item.id));
        }
        break;
      case 5:
        if (!model.empty()) {
          auto it = list.begin();
          const size_t index = input.next() % model.size();
          for (size_t j = 0; j < index; ++j) {
            ++it;
          }
          list.erase(it);
          model.erase(model.begin() + index);
        }
        break;
      case 6:
        list.clear();
        model.clear();
        break;
    }
    if (list.count() != model.size()) {
      fail("IntrusiveList count", i);
    }
    size_t position = 0;
    for (auto it = list.begin(); it != list.end(); ++it) {
      if (position >= model.size() || it->id != model[position++]) {
        fail("IntrusiveList iterator", i);
      }
    }
    if (position != model.size()) {
      fail("IntrusiveList iterator end", i);
    }
    for (const Item& candidate : items) {
      const bool expected = std::find(model.begin(), model.end(), candidate.id) != model.end();
      if (list.contains(candidate) != expected) {
        fail("IntrusiveList contains", i);
      }
    }
  }
}

void fuzzBufferView(Input input) {
  std::array<std::array<uint8_t, 16>, 3> chunks{};
  BufferView storage[3];
  BufferViewSequence sequence(storage, 3);
  std::vector<uint8_t> model;
  for (size_t i = 0; i < chunks.size(); ++i) {
    const size_t length = 1 + (input.next() % chunks[i].size());
    BufferView view(chunks[i].data(), length);
    if (!sequence.append(view) || view.size() != length || view.first(length).size() != length ||
        view.subview(length, 0).size() != 0) {
      fail("BufferView bounds", i);
    }
    model.resize(model.size() + length);
  }
  if (sequence.size() != model.size() || sequence.count() != 3 || sequence.append(storage[0])) {
    fail("BufferViewSequence descriptors", 0);
  }
  for (size_t i = 0; i < 32; ++i) {
    const size_t offset = input.next() % (model.size() + 1);
    const size_t length = input.next() % (model.size() - offset + 1);
    std::vector<uint8_t> source(length);
    for (uint8_t& value : source) {
      value = input.next();
    }
    if (!sequence.copyFrom(source.data(), length, offset)) {
      fail("BufferViewSequence copyFrom", i);
    }
    std::copy(source.begin(), source.end(), model.begin() + offset);
    std::vector<uint8_t> actual(model.size());
    if (!sequence.copyTo(actual.data(), actual.size()) || actual != model) {
      fail("BufferViewSequence copyTo", i);
    }
  }
  sequence.clear();
  if (!sequence.empty() || sequence.count() || sequence.size()) {
    fail("BufferViewSequence clear", 0);
  }
}

void fuzzFifos(Input input) {
  constexpr size_t capacity = 8;
  Buffer<uint8_t, true> buffer(capacity);
  RingBuffer<uint8_t, capacity> ring(capacity);
  std::deque<uint8_t> bufferModel;
  std::deque<uint8_t> ringModel;
  for (size_t i = 0; i < 32; ++i) {
    const uint8_t operation = input.next() % 6;
    const uint8_t number = input.next();
    if (operation <= 2) {
      uint8_t values[8];
      const size_t length = 1 + (input.next() % 8);
      for (size_t j = 0; j < length; ++j) {
        values[j] = input.next();
      }
      const size_t available = capacity - bufferModel.size();
      size_t expected = std::min(length, available);
      size_t actual = 0;
      if (operation == 0) {
        actual = buffer.writeAvailable(values, length);
      } else if (operation == 1) {
        expected = length <= available ? length : 0;
        actual = buffer.writeAvailable(values, length, true);
      } else {
        expected = length <= available ? length : 0;
        actual = buffer.writeAtomic(values, length, false);
      }
      if (actual != expected) {
        fail("Buffer write", i);
      }
      for (size_t j = 0; j < actual; ++j) {
        bufferModel.push_back(values[j]);
      }
      const auto result = ring.tryWrite(number);
      if (ringModel.size() < capacity) {
        if (result != ring.NoError) {
          fail("RingBuffer write", i);
        }
        ringModel.push_back(number);
      } else if (result != ring.WouldBlock) {
        fail("RingBuffer full", i);
      }
    } else if (operation == 3 || operation == 4) {
      uint8_t values[8] = {};
      const size_t requested = input.next() % 9;
      const size_t expected = std::min(requested, bufferModel.size());
      const size_t actual = buffer.read(values, requested, false);
      if (actual != expected) {
        fail("Buffer read count", i);
      }
      for (size_t j = 0; j < actual; ++j) {
        if (values[j] != bufferModel.front()) {
          fail("Buffer read order", i);
        }
        bufferModel.pop_front();
      }
      uint8_t ringValue = 0;
      typename RingBuffer<uint8_t, capacity>::Error error = ring.NoError;
      Time::Timestamp zero = 0;
      const bool got = ring.read(ringValue, zero, error);
      if (got != !ringModel.empty()) {
        fail("RingBuffer read status", i);
      }
      if (got) {
        if (ringValue != ringModel.front()) {
          fail("RingBuffer read order", i);
        }
        ringModel.pop_front();
      } else if (error != ring.Empty) {
        fail("RingBuffer empty", i);
      }
    } else {
      buffer.wipe();
      bufferModel.clear();
    }
    if (buffer.getDataSize() != bufferModel.size() || buffer.getSize() != capacity ||
        ring.dataReady() != !ringModel.empty() ||
        ring.canWrite() != (ringModel.size() < capacity)) {
      fail("FIFO state", i);
    }
  }
  ring.close();
  while (!ringModel.empty()) {
    uint8_t value = 0;
    if (!ring.takeAfterClose(value) || value != ringModel.front()) {
      fail("RingBuffer close drain", ringModel.size());
    }
    ringModel.pop_front();
  }
  uint8_t value = 0;
  if (ring.takeAfterClose(value) || ring.tryWrite(1) != ring.Closed) {
    fail("RingBuffer close", 0);
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 1024) {
    return 0;
  }
  const Input input{data, size};
  fuzzString(input);
  fuzzStaticString(input);
  fuzzCords(input);
  fuzzContainers(input);
  fuzzIntrusiveList(input);
  fuzzBufferView(input);
  fuzzFifos(input);
  return 0;
}
