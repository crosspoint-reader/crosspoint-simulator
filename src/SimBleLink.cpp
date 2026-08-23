// SimBleLink -- socket, reader thread and line framing for the fake BLE
// peripheral. The interface is frozen in SimBleLink.h; this file is the
// transport half of it and knows nothing about GATT.
//
// Design notes that are not obvious from the code:
//
// - **Loopback only.** The listener binds 127.0.0.1, never 0.0.0.0. This
//   process runs firmware command handling, so a LAN-reachable port would let
//   anything on the network drive the device model.
// - **One reader thread, always in poll().** The thread never blocks in
//   accept() or recv(): it polls the listener, the client and a self-pipe,
//   and only calls accept/recv on a fd poll already reported readable. That
//   is what makes stop() prompt -- see wakeReader().
// - **The reader owns the client fd's lifetime.** Only the reader closes it.
//   emit() takes the mutex, writes the whole line, and on a write failure
//   shuts the fd down instead of closing it, which makes poll() return and
//   lets the reader do the teardown in one place.

#include "SimBleLink.h"

#include "SimBleProtocol.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace {

namespace ble = crosspoint_simulator::ble;

// Bytes pulled from the socket per recv. Small on purpose: a line is split
// across recv boundaries either way, so the framer has to handle it.
constexpr size_t kRecvChunk = 4096;

// A wedged client must not hang the firmware thread inside emit(). After this
// long the send fails, the link is dropped and the simulator carries on.
constexpr int kSendTimeoutSeconds = 5;

struct LinkState {
  // Guards clientFd, sink and sinkCtx. Held across a whole emit() so two
  // threads cannot interleave halves of two lines.
  std::mutex mtx;
  // Serialises start() and stop() against each other. Separate from mtx
  // because stop() joins the reader while the reader takes mtx.
  std::mutex lifecycle;

  int listenFd = -1;
  int clientFd = -1;
  int wakeRead = -1;
  int wakeWrite = -1;

  std::thread reader;
  std::atomic<bool> up{false};
  std::atomic<bool> stopping{false};

  void (*sink)(void *, const SimBleEvent &) = nullptr;
  void *sinkCtx = nullptr;

  // Reader thread only. No lock.
  std::string rx;
  bool skippingLongLine = false;
};

LinkState &state() {
  static LinkState s;
  return s;
}

void closeFd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// Writes one byte into the self-pipe. This is the only wake that is reliable:
// shutdown() on a listening socket is not portable, and closing a fd another
// thread is polling is a use-after-free waiting to happen.
void wakeReader(LinkState &s) {
  if (s.wakeWrite < 0)
    return;
  const char byte = 1;
  while (::write(s.wakeWrite, &byte, 1) < 0 && errno == EINTR) {
  }
}

void drainWake(LinkState &s) {
  char scratch[64];
  while (::read(s.wakeRead, scratch, sizeof(scratch)) > 0) {
  }
}

// Sends every byte or gives up. Caller holds s.mtx.
bool sendAll(int fd, const char *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

// Best effort single line to a fd the state does not own yet. Used to tell a
// second client it is not welcome.
void sendRefusal(int fd, const std::string &line) {
  const std::string framed = line + "\n";
  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sendAll(fd, framed.data(), framed.size());
}

void emitLine(const std::string &line);

// Drops the client. With `notify` set and a sink installed it also
// synthesizes a disconnect op: a socket that goes away is a link that went
// away, and without this the GATT model would keep believing a central is
// connected. `reason` 0x13 is the same remote-terminated code the disconnect
// op defaults to. The teardown path passes false, because stop() is not a
// link event and the model is being destroyed anyway.
void dropClient(LinkState &s, bool notify) {
  void (*sink)(void *, const SimBleEvent &) = nullptr;
  void *ctx = nullptr;
  bool had = false;
  {
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.clientFd >= 0) {
      ::shutdown(s.clientFd, SHUT_RDWR);
      closeFd(s.clientFd);
      had = true;
    }
    sink = s.sink;
    ctx = s.sinkCtx;
  }
  s.rx.clear();
  s.skippingLongLine = false;
  if (!notify || !had || !sink)
    return;
  SimBleEvent ev;
  ev.op = "disconnect";
  ev.a = 0x13;
  sink(ctx, ev);
}

void handleLine(LinkState &s, const char *line, size_t len) {
  if (len == 0)
    return;  // A blank line is not an op and not an error.

  SimBleEvent ev;
  std::string err;
  if (!ble::parseLine(line, len, ev, err)) {
    emitLine(ble::errorLine(err.empty() ? "malformed line" : err));
    return;
  }

  void (*sink)(void *, const SimBleEvent &) = nullptr;
  void *ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(s.mtx);
    sink = s.sink;
    ctx = s.sinkCtx;
  }
  if (sink)
    sink(ctx, ev);
}

// Line framing. Buffers partial reads, splits on '\n', tolerates "\r\n", and
// caps one line at ble::kMaxLineBytes: past that the buffer is thrown away,
// one error goes out, and everything up to the next newline is discarded.
void feedBytes(LinkState &s, const char *data, size_t len) {
  s.rx.append(data, len);

  size_t start = 0;
  while (true) {
    const size_t nl = s.rx.find('\n', start);
    if (nl == std::string::npos)
      break;
    size_t end = nl;
    if (end > start && s.rx[end - 1] == '\r')
      --end;
    if (s.skippingLongLine) {
      // This "line" is only the tail of the one already dropped.
      s.skippingLongLine = false;
    } else {
      handleLine(s, s.rx.data() + start, end - start);
    }
    start = nl + 1;
  }
  s.rx.erase(0, start);

  if (s.rx.size() > ble::kMaxLineBytes) {
    if (!s.skippingLongLine) {
      emitLine(ble::errorLine("line longer than 65536 bytes, dropped"));
      s.skippingLongLine = true;
    }
    s.rx.clear();
  }
}

void acceptClient(LinkState &s) {
  sockaddr_in peer{};
  socklen_t peerLen = sizeof(peer);
  const int fd =
      ::accept(s.listenFd, reinterpret_cast<sockaddr *>(&peer), &peerLen);
  if (fd < 0)
    return;

  bool busy = false;
  {
    std::lock_guard<std::mutex> lock(s.mtx);
    busy = (s.clientFd >= 0);
  }
  if (busy) {
    sendRefusal(fd, ble::errorLine("busy: the shim takes one client at a time"));
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    return;
  }

  int yes = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
  timeval tv{};
  tv.tv_sec = kSendTimeoutSeconds;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  s.rx.clear();
  s.skippingLongLine = false;
  {
    std::lock_guard<std::mutex> lock(s.mtx);
    s.clientFd = fd;
  }
}

void readerLoop() {
  LinkState &s = state();
  while (!s.stopping.load()) {
    int cfd = -1;
    {
      std::lock_guard<std::mutex> lock(s.mtx);
      cfd = s.clientFd;
    }

    pollfd fds[3];
    int count = 0;
    const int listenSlot = count;
    fds[count++] = {s.listenFd, POLLIN, 0};
    const int wakeSlot = count;
    fds[count++] = {s.wakeRead, POLLIN, 0};
    int clientSlot = -1;
    if (cfd >= 0) {
      clientSlot = count;
      fds[count++] = {cfd, POLLIN, 0};
    }

    const int ready = ::poll(fds, static_cast<nfds_t>(count), -1);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (fds[wakeSlot].revents != 0) {
      drainWake(s);
      if (s.stopping.load())
        break;
    }

    if (clientSlot >= 0 && fds[clientSlot].revents != 0) {
      char buffer[kRecvChunk];
      const ssize_t n = ::recv(cfd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (n > 0) {
        feedBytes(s, buffer, static_cast<size_t>(n));
      } else if (n == 0) {
        dropClient(s, true);
      } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        dropClient(s, true);
      }
    }

    if (fds[listenSlot].revents != 0)
      acceptClient(s);
  }
  dropClient(state(), false);
}

}  // namespace

SimBleLink &SimBleLink::get() {
  static SimBleLink instance;
  return instance;
}

bool SimBleLink::start(uint16_t port) {
  LinkState &s = state();
  std::lock_guard<std::mutex> life(s.lifecycle);
  if (port == 0)
    return false;
  if (s.up.load())
    return true;

  int wake[2] = {-1, -1};
  if (::pipe(wake) != 0)
    return false;
  // The reader drains the pipe in a loop, so its read end must not block.
  ::fcntl(wake[0], F_SETFL, ::fcntl(wake[0], F_GETFL, 0) | O_NONBLOCK);
  ::fcntl(wake[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(wake[1], F_SETFD, FD_CLOEXEC);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    ::close(wake[0]);
    ::close(wake[1]);
    return false;
  }
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1, never a LAN nic.
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 4) != 0) {
    ::close(fd);
    ::close(wake[0]);
    ::close(wake[1]);
    return false;
  }

  s.listenFd = fd;
  s.wakeRead = wake[0];
  s.wakeWrite = wake[1];
  s.rx.clear();
  s.skippingLongLine = false;
  s.stopping.store(false);
  s.up.store(true);
  s.reader = std::thread(readerLoop);
  return true;
}

void SimBleLink::stop() {
  LinkState &s = state();
  std::lock_guard<std::mutex> life(s.lifecycle);
  if (!s.up.load()) {
    // Safe when never started, and safe twice in a row.
    closeFd(s.listenFd);
    closeFd(s.wakeRead);
    closeFd(s.wakeWrite);
    return;
  }

  s.stopping.store(true);
  wakeReader(s);
  {
    // Shuts a connected client down so a reader mid-recv sees EOF at once.
    // The listener is left alone: the reader is in poll(), not accept().
    std::lock_guard<std::mutex> lock(s.mtx);
    if (s.clientFd >= 0)
      ::shutdown(s.clientFd, SHUT_RDWR);
  }
  if (s.reader.joinable())
    s.reader.join();

  {
    std::lock_guard<std::mutex> lock(s.mtx);
    closeFd(s.clientFd);
  }
  closeFd(s.listenFd);
  closeFd(s.wakeRead);
  closeFd(s.wakeWrite);
  s.rx.clear();
  s.skippingLongLine = false;
  s.up.store(false);
  s.stopping.store(false);
}

bool SimBleLink::running() const { return state().up.load(); }

void SimBleLink::setSink(void (*fn)(void *, const SimBleEvent &), void *ctx) {
  LinkState &s = state();
  std::lock_guard<std::mutex> lock(s.mtx);
  s.sink = fn;
  s.sinkCtx = fn ? ctx : nullptr;
}

void SimBleLink::emit(const char *json) {
  if (!json || !*json)
    return;
  emitLine(std::string(json));
}

namespace {

void emitLine(const std::string &line) {
  LinkState &s = state();
  std::string framed;
  framed.reserve(line.size() + 1);
  for (const char ch : line) {
    // An embedded newline would inject a second frame, so it cannot survive.
    framed.push_back((ch == '\n' || ch == '\r') ? ' ' : ch);
  }
  while (!framed.empty() && framed.back() == ' ')
    framed.pop_back();
  if (framed.empty())
    return;
  framed.push_back('\n');

  std::lock_guard<std::mutex> lock(s.mtx);
  if (s.clientFd < 0)
    return;
  if (!sendAll(s.clientFd, framed.data(), framed.size())) {
    // Do not close here: the reader owns the fd's lifetime. A shutdown makes
    // its poll() return and the teardown happens in one place.
    ::shutdown(s.clientFd, SHUT_RDWR);
  }
}

}  // namespace
