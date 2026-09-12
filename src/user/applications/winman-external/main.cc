#include "compositor.h"
#include "demo-process.h"

#include "libui/decoration.h"

#include <fontconfig/fontconfig.h>
#include <pedigree/log.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char *SocketPath = "/run/pedigree-winman.sock";
constexpr const char *NotepadPath = "/usr/bin/winman-external-notepad";
constexpr const char *BundledFontPath = "/usr/share/fonts/ms-sans-serif-1.ttf";

void registerBundledFont() {
  setenv("PANGOCAIRO_BACKEND", "fontconfig", 1);
  const bool registered =
      FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8 *>(BundledFontPath)) &&
      FcConfigBuildFonts(nullptr);
  if (!registered) {
    pedigree_log(LOG_WARNING, "winman: unable to register bundled font '%s'",
                 BundledFontPath);
  }
}

}  // namespace

int main() {
  registerBundledFont();
  std::vector<pid_t> children;
  const std::string socketPath = SocketPath;

  auto spawnNotepad = [&children, &socketPath] {
    const pid_t child = spawnDemoApp(NotepadPath, socketPath, "Notepad", 40, 40, 440, 300,
                                     win95::face);
    if (child > 0)
      children.push_back(child);
  };

  CompositorOptions options;
  options.socketPath = socketPath;
  options.display.title = "Pedigree Winman";
  options.display.clearColor = win95::desktop;
  options.startClients = spawnNotepad;
  options.launchApp = [&spawnNotepad](std::string_view app) {
    if (app == "notepad") {
      spawnNotepad();
    } else {
      pedigree_log(LOG_WARNING, "winman: no target implementation for '%.*s'",
                   static_cast<int>(app.size()), app.data());
    }
  };
  options.stopClients = [&children] { reapChildren(children); };

  pedigree_log(LOG_INFO, "winman: starting out-of-tree compositor");
  return runCompositor(options);
}
