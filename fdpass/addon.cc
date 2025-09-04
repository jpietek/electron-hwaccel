#include <napi.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

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

Napi::Value SendFd(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3) {
    Napi::TypeError::New(env, "Expected (socketPath: string, fd: number, token: string)").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string sock_path = info[0].As<Napi::String>().Utf8Value();
  int send_fd = info[1].As<Napi::Number>().Int32Value();
  std::string token = info[2].As<Napi::String>().Utf8Value();

  int s = connect_unix_socket(sock_path);
  if (s < 0) {
    Napi::Error::New(env, "Failed to connect to UNIX socket").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Compose a small message containing the token as payload
  std::vector<char> buf(token.begin(), token.end());
  buf.push_back('\n');

  // Allocate control buffer with CMSG_SPACE to satisfy alignment
  char control[CMSG_SPACE(sizeof(int))];

  struct iovec iov;
  iov.iov_base = buf.data();
  iov.iov_len = buf.size();

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

  ssize_t n = ::sendmsg(s, &msg, 0);
  int saved_errno = errno;
  ::close(s);
  if (n < 0) {
    Napi::Error::New(env, std::string("sendmsg failed: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
    return env.Null();
  }

  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("sendFd", Napi::Function::New(env, SendFd));
  return exports;
}

} // namespace

NODE_API_MODULE(fdpass, Init)


