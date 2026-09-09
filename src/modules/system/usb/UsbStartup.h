/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef USB_STARTUP_H
#define USB_STARTUP_H
#include <stddef.h>

namespace UsbStartup {
enum class Outcome { Neutral, Failed, DeviceReady };

// The controller serializes access. Nested hub enumeration and concurrent PnP
// probes must settle before a failed takeover can return the whole controller.
class State {
 public:
  void enableRecovery(bool enabled) {
    m_Enabled = enabled;
  }
  bool begin() {
    if (m_Closed)
      return false;
    ++m_Active;
    return true;
  }
  bool finish(Outcome outcome) {
    if (!m_Active)
      return false;
    m_Failed |= outcome == Outcome::Failed;
    m_Committed |= outcome == Outcome::DeviceReady;
    --m_Active;
    if (m_Enabled && !m_Closed && !m_Active && m_Failed && !m_Committed) {
      m_Closed = true;
      return true;
    }
    return false;
  }
  void close() {
    m_Closed = true;
  }

 private:
  size_t m_Active = 0;
  bool m_Enabled = false;
  bool m_Failed = false;
  bool m_Committed = false;
  bool m_Closed = false;
};
}  // namespace UsbStartup
#endif
