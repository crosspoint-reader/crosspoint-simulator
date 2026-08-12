#include "HalStorage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

HalStorage HalStorage::instance;
HalStorage::HalStorage() {}

namespace {
std::string configuredStorageRoot() {
  const char *root = std::getenv("CROSSPOINT_SIM_SD");
  if (!root || !*root) {
    root = std::getenv("CROSSPOINT_EMU_SD");
  }
  return (root && *root) ? std::string(root) : std::string("./fs_");
}

bool containsUnsafeSegment(const std::string &path) {
  std::stringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (segment == "..") {
      return true;
    }
  }
  return false;
}

std::string resolveStoragePath(const char *path) {
  std::string logical = path ? std::string(path) : std::string("/");
  if (logical.empty()) {
    logical = "/";
  }
  if (containsUnsafeSegment(logical)) {
    fprintf(stderr, "[SIM] rejected unsafe storage path: %s\n",
            logical.c_str());
    return {};
  }
  while (!logical.empty() && logical.front() == '/') {
    logical.erase(logical.begin());
  }

  std::string root = configuredStorageRoot();
  while (root.size() > 1 && root.back() == '/') {
    root.pop_back();
  }
  if (logical.empty()) {
    return root;
  }
  return root + "/" + logical;
}

bool ensureParentDirectories(const std::string &full) {
  const size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) {
    return true;
  }
  const std::string parent = full.substr(0, slash);
  if (parent.empty()) {
    return true;
  }
  for (size_t i = 1; i < parent.size(); ++i) {
    if (parent[i] == '/') {
      ::mkdir(parent.substr(0, i).c_str(), 0777);
    }
  }
  return ::mkdir(parent.c_str(), 0777) == 0 || errno == EEXIST;
}
} // namespace

bool HalStorage::begin() {
  const std::string root = configuredStorageRoot();
  for (size_t i = 1; i < root.size(); ++i) {
    if (root[i] == '/') {
      ::mkdir(root.substr(0, i).c_str(), 0777);
    }
  }
  return ::mkdir(root.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::ready() const { return true; }

class HalFile::Impl {
public:
  int fd = -1;
  std::string path;
  DIR *dir = nullptr;

  bool open(const char *p, int flags) {
    path = p;
    // The simulator's FsApiConstants.h just includes <fcntl.h> and typedef int
    // oflag_t, so all O_* constants are already native POSIX values — pass them
    // straight through.
    fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
      fprintf(stderr, "[SIM] open failed: %s (flags=0x%x errno=%d %s)\n",
              path.c_str(), flags, errno, strerror(errno));
    }
    return fd >= 0;
  }

  bool openAsDir(const char *p) {
    path = p;
    dir = opendir(p);
    return dir != nullptr;
  }

  bool isDir() const { return dir != nullptr; }
  bool isOpen() const { return fd >= 0 || dir != nullptr; }
};

HalFile::HalFile() : impl(new Impl()) {}
HalFile::~HalFile() {
  if (impl && impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
}
HalFile::HalFile(HalFile &&other) : impl(std::move(other.impl)) {}
HalFile &HalFile::operator=(HalFile &&other) {
  if (this != &other) {
    if (impl && impl->fd >= 0) {
      ::close(impl->fd);
      impl->fd = -1;
    }
    impl = std::move(other.impl);
  }
  return *this;
}

void HalFile::flush() {
  if (impl && impl->fd >= 0)
    fsync(impl->fd);
}
bool HalFile::sync() {
  if (!impl || impl->fd < 0)
    return false;
  return fsync(impl->fd) == 0;
}
size_t HalFile::getName(char *name, size_t len) {
  if (!impl || impl->path.empty())
    return 0;
  size_t slash = impl->path.rfind('/');
  std::string fname =
      (slash == std::string::npos) ? impl->path : impl->path.substr(slash + 1);
  size_t n = std::min(fname.size(), len - 1);
  memcpy(name, fname.c_str(), n);
  name[n] = '\0';
  return n;
}
size_t HalFile::size() {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return end < 0 ? 0 : (size_t)end;
}
size_t HalFile::fileSize() { return size(); }
uint64_t HalFile::fileSize64() { return size(); }
bool HalFile::seek(size_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)pos, SEEK_SET) >= 0;
}
bool HalFile::seek64(uint64_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  if (pos > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    return false;
  return lseek(impl->fd, static_cast<off_t>(pos), SEEK_SET) >= 0;
}
bool HalFile::seekCur(int64_t offset) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_CUR) >= 0;
}
bool HalFile::seekSet(size_t offset) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_SET) >= 0;
}
int HalFile::available() const {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return (int)(end - cur);
}
size_t HalFile::position() const {
  if (!impl || impl->fd < 0)
    return 0;
  off_t pos = lseek(impl->fd, 0, SEEK_CUR);
  return pos < 0 ? 0 : (size_t)pos;
}
int HalFile::read(void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return -1;
  ssize_t n = ::read(impl->fd, buf, count);
  return (int)n;
}
int HalFile::read() {
  if (!impl || impl->fd < 0)
    return -1;
  uint8_t c;
  return (::read(impl->fd, &c, 1) == 1) ? c : -1;
}
size_t HalFile::write(const void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return 0;
  ssize_t n = ::write(impl->fd, buf, count);
  return n < 0 ? 0 : (size_t)n;
}
size_t HalFile::write(const uint8_t *buf, size_t count) {
  return write(static_cast<const void *>(buf), count);
}
size_t HalFile::write(uint8_t b) {
  if (!impl || impl->fd < 0)
    return 0;
  return (::write(impl->fd, &b, 1) == 1) ? 1 : 0;
}
bool HalFile::rename(const char *newPath) {
  if (!impl || impl->path.empty()) {
    return false;
  }
  const std::string resolved = resolveStoragePath(newPath);
  if (resolved.empty()) {
    return false;
  }
  close();
  ensureParentDirectories(resolved);
  return ::rename(impl->path.c_str(), resolved.c_str()) == 0;
}
bool HalFile::isDirectory() const { return impl && impl->isDir(); }
void HalFile::rewindDirectory() {
  if (impl && impl->dir)
    rewinddir(impl->dir);
}
bool HalFile::close() {
  if (!impl)
    return true;
  if (impl->dir) {
    closedir(impl->dir);
    impl->dir = nullptr;
  }
  if (impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
  return true;
}
HalFile HalFile::openNextFile() {
  if (!impl || !impl->dir)
    return HalFile();
  while (true) {
    struct dirent *entry = readdir(impl->dir);
    if (!entry)
      return HalFile();
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue; // skip . and ..

    std::string childFsPath = impl->path;
    if (childFsPath.back() != '/')
      childFsPath += '/';
    childFsPath += entry->d_name;

    HalFile child;
    struct stat st;
    if (stat(childFsPath.c_str(), &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      child.impl->openAsDir(childFsPath.c_str());
    } else {
      child.impl->open(childFsPath.c_str(), O_RDONLY);
    }
    return child;
  }
}
bool HalFile::isOpen() const {
  if (!impl)
    return false;
  return impl->isOpen();
}
HalFile::operator bool() const { return isOpen(); }

HalFile HalStorage::open(const char *path, const oflag_t oflag) {
  std::string full = resolveStoragePath(path);
  HalFile f;
  if (full.empty()) {
    return f;
  }
  if ((oflag & O_CREAT) != 0) {
    ensureParentDirectories(full);
  }
  struct stat st;
  if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    f.impl->openAsDir(full.c_str());
  } else {
    f.impl->open(full.c_str(), oflag);
  }
  return f;
}
bool HalStorage::mkdir(const char *path, const bool /*pFlag*/) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  // Create all intermediate directories (mkdir -p semantics).
  for (size_t i = 1; i < full.size(); ++i) {
    if (full[i] == '/') {
      ::mkdir(full.substr(0, i).c_str(),
              0777); // ignore errors (may already exist)
    }
  }
  return ::mkdir(full.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::exists(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  struct stat buffer;
  return (stat(full.c_str(), &buffer) == 0);
}
bool HalStorage::remove(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return ::remove(full.c_str()) == 0;
}
bool HalStorage::rename(const char *oldPath, const char *newPath) {
  std::string o = resolveStoragePath(oldPath);
  std::string n = resolveStoragePath(newPath);
  if (o.empty() || n.empty()) {
    return false;
  }
  ensureParentDirectories(n);
  return ::rename(o.c_str(), n.c_str()) == 0;
}
static bool removeDirRecursive(const std::string &full) {
  DIR *d = opendir(full.c_str());
  if (!d)
    return ::remove(full.c_str()) == 0; // might be a plain file
  struct dirent *entry;
  while ((entry = readdir(d)) != nullptr) {
    if (entry->d_name[0] == '.')
      continue;
    std::string child = full + "/" + entry->d_name;
    struct stat st;
    if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      removeDirRecursive(child);
    } else {
      ::remove(child.c_str());
    }
  }
  closedir(d);
  return ::rmdir(full.c_str()) == 0;
}

bool HalStorage::rmdir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}
bool HalStorage::removeDir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}

String HalStorage::readFile(const char *path) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return String("");
  size_t s = f.size();
  std::string content(s, '\0');
  f.read((void *)content.data(), s);
  return String(content);
}
bool HalStorage::readFileToStream(const char *path, Print &out,
                                  size_t chunkSize) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return false;
  std::vector<char> buf(chunkSize);
  int n;
  while ((n = f.read(buf.data(), chunkSize)) > 0) {
    out.write(reinterpret_cast<const uint8_t *>(buf.data()), n);
  }
  return true;
}
size_t HalStorage::readFileToBuffer(const char *path, char *buffer,
                                    size_t bufferSize, size_t maxBytes) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return 0;
  size_t toRead = bufferSize - 1;
  if (maxBytes > 0 && maxBytes < toRead)
    toRead = maxBytes;
  int n = f.read(buffer, toRead);
  if (n < 0)
    n = 0;
  buffer[n] = '\0';
  return n;
}
bool HalStorage::writeFile(const char *path, const String &content) {
  HalFile f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f)
    return false;
  f.write(content.c_str(), content.length());
  return true;
}
bool HalStorage::ensureDirectoryExists(const char *path) { return mkdir(path); }

bool HalStorage::openFileForRead(const char *moduleName, const char *path,
                                 HalFile &file) {
  file = open(path, O_RDONLY);
  return file.isOpen();
}
bool HalStorage::openFileForRead(const char *moduleName,
                                 const std::string &path, HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char *moduleName, const String &path,
                                 HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const char *path,
                                  HalFile &file) {
  // O_RDWR, not O_WRONLY, to match SDCardManager::openFileForWrite on device
  // (O_RDWR | O_CREAT | O_TRUNC). Firmware reads back from a write handle while
  // it is still open -- Section::loadPageDuringBuild seeks into the .bin it is
  // building to render an already-written page. On a write-only fd that read
  // fails and the page renders blank.
  file = open(path, O_RDWR | O_CREAT | O_TRUNC);
  return file.isOpen();
}
bool HalStorage::openFileForWrite(const char *moduleName,
                                  const std::string &path, HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const String &path,
                                  HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

std::vector<String> HalStorage::listFiles(const char *path, int maxFiles) {
  std::vector<String> result;
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return result;
  }
  DIR *dir = opendir(full.c_str());
  if (!dir)
    return result;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr && (int)result.size() < maxFiles) {
    if (entry->d_name[0] == '.')
      continue; // skip . and ..
    result.push_back(String(entry->d_name));
  }
  closedir(dir);
  return result;
}
