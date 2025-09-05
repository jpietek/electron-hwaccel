#include <napi.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <arpa/inet.h>

// EGL dma-buf import (EGL_EXT_image_dma_buf_import)
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

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
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Expected (socketPath: string, fd: number)").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string sock_path = info[0].As<Napi::String>().Utf8Value();
  int send_fd = info[1].As<Napi::Number>().Int32Value();

  int s = connect_unix_socket(sock_path);
  if (s < 0) {
    int saved_errno = errno;
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

  ssize_t n = ::sendmsg(s, &msg, 0);
  int saved_errno = errno;
  ::close(s);
  if (n < 0) {
    Napi::Error::New(env, std::string("sendmsg failed: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
    return env.Null();
  }

  return env.Undefined();
}

// sendJsonWithFds(socketPath: string, jsonPayload: string, fds?: number[])
Napi::Value SendJsonWithFds(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2) {
    Napi::TypeError::New(env, "Expected (socketPath: string, jsonPayload: string, fds?: number[])").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "socketPath and jsonPayload must be strings").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string sock_path = info[0].As<Napi::String>().Utf8Value();
  std::string payload   = info[1].As<Napi::String>().Utf8Value();

  std::vector<int> fds;
  if (info.Length() >= 3 && !info[2].IsUndefined() && !info[2].IsNull()) {
    if (!info[2].IsArray()) {
      Napi::TypeError::New(env, "fds must be an array of numbers").ThrowAsJavaScriptException();
      return env.Null();
    }
    Napi::Array arr = info[2].As<Napi::Array>();
    uint32_t len = arr.Length();
    fds.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
      Napi::Value v = arr.Get(i);
      if (!v.IsNumber()) continue;
      fds.push_back(v.As<Napi::Number>().Int32Value());
    }
  }

  int s = connect_unix_socket(sock_path);
  if (s < 0) {
    int saved_errno = errno;
    Napi::Error::New(env, std::string("Failed to connect to UNIX socket: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
    return env.Null();
  }

  // Frame: [u32_be length][JSON bytes]
  const uint32_t json_len = static_cast<uint32_t>(payload.size());
  std::vector<unsigned char> body;
  body.resize(4 + json_len);
  const uint32_t be_len = htonl(json_len);
  std::memcpy(body.data(), &be_len, 4);
  if (json_len > 0) {
    std::memcpy(body.data() + 4, payload.data(), json_len);
  }

  struct iovec iov;
  iov.iov_base = body.data();
  iov.iov_len = body.size();

  struct msghdr msg;
  std::memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  std::vector<char> control;
  if (!fds.empty()) {
    control.resize(CMSG_SPACE(sizeof(int) * fds.size()));
    std::memset(control.data(), 0, control.size());
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
  }

  ssize_t n = ::sendmsg(s, &msg, 0);
  int saved_errno = errno;

  // If partial data was sent on a stream socket, write the remainder
  if (n >= 0 && static_cast<size_t>(n) < body.size()) {
    size_t offset = static_cast<size_t>(n);
    while (offset < body.size()) {
      ssize_t wn = ::send(s, body.data() + offset, body.size() - offset, 0);
      if (wn < 0) { saved_errno = errno; break; }
      offset += static_cast<size_t>(wn);
    }
    if (static_cast<size_t>(n) < body.size() && saved_errno != 0) {
      ::close(s);
      Napi::Error::New(env, std::string("send failed: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  ::close(s);
  if (n < 0) {
    Napi::Error::New(env, std::string("sendmsg failed: ") + std::strerror(saved_errno)).ThrowAsJavaScriptException();
    return env.Null();
  }

  return env.Undefined();
}

// Lazy EGL display init for creating/destroying EGLImages
EGLDisplay get_or_init_egl_display() {
  static EGLDisplay display = EGL_NO_DISPLAY;
  static bool initialized = false;
  if (!initialized) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) return EGL_NO_DISPLAY;
    initialized = true;
  }
  return display;
}

// Resolve extension functions at runtime to avoid undefined symbol errors
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = nullptr;

bool ensure_egl_khr_image_funcs() {
  if (!p_eglCreateImageKHR) {
    p_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  }
  if (!p_eglDestroyImageKHR) {
    p_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  }
  return p_eglCreateImageKHR != nullptr && p_eglDestroyImageKHR != nullptr;
}

// createEGLImageFromDMABuf({ fd, width, height, pitch, offset })
Napi::Value CreateEGLImageFromDMABuf(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected single options object").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object opts = info[0].As<Napi::Object>();
  auto getRequiredInt = [&](const char *name, int &out) -> bool {
    if (!opts.Has(name)) {
      Napi::TypeError::New(env, std::string("Missing required option: ") + name).ThrowAsJavaScriptException();
      return false;
    }
    Napi::Value v = opts.Get(name);
    if (!v.IsNumber()) {
      Napi::TypeError::New(env, std::string("Expected number for option: ") + name).ThrowAsJavaScriptException();
      return false;
    }
    out = v.As<Napi::Number>().Int32Value();
    return true;
  };

  int fd = -1, width = 0, height = 0, fourcc = 0, pitch = 0, offset = 0;
  if (!getRequiredInt("fd", fd)) return env.Null();
  if (!getRequiredInt("width", width)) return env.Null();
  if (!getRequiredInt("height", height)) return env.Null();
  if (!getRequiredInt("pitch", pitch)) return env.Null();
  if (!getRequiredInt("offset", offset)) return env.Null();

  // Hardcode to DRM_FORMAT_ARGB8888 ('AR24' = 0x34325241)
  fourcc = 0x34325241;

  EGLDisplay dpy = get_or_init_egl_display();
  if (dpy == EGL_NO_DISPLAY) {
    Napi::Error::New(env, "Failed to initialize EGL display").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!ensure_egl_khr_image_funcs()) {
    Napi::Error::New(env, "eglCreateImageKHR/eglDestroyImageKHR not available via eglGetProcAddress").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::vector<EGLint> attrs;
  attrs.reserve(32);
  attrs.push_back(EGL_WIDTH); attrs.push_back(width);
  attrs.push_back(EGL_HEIGHT); attrs.push_back(height);
  attrs.push_back(EGL_LINUX_DRM_FOURCC_EXT); attrs.push_back(fourcc);

  attrs.push_back(EGL_DMA_BUF_PLANE0_FD_EXT); attrs.push_back(fd);
  attrs.push_back(EGL_DMA_BUF_PLANE0_OFFSET_EXT); attrs.push_back(offset);
  attrs.push_back(EGL_DMA_BUF_PLANE0_PITCH_EXT); attrs.push_back(pitch);

  // Terminate attribute list
  attrs.push_back(EGL_NONE);

  EGLImageKHR image = p_eglCreateImageKHR(
      dpy,
      EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer) nullptr,
      attrs.data());

  if (image == EGL_NO_IMAGE_KHR) {
    EGLint err = eglGetError();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%04X", err);
    Napi::Error::New(env, std::string("eglCreateImageKHR failed, error=") + buf).ThrowAsJavaScriptException();
    return env.Null();
  }

  // Return the opaque EGLImageKHR handle as a BigInt (uint64)
  uint64_t handle = reinterpret_cast<uint64_t>(image);
  return Napi::BigInt::New(env, handle);
}

Napi::Value DestroyEGLImage(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsBigInt()) {
    Napi::TypeError::New(env, "Expected (imageHandle: bigint)").ThrowAsJavaScriptException();
    return env.Null();
  }
  bool lossless = false;
  uint64_t handle = info[0].As<Napi::BigInt>().Uint64Value(&lossless);
  if (!lossless) {
    Napi::TypeError::New(env, "imageHandle bigint not lossless").ThrowAsJavaScriptException();
    return env.Null();
  }

  EGLDisplay dpy = get_or_init_egl_display();
  if (dpy == EGL_NO_DISPLAY) {
    Napi::Error::New(env, "Failed to initialize EGL display").ThrowAsJavaScriptException();
    return env.Null();
  }

  EGLImageKHR image = reinterpret_cast<EGLImageKHR>(handle);
  if (image != EGL_NO_IMAGE_KHR) {
    if (!ensure_egl_khr_image_funcs()) {
      Napi::Error::New(env, "eglDestroyImageKHR not available").ThrowAsJavaScriptException();
      return env.Null();
    }
    if (p_eglDestroyImageKHR(dpy, image) != EGL_TRUE) {
      Napi::Error::New(env, "eglDestroyImageKHR failed").ThrowAsJavaScriptException();
      return env.Null();
    }
  }
  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("sendFd", Napi::Function::New(env, SendFd));
  exports.Set("sendJsonWithFds", Napi::Function::New(env, SendJsonWithFds));
  exports.Set("createEGLImageFromDMABuf", Napi::Function::New(env, CreateEGLImageFromDMABuf));
  exports.Set("destroyEGLImage", Napi::Function::New(env, DestroyEGLImage));
  return exports;
}

} // namespace

NODE_API_MODULE(fdpass, Init)


