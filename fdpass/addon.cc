#include <napi.h>
#include <string>
#include <vector>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

int connect_unix_socket(const std::string &path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

static int g_sock = -1;
static std::string g_sock_path;

bool ensure_connected(const std::string &path, int &saved_errno) {
  if (g_sock >= 0 && path == g_sock_path) return true;
  if (g_sock >= 0) { ::close(g_sock); g_sock = -1; }
  g_sock = connect_unix_socket(path);
  saved_errno = errno;
  if (g_sock >= 0) g_sock_path = path;
  return g_sock >= 0;
}

Napi::Value SendFd(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Expected (socketPath: string, fd: number)").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string sock_path = info[0].As<Napi::String>().Utf8Value();
  int send_fd = info[1].As<Napi::Number>().Int32Value();

  int saved_errno = 0;
  if (!ensure_connected(sock_path, saved_errno)) {
    Napi::Error::New(env, std::string("Failed to connect to UNIX socket: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
    return env.Null();
  }

  // Compose a minimal payload; some UNIXes require at least 1 byte with SCM_RIGHTS
  char dummy = 0;

  // Allocate control buffer with CMSG_SPACE to satisfy alignment
  char control[CMSG_SPACE(sizeof(int))];

  struct iovec iov;
  iov.iov_base = &dummy;
  iov.iov_len = 1;

  struct msghdr msg;
  std::memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  std::memset(control, 0, sizeof(control));
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  *reinterpret_cast<int *>(CMSG_DATA(cmsg)) = send_fd;

  auto do_send = [&]() -> bool {
    ssize_t n = ::sendmsg(g_sock, &msg, 0);
    if (n < 0) { saved_errno = errno; return false; }
    return true;
  };

  if (!do_send()) {
    // Try one reconnect once on failure
    ::close(g_sock); g_sock = -1;
    if (!ensure_connected(sock_path, saved_errno) || !do_send()) {
      Napi::Error::New(env, std::string("sendmsg failed: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  return env.Undefined();
}

Napi::Value Close(const Napi::CallbackInfo &info) {
  if (g_sock >= 0) { ::close(g_sock); g_sock = -1; g_sock_path.clear(); }
  return info.Env().Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("sendFd", Napi::Function::New(env, SendFd));
  exports.Set("close", Napi::Function::New(env, Close));
  return exports;
}

} // namespace

NODE_API_MODULE(fdpass, Init)


