// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/globals.h"
#if defined(DART_HOST_OS_OHOS)

#include "platform/syslog.h"

#include <hilog/log.h>    // NOLINT
#include <stdio.h>        // NOLINT

namespace dart {

void Syslog::VPrint(const char* format, va_list args) {

  va_list stdio_args;
  va_copy(stdio_args, args);
  vprintf(format, stdio_args);
  fflush(stdout);
  va_end(stdio_args);

  va_list log_args;
  va_copy(log_args, args);
  OH_LOG_Print(LOG_APP, LOG_INFO, 0, "Dart", format, log_args);
  va_end(log_args);
}

void Syslog::VPrintErr(const char* format, va_list args) {
  va_list stdio_args;
  va_copy(stdio_args, args);
  vprintf(format, stdio_args);
  fflush(stdout);
  va_end(stdio_args);
  va_list log_args;
  va_copy(log_args, args);
  OH_LOG_Print(LOG_APP, LOG_ERROR, 0, "Dart", format, log_args);
  va_end(log_args);
}

}  // namespace dart
#endif  // defined(DART_HOST_OS_OHOS)
