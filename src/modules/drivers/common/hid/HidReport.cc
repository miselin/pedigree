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

#include "modules/drivers/common/hid/HidReport.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/drivers/common/hid/HidUsages.h"
#include "modules/drivers/common/hid/HidUtils.h"

// Handy macro for mixing tag and type in a single value
#define MIX_TYPE_N_TAG(type, tag) (type | (tag << 2))

// Log macro that also outputs a number of tabs before the text
#define TABBED_LOG(tabs, text)              \
  do {                                      \
    char* sTabs = new char[(tabs * 4) + 1]; \
    ByteSet(sTabs, ' ', tabs * 4);          \
    sTabs[tabs * 4] = '\0';                 \
    DEBUG_LOG(sTabs << text);               \
    delete[] sTabs;                         \
  } while (0)

#define ITEM_LOG(tabs, type, value) TABBED_LOG(tabs, type << " (" << value << ")" << Hex)
#define ITEM_LOG_DEC(tabs, type, value) TABBED_LOG(tabs, Dec << type << " (" << value << ")" << Hex)

HidReport::HidReport()
    : m_pRootCollection(nullptr),
      m_ReportBits{},
      m_OldReports{},
      m_HasReportIds(false),
      m_Valid(false) {}

HidReport::~HidReport() {
  delete m_pRootCollection;
  for (auto* report : m_OldReports)
    delete[] report;
}
HidReport::Collection::~Collection() {
  for (auto* child : childs) {
    if (child->type == CollectionChild)
      delete child->pCollection;
    else
      delete child->pInputBlock;
    delete child;
  }
}

void HidReport::parseDescriptor(uint8_t* pDescriptor, size_t nDescriptorLength) {
  if (!pDescriptor || !nDescriptorLength || m_pRootCollection)
    return;
  m_pRootCollection = new Collection();
  m_pRootCollection->pParent = nullptr;
  // This will store all the values that change during the parsing
  LocalState currentState;
  LocalState globalStack[16];
  size_t globalDepth = 0;

  // Whether PhysMin and PhysMax form a pair
  bool bPhysPair = false;
  // Whether LogMin and LogMax form a pair
  bool bLogPair = false;

  // Pointer to the collection under which we are parsing
  Collection* pCurrentCollection = m_pRootCollection;

  // The depth of the Collection tree
  size_t nDepth = 0;

  // Parse every item in the descriptor
  for (size_t i = 0; i < nDescriptorLength; i++) {
    // A union and structure used to unpack the item's data
    union {
      struct {
        uint8_t size : 2;
        uint8_t type : 2;
        uint8_t tag : 4;
      } PACKED;
      uint8_t raw;
    } item;

    item.raw = pDescriptor[i];
    if (item.raw == 0xfe) {
      if (nDescriptorLength - i < 3 || pDescriptor[i + 1] > nDescriptorLength - i - 3)
        return;
      i += 2 + pDescriptor[i + 1];
      continue;
    }
    uint8_t size = item.size == 3 ? 4 : item.size;
    if (size > nDescriptorLength - i - 1)
      return;

    // Get the value
    uint32_t value = 0;
    if (size == 1)
      value = pDescriptor[i + 1];
    else if (size == 2)
      value = pDescriptor[i + 1] | (pDescriptor[i + 2] << 8);
    else if (size == 4)
      value = pDescriptor[i + 1] | (pDescriptor[i + 2] << 8) | (pDescriptor[i + 3] << 16) |
              (pDescriptor[i + 4] << 24);

    // Update the byte counter (we may hit a continue)
    i += size;

    // Don't allow for Main items (Input, Output, Feature, etc.) outside a
    // collection
    if (!pCurrentCollection)
      if (item.type == MainItem && item.tag != CollectionItem)
        continue;

    // Check for type and tag to find which item do we have
    switch (MIX_TYPE_N_TAG(item.type, item.tag)) {
      // Main items
      case MIX_TYPE_N_TAG(MainItem, InputItem): {
        if (currentState.nReportSize <= 0 || currentState.nReportSize > 64 ||
            currentState.nReportCount <= 0 || currentState.nReportCount > 1024)
          return;
        const uint8_t reportId = currentState.nReportID == -1 ? 0 : currentState.nReportID;
        const size_t bits = currentState.nReportSize * currentState.nReportCount;
        if (bits > 8192 - m_ReportBits[reportId])
          return;
        m_ReportBits[reportId] += bits;

        // Create a new InputBlock and set the state and type
        InputBlock* pBlock = new InputBlock();
        pBlock->state = currentState;
        if (value & InputConstant) {
          pBlock->type = InputBlock::Constant;
          ITEM_LOG(nDepth, "Input", "Constant");
        } else {
          if (value & InputVariable) {
            if (value & InputRelative) {
              pBlock->type = InputBlock::Relative;
              ITEM_LOG(nDepth, "Input", "Data, Variable, Relative");
            } else {
              pBlock->type = InputBlock::Absolute;
              ITEM_LOG(nDepth, "Input", "Data, Variable, Absolute");
            }
          } else {
            pBlock->type = InputBlock::Array;
            ITEM_LOG(nDepth, "Input", "Data, Array");
          }
        }

        // Push it into the child vector
        pCurrentCollection->childs.pushBack(new Collection::Child(pBlock));
        break;
      }
      case MIX_TYPE_N_TAG(MainItem, CollectionItem): {
        if (nDepth == 16)
          return;
        // Create a new Collection and set the state
        Collection* pCollection = new Collection();
        pCollection->pParent = pCurrentCollection;
        pCollection->state = currentState;

        // Push it into the child vector
        if (pCurrentCollection)
          pCurrentCollection->childs.pushBack(new Collection::Child(pCollection));

        // We now entered the collection
        pCurrentCollection = pCollection;
        ITEM_LOG(nDepth, "Collection", value);
        nDepth++;
        break;
      }
      case MIX_TYPE_N_TAG(MainItem, EndCollectionItem):
        // Move up to the parent
        if (pCurrentCollection == m_pRootCollection || !nDepth)
          return;
        pCurrentCollection = pCurrentCollection->pParent;
        nDepth--;
        TABBED_LOG(nDepth, "End Collection");
        break;

      // Global items (set various global variables)
      case MIX_TYPE_N_TAG(GlobalItem, UsagePageItem):
        currentState.nUsagePage = value;
        ITEM_LOG_DEC(nDepth, "Usage Page", value);
        break;
      case MIX_TYPE_N_TAG(GlobalItem, LogMinItem):
        currentState.nLogMin = value;

        if (currentState.nLogMax != ~0 && !bLogPair) {
          HidUtils::fixNegativeMinimum(currentState.nLogMin, currentState.nLogMax);
          bLogPair = true;
          ITEM_LOG(nDepth, "Logical Minimum", currentState.nLogMin);
          ITEM_LOG(nDepth, "Logical Maximum", currentState.nLogMax);
        } else
          bLogPair = false;
        break;
      case MIX_TYPE_N_TAG(GlobalItem, LogMaxItem):
        currentState.nLogMax = value;

        if (currentState.nLogMin != ~0 && !bLogPair) {
          HidUtils::fixNegativeMinimum(currentState.nLogMin, currentState.nLogMax);
          bLogPair = true;
          ITEM_LOG(nDepth, "Logical Minimum", currentState.nLogMin);
          ITEM_LOG(nDepth, "Logical Maximum", currentState.nLogMax);
        } else
          bLogPair = false;
        break;
      case MIX_TYPE_N_TAG(GlobalItem, PhysMinItem):
        currentState.nPhysMin = value;

        if (currentState.nPhysMax != ~0 && !bPhysPair) {
          HidUtils::fixNegativeMinimum(currentState.nPhysMin, currentState.nPhysMax);
          bPhysPair = true;
          ITEM_LOG_DEC(nDepth, "Physical Minimum", currentState.nPhysMin);
          ITEM_LOG_DEC(nDepth, "Physical Maximum", currentState.nPhysMax);
        } else
          bPhysPair = false;
        break;
      case MIX_TYPE_N_TAG(GlobalItem, PhysMaxItem):
        currentState.nPhysMax = value;

        if (currentState.nPhysMin != ~0 && !bPhysPair) {
          HidUtils::fixNegativeMinimum(currentState.nPhysMin, currentState.nPhysMax);
          bPhysPair = true;
          ITEM_LOG_DEC(nDepth, "Physical Minimum", currentState.nPhysMin);
          ITEM_LOG_DEC(nDepth, "Physical Maximum", currentState.nPhysMax);
        } else
          bPhysPair = false;
        break;
      case MIX_TYPE_N_TAG(GlobalItem, ReportSizeItem):
        currentState.nReportSize = value;
        ITEM_LOG_DEC(nDepth, "Report Size", value);
        break;
      case MIX_TYPE_N_TAG(GlobalItem, ReportIDItem):
        if (!value || value > 255)
          return;
        currentState.nReportID = value;
        m_HasReportIds = true;
        ITEM_LOG_DEC(nDepth, "Report ID", value);
        break;
      case MIX_TYPE_N_TAG(GlobalItem, ReportCountItem):
        currentState.nReportCount = value;
        ITEM_LOG_DEC(nDepth, "Report Count", value);
        break;

      case MIX_TYPE_N_TAG(GlobalItem, PushItem):
        if (globalDepth == 16)
          return;
        globalStack[globalDepth++].copyGlobals(currentState);
        break;
      case MIX_TYPE_N_TAG(GlobalItem, PopItem):
        if (!globalDepth)
          return;
        currentState.copyGlobals(globalStack[--globalDepth]);
        break;

      // Local items (set various local variables, mostly usage-related)
      case MIX_TYPE_N_TAG(LocalItem, UsageItem):
        if (!currentState.pUsages)
          currentState.pUsages = new Vector<size_t>();
        currentState.pUsages->pushBack(value);
        ITEM_LOG_DEC(nDepth, "Usage", value);
        break;
      case MIX_TYPE_N_TAG(LocalItem, UsageMinItem):
        currentState.nUsageMin = value;
        ITEM_LOG_DEC(nDepth, "Usage Minimum", value);
        break;
      case MIX_TYPE_N_TAG(LocalItem, UsageMaxItem):
        currentState.nUsageMax = value;
        ITEM_LOG_DEC(nDepth, "Usage Maximum", value);
        break;
      default:
        ITEM_LOG_DEC(nDepth, "Unknown", item.type << " " << item.tag << " " << Hex << value);
    }

    // Reset the local values in currentState (will also delete the usage
    // vector if it's not used)
    if (item.type == MainItem)
      currentState.resetLocalValues();
  }
  if (nDepth || globalDepth || pCurrentCollection != m_pRootCollection ||
      (m_HasReportIds && m_ReportBits[0]))
    return;
  for (size_t id = 0; id < 256; ++id) {
    if (m_ReportBits[id]) {
      m_OldReports[id] = new uint8_t[(m_ReportBits[id] + 7) / 8]();
      m_Valid = true;
    }
  }
}

void HidReport::feedInput(uint8_t* pBuffer, uint8_t*, size_t nBufferSize) {
  if (!m_Valid || !pBuffer || !nBufferSize)
    return;
  const uint8_t reportId = m_HasReportIds ? *pBuffer++ : 0;
  if (m_HasReportIds)
    --nBufferSize;
  const size_t reportBytes = (m_ReportBits[reportId] + 7) / 8;
  if (!reportBytes || nBufferSize < reportBytes)
    return;
  size_t bitOffset = 0;
  m_pRootCollection->feedInput(pBuffer, m_OldReports[reportId], reportBytes, bitOffset, reportId);
  MemoryCopy(m_OldReports[reportId], pBuffer, reportBytes);
}

void HidReport::Collection::feedInput(uint8_t* pBuffer, uint8_t* pOldBuffer, size_t nBufferSize,
                                      size_t& nBitOffset, uint8_t reportId) {
  // Send input to each child
  for (size_t i = 0; i < childs.count(); i++) {
    Child* pChild = childs[i];

    // If it's a collection, just forward the arguments
    if (pChild->type == CollectionChild)
      pChild->pCollection->feedInput(pBuffer, pOldBuffer, nBufferSize, nBitOffset, reportId);

    // If it's an input block, we need to send also a guessed device type
    if (pChild->type == InputBlockChild)
      pChild->pInputBlock->feedInput(pBuffer, pOldBuffer, nBufferSize, nBitOffset,
                                     guessInputDevice(), reportId);
  }
}

HidDeviceType HidReport::Collection::guessInputDevice() {
  // Go all the way up looking for valid usages
  Collection* pCollection = this;
  while (pCollection) {
    // Check for Mouse, Joystick, Keyboard and Keypad usages
    if (pCollection->state.nUsagePage == HidUsagePages::GenericDesktop) {
      if (pCollection->state.getUsageByIndex(0) == HidUsages::Mouse)
        return Mouse;
      if (pCollection->state.getUsageByIndex(0) == HidUsages::Joystick)
        return Joystick;
      if (pCollection->state.getUsageByIndex(0) == HidUsages::Keyboard)
        return Keyboard;
      if (pCollection->state.getUsageByIndex(0) == HidUsages::Keypad)
        return Keyboard;
    }

    // Go up
    pCollection = pCollection->pParent;
  }

  // We found nothing
  return UnknownDevice;
}

void HidReport::InputBlock::feedInput(uint8_t* pBuffer, uint8_t* pOldBuffer, size_t nBufferSize,
                                      size_t& nBitOffset, HidDeviceType deviceType,
                                      uint8_t reportId) {
  const uint8_t id = state.nReportID == -1 ? 0 : state.nReportID;
  if (id != reportId)
    return;

  // Compute the size of this block
  int64_t nBlockSize = state.nReportCount * state.nReportSize;

  // We don't want to cross the end of the buffer and we can skip constant
  // inputs
  if ((nBitOffset + nBlockSize > nBufferSize * 8) || type == Constant) {
    nBitOffset += nBlockSize;
    return;
  }

  // Process each field
  for (int64_t i = 0; i < state.nReportCount; i++) {
    uint64_t nValue =
        HidUtils::getBufferField(pBuffer, nBitOffset + i * state.nReportSize, state.nReportSize);
    int64_t nRelativeValue = 0;
    switch (type) {
      case Absolute:
        // Here we have to take the current absolute value and
        // subtract it by the old value to get a relative value
        nRelativeValue =
            nValue - HidUtils::getBufferField(pOldBuffer, nBitOffset + i * state.nReportSize,
                                              state.nReportSize);

        if (nRelativeValue)
          HidUtils::sendInputToManager(deviceType, state.nUsagePage, state.getUsageByIndex(i),
                                       nRelativeValue);
        break;
      case Relative:
        // The actual value is relative
        nRelativeValue = nValue;
        if (state.nLogMin < 0 && state.nReportSize < 64 &&
            (nValue & (uint64_t{1} << (state.nReportSize - 1))))
          nRelativeValue = static_cast<int64_t>(nValue | (~uint64_t{0} << state.nReportSize));

        if (nRelativeValue)
          HidUtils::sendInputToManager(deviceType, state.nUsagePage, state.getUsageByIndex(i),
                                       nRelativeValue);
        break;
      case Array:
        // A non-zero value in an array means a holded key/button
        if (nValue) {
          // Check if this array entry is new
          bool bNew = true;
          for (int64_t j = 0; j < state.nReportCount; j++) {
            if (HidUtils::getBufferField(pOldBuffer, nBitOffset + j * state.nReportSize,
                                         state.nReportSize) == nValue) {
              bNew = false;
              break;
            }
          }

          // If it's new, we have a keyDown/buttonDown
          if (bNew)
            HidUtils::sendInputToManager(deviceType, state.nUsagePage, nValue, 1);
        }
        break;
      // This is to please GCC
      case Constant:
        break;
    }
  }

  // Special case here: check for array entries that disapeared
  // (keys/buttons that were released since last time)
  if (type == Array) {
    for (int64_t i = 0; i < state.nReportCount; i++) {
      uint64_t nOldValue = HidUtils::getBufferField(pOldBuffer, nBitOffset + i * state.nReportSize,
                                                    state.nReportSize);
      if (nOldValue) {
        // Check if this array entry disapeared
        bool bDisapeared = true;
        for (int64_t j = 0; j < state.nReportCount; j++) {
          if (HidUtils::getBufferField(pBuffer, nBitOffset + j * state.nReportSize,
                                       state.nReportSize) == nOldValue) {
            bDisapeared = false;
            break;
          }
        }

        // If it disapeared, we have a keyUp/buttonUp
        if (bDisapeared)
          HidUtils::sendInputToManager(deviceType, state.nUsagePage, nOldValue, -1);
      }
    }
  }

  // Update the bit offset
  nBitOffset += nBlockSize;
}

HidReport::LocalState::LocalState()
    : nUsagePage(~0),
      nLogMin(~0),
      nLogMax(~0),
      nPhysMin(~0),
      nPhysMax(~0),
      nReportSize(~0),
      nReportID(~0),
      nReportCount(~0),
      pUsages(0),
      nUsageMin(~0),
      nUsageMax(~0) {}

HidReport::LocalState::~LocalState() {
  // The parser is required to set pUsages to zero when it's actually
  // used
  if (pUsages)
    delete pUsages;
}

/// Resets the local values (called every time a Main item occurs)
void HidReport::LocalState::resetLocalValues() {
  // The parser is required to set pUsages to zero when it's actually
  // used
  if (pUsages) {
    delete pUsages;
    pUsages = 0;
  }
  nUsageMin = ~0;
  nUsageMax = ~0;
}

/// Returns the usage number represented by the given index
uint16_t HidReport::LocalState::getUsageByIndex(uint16_t nUsageIndex) {
  // If there's an usage vector, return the usage value in it or zero
  // if the index is not valid
  if (pUsages)
    return nUsageIndex < pUsages->count() ? (*pUsages)[nUsageIndex] : 0;

  // The usage value if in range, 0 otherwise
  return (nUsageMin + nUsageIndex) <= nUsageMax ? nUsageMin + nUsageIndex : 0;
}

/// Copy constructor
// This assignment transfers the usage vector from the source state.
HidReport::LocalState& HidReport::LocalState::operator=(LocalState& s) {
  copyGlobals(s);
  pUsages = s.pUsages;
  nUsageMin = s.nUsageMin;
  nUsageMax = s.nUsageMax;
  s.pUsages = nullptr;
  return *this;
}
void HidReport::LocalState::copyGlobals(const LocalState& s) {
  nUsagePage = s.nUsagePage;
  nLogMin = s.nLogMin;
  nLogMax = s.nLogMax;
  nPhysMin = s.nPhysMin;
  nPhysMax = s.nPhysMax;
  nReportSize = s.nReportSize;
  nReportID = s.nReportID;
  nReportCount = s.nReportCount;
}
