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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

bool GraphicsService::serve(ServiceFeatures::Type type, void* pData, size_t dataLen) {
  if (!pData)
    return false;

  bool mutating = false;
  if (type & ServiceFeatures::touch) {
    if (dataLen < sizeof(GraphicsProvider))
      return false;
    mutating = true;
  } else if (type & ServiceFeatures::withdraw) {
    if (dataLen < sizeof(GraphicsProvider))
      return false;
    mutating = true;
  } else if (type & ServiceFeatures::probe) {
    if (dataLen < sizeof(GraphicsParameters))
      return false;
  } else {
    return false;
  }

  const bool interruptsEnabled = Processor::getInterrupts();
  if (mutating && !interruptsEnabled)
    return false;

  TerminationDeferral terminationDeferral(interruptsEnabled);
  const bool lockAcquired =
      interruptsEnabled ? m_ProviderLock.acquire() : m_ProviderLock.tryAcquire();
  if (!lockAcquired)
    return false;

  bool success = false;
  if (type & ServiceFeatures::touch) {
    GraphicsProvider* pProvider = reinterpret_cast<GraphicsProvider*>(pData);
    bool alreadyRegistered = false;
    for (List<GraphicsProvider*>::Iterator it = m_Providers.begin(); it != m_Providers.end();
         ++it) {
      if (*it == pProvider) {
        alreadyRegistered = true;
        break;
      }
    }

    if (!alreadyRegistered)
      m_Providers.pushBack(pProvider);

    ProviderPair bestProvider = determineBestProvider();
    m_pCurrentProvider = bestProvider.bestBase;
    m_pCurrentTextProvider = bestProvider.bestText;
    success = true;
  } else if (type & ServiceFeatures::withdraw) {
    GraphicsProvider* pProvider = reinterpret_cast<GraphicsProvider*>(pData);
    for (List<GraphicsProvider*>::Iterator it = m_Providers.begin(); it != m_Providers.end();
         ++it) {
      if (*it != pProvider)
        continue;

      m_Providers.erase(it);
      ProviderPair bestProvider = determineBestProvider();
      m_pCurrentProvider = bestProvider.bestBase;
      m_pCurrentTextProvider = bestProvider.bestText;
      success = true;
      break;
    }
  } else if (type & ServiceFeatures::probe) {
    GraphicsParameters* params = reinterpret_cast<GraphicsParameters*>(pData);
    params->providerFound = false;

    if (params->wantTextMode) {
      if (m_pCurrentTextProvider) {
        MemoryCopy(&params->providerResult, m_pCurrentTextProvider, sizeof(GraphicsProvider));
        params->providerFound = true;
        success = true;
      }
    } else if (m_pCurrentProvider) {
      MemoryCopy(&params->providerResult, m_pCurrentProvider, sizeof(GraphicsProvider));
      params->providerFound = true;
      success = true;
    }
  }

  m_ProviderLock.release();
  return success;
}

GraphicsService::ProviderPair GraphicsService::determineBestProvider() {
  ProviderPair result;
  result.bestBase = 0;
  result.bestText = 0;

  uint64_t bestPoints = 0;
  uint64_t bestTextPoints = 0;
  for (List<GraphicsProvider*>::Iterator it = m_Providers.begin(); it != m_Providers.end(); it++) {
    if (!*it)
      continue;

    GraphicsProvider* pProvider = *it;

    uint64_t points = 0;
    uint64_t textPoints = 0;

    // Hardware acceleration points
    if (pProvider->bHardwareAccel)
      points +=
          (0x10000ULL * 0x10000ULL) * 32ULL;  // 16384x16384x32, hard to beat if no hardware accel

    // Maximums points (highest resolution in bits)
    points += pProvider->maxWidth * pProvider->maxHeight * pProvider->maxDepth;
    textPoints += pProvider->maxTextWidth * pProvider->maxTextHeight;

    if (!pProvider->bTextModes) {
      textPoints *= 0;
    }

    String name;
    pProvider->pDisplay->getName(name);
    DEBUG_LOG("GraphicsService: provider with display name '"
              << name << "' got " << points << " points (" << textPoints << " text points)");

    // Is this the new best?
    if (points > bestPoints) {
      bestPoints = points;
      result.bestBase = pProvider;

      DEBUG_LOG("  => new best provider");
    }

    if (textPoints > bestTextPoints) {
      bestTextPoints = textPoints;
      result.bestText = pProvider;

      DEBUG_LOG("  => new best text provider");
    }
  }

  return result;
}
