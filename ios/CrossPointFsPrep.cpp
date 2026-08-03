// Filesystem preparation for the iOS harness: root the emulated SD card at the
// app's Documents directory and make every sideload target reachable from the
// Files app.
//
// Split out of CrossPointIOSShim.cpp so this logic can be compiled and
// exercised on a desktop host -- it is plain POSIX plus SDL's log and base-path
// helpers, where the shim proper only builds against a real SDL and a real
// screen. Nothing here may grow an SDL dependency beyond logging and
// SDL_GetBasePath (which needs no SDL_Init and works headless).

#include "CrossPointHarness.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <climits>

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

// --- Bundled font seeding ---------------------------------------------------
//
// Families built by CI ship inside the bundle under Resources/SeedFonts/
// (ios/CMakeLists.txt, CROSSPOINT_IOS_SEED_FONTS_DIR) and are copied into the
// visible "fonts/" root here, so a TestFlight update delivers font updates
// without a trip through the Files app.
//
// The bundle owns its families: a bundled file is (re)copied whenever the
// destination differs, so a stale copy from an older build never lingers —
// and a hand-dropped replacement of a BUNDLED family is overwritten on next
// launch. Families the bundle does not carry are never touched. Comparison is
// size-then-bytes: .cpfont files are compiled artifacts where a real change
// virtually always moves the size, but the byte check makes same-size drift
// impossible to miss, and a full compare of a few hundred KB from flash is
// milliseconds.

bool filesIdentical(const char *a, const char *b) {
  struct stat sa{}, sb{};
  if (::stat(a, &sa) != 0 || ::stat(b, &sb) != 0) return false;
  if (sa.st_size != sb.st_size) return false;
  const int fa = ::open(a, O_RDONLY);
  if (fa < 0) return false;
  const int fb = ::open(b, O_RDONLY);
  if (fb < 0) {
    ::close(fa);
    return false;
  }
  bool same = true;
  char bufA[65536], bufB[65536];
  for (;;) {
    const ssize_t nA = ::read(fa, bufA, sizeof(bufA));
    const ssize_t nB = ::read(fb, bufB, sizeof(bufB));
    if (nA != nB || nA < 0) {
      same = false;
      break;
    }
    if (nA == 0) break;
    if (memcmp(bufA, bufB, static_cast<size_t>(nA)) != 0) {
      same = false;
      break;
    }
  }
  ::close(fa);
  ::close(fb);
  return same;
}

bool copyFile(const char *from, const char *to) {
  const int src = ::open(from, O_RDONLY);
  if (src < 0) return false;
  // Write to a temp name and rename into place: a crash mid-copy must not
  // leave a truncated .cpfont where the firmware will happily mmap-read it.
  const std::string tmp = std::string(to) + ".seed-tmp";
  const int dst = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst < 0) {
    ::close(src);
    return false;
  }
  bool ok = true;
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(src, buf, sizeof(buf));
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    if (::write(dst, buf, static_cast<size_t>(n)) != n) {
      ok = false;
      break;
    }
  }
  ::close(src);
  ok = ::close(dst) == 0 && ok;
  if (ok) ok = ::rename(tmp.c_str(), to) == 0;
  if (!ok) ::unlink(tmp.c_str());
  return ok;
}

// Copy every regular file from `from` into `to`, then prune files in `to` that
// the bundle no longer carries. Subdirectories are NOT walked here; the caller
// decides which ones to seed, because a family's `2x/` companion has to land in
// its own destination directory rather than being flattened into the parent.
void seedOneFontDirectory(const std::string &from, const std::string &to) {
  DIR *fam = ::opendir(from.c_str());
  if (!fam) return;
  ::mkdir(to.c_str(), 0777);
  std::vector<std::string> bundled;
  while (struct dirent *entry = ::readdir(fam)) {
    if (entry->d_name[0] == '.') continue;
    const std::string src = from + "/" + entry->d_name;
    const std::string dst = to + "/" + entry->d_name;
    if (isDirectory(src.c_str())) continue;
    bundled.emplace_back(entry->d_name);
    if (filesIdentical(src.c_str(), dst.c_str())) continue;
    if (copyFile(src.c_str(), dst.c_str())) {
      SDL_Log("[harness] seeded %s", dst.c_str());
    } else {
      SDL_Log("[harness] seeding %s FAILED", dst.c_str());
    }
  }
  ::closedir(fam);

  // The bundle owns its families, including their SIZE SET: when a family's
  // ramp changes (e.g. Junicode moving from 12-18pt to the harmonized
  // 14-20pt), files the bundle no longer carries must go, or the family
  // becomes a mixed-ramp hybrid of old and new builds -- size stepping would
  // silently switch design between sizes. Only bundled families are pruned,
  // and only within their own directory.
  DIR *have = ::opendir(to.c_str());
  if (!have) return;
  while (struct dirent *entry = ::readdir(have)) {
    if (entry->d_name[0] == '.') continue;
    bool inBundle = false;
    for (const std::string &name : bundled) {
      if (name == entry->d_name) {
        inBundle = true;
        break;
      }
    }
    if (inBundle) continue;
    const std::string stale = to + "/" + entry->d_name;
    if (isDirectory(stale.c_str())) continue;
    if (::unlink(stale.c_str()) == 0) {
      SDL_Log("[harness] pruned stale %s", stale.c_str());
    }
  }
  ::closedir(have);
}

// Attempt to symlink `to` -> `from` (an absolute bundle path).
//
// Returns true and creates the symlink when:
//   - `to` does not exist yet (new install)
//   - `to` is already a symlink but points somewhere else (bundle moved after
//     an update); the stale link is removed first.
//
// Returns false without touching `to` when it is a REAL directory: that means
// a previous install left a genuine copy (from an older build that did not
// symlink) or the user manually populated the family. In either case we fall
// back to seedOneFontDirectory() so the copy+prune path still runs and keeps
// the files up to date.
//
// Correctness constraints satisfied:
//  - Prune logic never runs through a live symlink (we skip seedOneFontDirectory
//    entirely on success).
//  - The 2x companion is automatically reachable through the symlinked
//    directory; no separate seedOneFontDirectory call is needed.
//  - A user who drops a replacement family folder via Files would write into
//    the read-only bundle directory and get an EROFS error from iOS, which is
//    the correct behaviour for a bundled resource. They can install a DIFFERENT
//    family name alongside; that will be a real directory and is never touched.
bool symlinkFontDirectory(const std::string &from, const std::string &to) {
  struct stat st{};
  if (::lstat(to.c_str(), &st) == 0) {
    if (S_ISLNK(st.st_mode)) {
      // Already a symlink: check if it targets the right bundle path.
      char linkbuf[PATH_MAX];
      const ssize_t len = ::readlink(to.c_str(), linkbuf, sizeof(linkbuf) - 1);
      if (len > 0) {
        linkbuf[len] = '\0';
        if (std::string(linkbuf) == from) {
          return true;  // already correct, nothing to do
        }
      }
      // Stale symlink (bundle UUID changed after update). Remove and recreate.
      ::unlink(to.c_str());
    } else {
      // Real directory: user-populated or old copy. Let caller fall back to
      // the copy+prune path.
      return false;
    }
  }
  if (::symlink(from.c_str(), to.c_str()) == 0) {
    SDL_Log("[harness] symlinked fonts/%s -> bundle SeedFonts",
            to.c_str() + (to.rfind('/') != std::string::npos ? to.rfind('/') + 1 : 0));
    return true;
  }
  SDL_Log("[harness] symlink %s FAILED: %s", to.c_str(), ::strerror(errno));
  return false;
}

void seedBundledFontFamilies() {
  // SDL_GetBasePath: the bundle's Resources directory on iOS, the executable's
  // directory on a desktop host (where SeedFonts/ simply doesn't exist and
  // this whole pass is a no-op). Needs no SDL_Init.
  const char *base = SDL_GetBasePath();
  if (!base) return;
  const std::string seedRoot = std::string(base) + "SeedFonts";
  DIR *dir = ::opendir(seedRoot.c_str());
  if (!dir) return;

  std::vector<std::string> families;
  while (struct dirent *entry = ::readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    families.emplace_back(entry->d_name);
  }
  ::closedir(dir);

  for (const std::string &family : families) {
    const std::string from = seedRoot + "/" + family;
    if (!isDirectory(from.c_str())) continue;
    ::mkdir("fonts", 0777);
    const std::string to = std::string("fonts/") + family;

    // Prefer a symlink: the bundle copy stays in the app and Documents gets a
    // zero-byte directory entry. ~54 MB that used to exist twice now exists
    // once. 2x companion files are reachable through the symlinked directory
    // with no extra work.
    if (symlinkFontDirectory(from, to)) continue;

    // Fall back to copy+prune when `to` is already a real directory (existing
    // install or user-populated family). Keeps updates landing correctly.
    seedOneFontDirectory(from, to);
    // The hi-res companion set lives in `<family>/2x/` and must be seeded into
    // the same shape on the card, or SdCardFontManager finds no companion and
    // the renderer silently falls back to replicating the 1x glyphs -- which
    // looks exactly like today rather than like a failure.
    seedOneFontDirectory(from + "/2x", to + "/2x");
  }
}

// Default books ship under Resources/SeedBooks (ios/CMakeLists.txt) and are
// copied into books/ ONLY when the file is absent. Books are user data: unlike
// the font families above, an existing copy is never overwritten (the user's
// reading position keys off the file) and nothing is ever pruned.
void seedBundledBooks() {
  const char *base = SDL_GetBasePath();
  if (!base) return;
  const std::string seedRoot = std::string(base) + "SeedBooks";
  DIR *dir = ::opendir(seedRoot.c_str());
  if (!dir) return;

  ::mkdir("books", 0777);
  while (struct dirent *entry = ::readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    const std::string src = seedRoot + "/" + entry->d_name;
    if (isDirectory(src.c_str())) continue;
    const std::string dst = std::string("books/") + entry->d_name;
    struct stat st{};
    if (::stat(dst.c_str(), &st) == 0) continue;  // exists: hands off
    if (copyFile(src.c_str(), dst.c_str())) {
      SDL_Log("[harness] seeded book %s", dst.c_str());
    } else {
      SDL_Log("[harness] seeding book %s FAILED", dst.c_str());
    }
  }
  ::closedir(dir);
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

  // After migration, so a bundled family lands in its final home and the
  // bundle-wins comparison sees the migrated copy rather than missing it.
  seedBundledFontFamilies();
  seedBundledBooks();
}
