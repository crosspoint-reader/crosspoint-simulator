// Filesystem preparation for the iOS harness: root the emulated SD card at the
// app's Documents directory and make every sideload target reachable from the
// Files app.
//
// Split out of CrossPointIOSShim.cpp so this logic can be compiled and
// exercised on a desktop host -- it is plain POSIX plus SDL_Log, where the shim
// proper only builds against a real SDL and a real screen. Nothing here may
// grow an SDL dependency beyond logging.

#include "CrossPointHarness.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

bool isDirectory(const char *path) {
  struct stat st{};
  return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// --- Fonts sideload through the visible root -------------------------------
//
// The firmware reads SD-card fonts from TWO roots and merges them: "/.fonts"
// (hidden, preferred on name collisions) and "/fonts" (visible) --
// crosspoint-reader lib/EpdFont/SdCardFontRegistry.h:33-34, both scanned by
// discover(). The Files app refuses to create or show dot-prefixed names, so
// on the phone the visible root is the only one a person can reach. Creating
// "fonts" eagerly (next to "books") therefore gives them a drop target the
// firmware already scans -- no HAL and no firmware change involved.
//
// Families the firmware itself downloaded earlier live under "/.fonts":
// SdCardFontRegistry::defaultWriteRoot() prefers the hidden root when it
// exists, and also when neither root exists, so the first in-app download
// creates it. This helper moves those families up into "fonts/", one rename()
// per family (same-volume move, so nothing is copied), which makes them
// visible and manageable in Files -- and because afterwards only the visible
// root exists, defaultWriteRoot() sends FUTURE downloads to "/fonts" too.
//
// A family whose name already exists at the destination stays in the source
// untouched: for the live hidden root the registry de-dups by name with hidden
// winning, so moving it would change which files the firmware reads, and
// clobbering the visible copy is never worth that. Whatever stays behind keeps
// the source root alive below, which is harmless -- a live "/.fonts" is still
// read in place, a leftover under fs_/ stays dormant like any other fs_ stray,
// and the next launch simply tries again.
void migrateFontFamilies(const char *fromRoot, const char *toRoot) {
  DIR *dir = ::opendir(fromRoot);
  if (!dir) {
    return;
  }

  // Collect names first: renaming entries out of a directory mid-readdir() is
  // unspecified behaviour.
  std::vector<std::string> names;
  while (struct dirent *entry = ::readdir(dir)) {
    // "." / ".." plus the hidden/system junk the firmware's own scan skips
    // (macOS ._* resource forks and friends, SdCardFontRegistry scanRoot).
    if (entry->d_name[0] == '.' || entry->d_name[0] == '_') {
      continue;
    }
    names.emplace_back(entry->d_name);
  }
  ::closedir(dir);

  for (const std::string &name : names) {
    const std::string from = std::string(fromRoot) + "/" + name;
    const std::string to = std::string(toRoot) + "/" + name;
    // Only family directories move; a stray file in the root means nothing to
    // the firmware and is not ours to relocate.
    if (!isDirectory(from.c_str())) {
      continue;
    }
    struct stat st{};
    if (::stat(to.c_str(), &st) == 0) {
      continue; // installed in both roots: hidden wins, leave it
    }
    ::mkdir(toRoot, 0777);
    if (::rename(from.c_str(), to.c_str()) == 0) {
      SDL_Log("[harness] migrated %s -> %s", from.c_str(), to.c_str());
    }
  }

  // Only succeeds once the source root is empty; a family or stray that stayed
  // behind keeps it, and that is fine (see above).
  ::rmdir(fromRoot);
}

} // namespace

void CrossPointHarness_prepareFilesystem() {
  // The app's Documents directory IS the emulated SD card. Info.plist sets
  // UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace, so this folder is
  // "On My iPhone > CrossPoint X3" in the Files app -- the only way to load
  // books on a phone. Rooting the card at Documents itself (not Documents/fs_)
  // puts "books" at the top level of that view, where a person will actually
  // find it. The firmware's hidden state (.crosspoint) starts with a dot, which
  // both HalStorage iteration and the Files app keep out of sight.
  const char *home = std::getenv("HOME");
  if (!home) {
    SDL_Log("[harness] HOME unset; leaving CWD alone");
    return;
  }
  const std::string docs = std::string(home) + "/Documents";
  if (chdir(docs.c_str()) != 0) {
    SDL_Log("[harness] chdir(%s) failed", docs.c_str());
    return;
  }
  SDL_Log("[harness] cwd -> %s", docs.c_str());

  // Root HalStorage at Documents. overwrite=0 keeps any externally supplied
  // root (QA harnesses) authoritative.
  setenv("CROSSPOINT_SIM_SD", docs.c_str(), 0);

  // One-time migration from the pre-Files layout, where the card lived at
  // Documents/fs_. rename() is a same-volume move, so an existing library and
  // reading state carry over instead of silently vanishing after an update.
  // Fonts the old layout held (downloaded into fs_/.fonts, or dropped into
  // fs_/fonts back when fs_ itself was the Files-visible folder) migrate
  // per-family straight into the visible root.
  if (isDirectory("fs_")) {
    if (::rename("fs_/books", "books") == 0) {
      SDL_Log("[harness] migrated fs_/books -> books");
    }
    if (::rename("fs_/.crosspoint", ".crosspoint") == 0) {
      SDL_Log("[harness] migrated fs_/.crosspoint -> .crosspoint");
    }
    migrateFontFamilies("fs_/.fonts", "fonts");
    migrateFontFamilies("fs_/fonts", "fonts");
    // Only succeeds once fs_ is empty; harmless if a stray file remains.
    ::rmdir("fs_");
  }

  // Fonts the firmware downloaded into the hidden root on THIS layout become
  // visible too; see migrateFontFamilies for why and for the collision rule.
  migrateFontFamilies(".fonts", "fonts");

  // Create the sideload targets eagerly so "books" and "fonts" exist in Files
  // from the very first launch, before the firmware ever touches storage.
  ::mkdir("books", 0777);
  ::mkdir("fonts", 0777);
}
