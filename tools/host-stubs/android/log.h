#pragma once
// Desktop stand-in for the NDK header, used only by tools/panel_preview.cpp.
#include <cstdarg>
#include <cstdio>

enum { ANDROID_LOG_INFO = 4 };

inline int __android_log_print(int, const char *tag, const char *fmt, ...) {
  std::fprintf(stderr, "[%s] ", tag);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  return 0;
}
