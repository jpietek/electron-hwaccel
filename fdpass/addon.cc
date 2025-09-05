#include <napi.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

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

// Lazy EGL display init for creating/destroying EGLImages
EGLDisplay get_or_init_egl_display(Napi::Env env) {
  static EGLDisplay display = EGL_NO_DISPLAY;
  static bool initialized = false;
  if (!initialized) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
      Napi::Error::New(env, "eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY").ThrowAsJavaScriptException();
      return EGL_NO_DISPLAY;
    }
    EGLint major = 0, minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
      Napi::Error::New(env, "eglInitialize failed").ThrowAsJavaScriptException();
      return EGL_NO_DISPLAY;
    }
    initialized = true;
  }
  return display;
}

// createEGLImageFromDMABuf({ fd, width, height, fourcc, pitch, offset, [plane1Fd, plane1Pitch, plane1Offset, plane2Fd, plane2Pitch, plane2Offset] })
Napi::Value CreateEGLImageFromDMABuf(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected single options object").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object opts = info[0].As<Napi::Object>();

  auto requireProp = [&](const char *name) -> int {
    if (!opts.Has(name)) {
      Napi::TypeError::New(env, std::string("Missing required option: ") + name).ThrowAsJavaScriptException();
      return 0;
    }
    return opts.Get(name).As<Napi::Number>().Int32Value();
  };

  int fd = requireProp("fd");
  int width = requireProp("width");
  int height = requireProp("height");
  int fourcc = requireProp("fourcc");
  int pitch = requireProp("pitch");
  int offset = requireProp("offset");

  // Optional planes for semi/fully planar formats
  bool has_p1 = opts.Has("plane1Fd");
  bool has_p2 = opts.Has("plane2Fd");
  int p1_fd = has_p1 ? opts.Get("plane1Fd").As<Napi::Number>().Int32Value() : -1;
  int p1_pitch = has_p1 ? opts.Get("plane1Pitch").As<Napi::Number>().Int32Value() : 0;
  int p1_offset = has_p1 ? opts.Get("plane1Offset").As<Napi::Number>().Int32Value() : 0;
  int p2_fd = has_p2 ? opts.Get("plane2Fd").As<Napi::Number>().Int32Value() : -1;
  int p2_pitch = has_p2 ? opts.Get("plane2Pitch").As<Napi::Number>().Int32Value() : 0;
  int p2_offset = has_p2 ? opts.Get("plane2Offset").As<Napi::Number>().Int32Value() : 0;

  EGLDisplay dpy = get_or_init_egl_display(env);
  if (dpy == EGL_NO_DISPLAY) {
    return env.Null();
  }

  std::vector<EGLint> attrs;
  attrs.reserve(32);
  attrs.push_back(EGL_WIDTH); attrs.push_back(width);
  attrs.push_back(EGL_HEIGHT); attrs.push_back(height);
  attrs.push_back(EGL_LINUX_DRM_FOURCC_EXT); attrs.push_back(fourcc);
  // Plane 0
  attrs.push_back(EGL_DMA_BUF_PLANE0_FD_EXT); attrs.push_back(fd);
  attrs.push_back(EGL_DMA_BUF_PLANE0_OFFSET_EXT); attrs.push_back(offset);
  attrs.push_back(EGL_DMA_BUF_PLANE0_PITCH_EXT); attrs.push_back(pitch);
  // Optional YUV planes
  if (has_p1) {
    attrs.push_back(EGL_DMA_BUF_PLANE1_FD_EXT); attrs.push_back(p1_fd);
    attrs.push_back(EGL_DMA_BUF_PLANE1_OFFSET_EXT); attrs.push_back(p1_offset);
    attrs.push_back(EGL_DMA_BUF_PLANE1_PITCH_EXT); attrs.push_back(p1_pitch);
  }
  if (has_p2) {
    attrs.push_back(EGL_DMA_BUF_PLANE2_FD_EXT); attrs.push_back(p2_fd);
    attrs.push_back(EGL_DMA_BUF_PLANE2_OFFSET_EXT); attrs.push_back(p2_offset);
    attrs.push_back(EGL_DMA_BUF_PLANE2_PITCH_EXT); attrs.push_back(p2_pitch);
  }

  // Terminate attribute list
  attrs.push_back(EGL_NONE);

  EGLImageKHR image = eglCreateImageKHR(
      dpy,
      EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer) nullptr,
      attrs.data());

  if (image == EGL_NO_IMAGE_KHR) {
    EGLint err = eglGetError();
    Napi::Error::New(env, std::string("eglCreateImageKHR failed, error=0x") +
                              ([](EGLint e){ char buf[16]; std::snprintf(buf, sizeof(buf), "%04X", e); return std::string(buf);} )(err))
        .ThrowAsJavaScriptException();
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

  EGLDisplay dpy = get_or_init_egl_display(env);
  if (dpy == EGL_NO_DISPLAY) {
    return env.Null();
  }

  EGLImageKHR image = reinterpret_cast<EGLImageKHR>(handle);
  if (image != EGL_NO_IMAGE_KHR) {
    if (eglDestroyImageKHR(dpy, image) != EGL_TRUE) {
      Napi::Error::New(env, "eglDestroyImageKHR failed").ThrowAsJavaScriptException();
      return env.Null();
    }
  }
  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("sendFd", Napi::Function::New(env, SendFd));
  exports.Set("createEGLImageFromDMABuf", Napi::Function::New(env, CreateEGLImageFromDMABuf));
  exports.Set("destroyEGLImage", Napi::Function::New(env, DestroyEGLImage));
  return exports;
}

} // namespace

NODE_API_MODULE(fdpass, Init)


