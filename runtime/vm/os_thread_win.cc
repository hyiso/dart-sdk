// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/globals.h"  // NOLINT
#if defined(DART_HOST_OS_WINDOWS) && !defined(DART_USE_ABSL)

#include "vm/growable_array.h"
#include "vm/lockers.h"
#include "vm/os_thread.h"

#include <process.h>  // NOLINT

#include "platform/address_sanitizer.h"
#include "platform/assert.h"
#include "platform/safe_stack.h"

#include "vm/flags.h"

namespace dart {

DEFINE_FLAG(int,
            worker_thread_priority,
            kMinInt,
            "The thread priority the VM should use for new worker threads.");

// This flag is flipped by platform_win.cc when the process is exiting.
// TODO(zra): Remove once VM shuts down cleanly.
bool private_flag_windows_run_tls_destructors = true;

class ThreadStartData {
 public:
  ThreadStartData(const char* name,
                  OSThread::ThreadStartFunction function,
                  uword parameter)
      : name_(name), function_(function), parameter_(parameter) {}

  const char* name() const { return name_; }
  OSThread::ThreadStartFunction function() const { return function_; }
  uword parameter() const { return parameter_; }

 private:
  const char* name_;
  OSThread::ThreadStartFunction function_;
  uword parameter_;

  DISALLOW_COPY_AND_ASSIGN(ThreadStartData);
};

// Dispatch to the thread start function provided by the caller. This trampoline
// is used to ensure that the thread is properly destroyed if the thread just
// exits.
static unsigned int __stdcall ThreadEntry(void* data_ptr) {
  if (FLAG_worker_thread_priority != kMinInt) {
    if (SetThreadPriority(GetCurrentThread(), FLAG_worker_thread_priority) ==
        0) {
      FATAL("Setting thread priority to %d failed: GetLastError() = %d\n",
            FLAG_worker_thread_priority, GetLastError());
    }
  }

  ThreadStartData* data = reinterpret_cast<ThreadStartData*>(data_ptr);

  const char* name = data->name();
  OSThread::ThreadStartFunction function = data->function();
  uword parameter = data->parameter();
  delete data;

  // Create new OSThread object and set as TLS for new thread.
  OSThread* thread = OSThread::CreateOSThread();
  if (thread != nullptr) {
    OSThread::SetCurrent(thread);
    thread->SetName(name);

    // Call the supplied thread start function handing it its parameters.
    function(parameter);
  }

  return 0;
}

int OSThread::Start(const char* name,
                    ThreadStartFunction function,
                    uword parameter) {
  ThreadStartData* start_data = new ThreadStartData(name, function, parameter);
  uint32_t tid;
  uintptr_t thread = _beginthreadex(nullptr, OSThread::GetMaxStackSize(),
                                    ThreadEntry, start_data, 0, &tid);
  if (thread == -1L || thread == 0) {
#ifdef DEBUG
    fprintf(stderr, "_beginthreadex error: %d (%s)\n", errno, strerror(errno));
#endif
    return errno;
  }

  // Close the handle, so we don't leak the thread object.
  CloseHandle(reinterpret_cast<HANDLE>(thread));

  return 0;
}

const ThreadJoinId OSThread::kInvalidThreadJoinId = nullptr;

ThreadLocalKey OSThread::CreateThreadLocal(ThreadDestructor destructor) {
  ThreadLocalKey key = TlsAlloc();
  if (key == kUnsetThreadLocalKey) {
    FATAL("TlsAlloc failed %d", GetLastError());
  }
  ThreadLocalData::AddThreadLocal(key, destructor);
  return key;
}

void OSThread::DeleteThreadLocal(ThreadLocalKey key) {
  ASSERT(key != kUnsetThreadLocalKey);
  BOOL result = TlsFree(key);
  if (!result) {
    FATAL("TlsFree failed %d", GetLastError());
  }
  ThreadLocalData::RemoveThreadLocal(key);
}

intptr_t OSThread::GetMaxStackSize() {
  const int kStackSize = (128 * kWordSize * KB);
  return kStackSize;
}

#ifdef SUPPORT_TIMELINE
ThreadId OSThread::GetCurrentThreadTraceId() {
  return ::GetCurrentThreadId();
}
#endif  // SUPPORT_TIMELINE

char* OSThread::GetCurrentThreadName() {
  // TODO(derekx): We aren't even setting the thread name on Windows, so we need
  // to figure out how to set/get the thread name on Windows.
  return nullptr;
}

ThreadJoinId OSThread::GetCurrentThreadJoinId(OSThread* thread) {
  ASSERT(thread != nullptr);
  // Make sure we're filling in the join id for the current thread.
  ThreadId id = GetCurrentThreadId();
  ASSERT(thread->id() == id);
  // Make sure the join_id_ hasn't been set, yet.
  DEBUG_ASSERT(thread->join_id_ == kInvalidThreadJoinId);
  HANDLE handle = OpenThread(SYNCHRONIZE, false, id);
  ASSERT(handle != nullptr);
#if defined(DEBUG)
  thread->join_id_ = handle;
#endif
  return handle;
}

void OSThread::Join(ThreadJoinId id) {
  HANDLE handle = static_cast<HANDLE>(id);
  ASSERT(handle != nullptr);
  DWORD res = WaitForSingleObject(handle, INFINITE);
  CloseHandle(handle);
  ASSERT(res == WAIT_OBJECT_0);
}

void OSThread::Detach(ThreadJoinId id) {
  HANDLE handle = static_cast<HANDLE>(id);
  ASSERT(handle != nullptr);
  CloseHandle(handle);
}

intptr_t OSThread::ThreadIdToIntPtr(ThreadId id) {
  COMPILE_ASSERT(sizeof(id) <= sizeof(intptr_t));
  return static_cast<intptr_t>(id);
}

ThreadId OSThread::ThreadIdFromIntPtr(intptr_t id) {
  return static_cast<ThreadId>(id);
}

bool OSThread::GetCurrentStackBounds(uword* lower, uword* upper) {
  // On Windows stack limits for the current thread are available in
  // the thread information block (TIB).
  NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  *upper = reinterpret_cast<uword>(tib->StackBase);
  // Notice that we cannot use the TIB's StackLimit for the stack end, as it
  // tracks the end of the committed range. We're after the end of the reserved
  // stack area (most of which will be uncommitted, most times).
  MEMORY_BASIC_INFORMATION stack_info;
  memset(&stack_info, 0, sizeof(MEMORY_BASIC_INFORMATION));
  size_t result_size =
      VirtualQuery(&stack_info, &stack_info, sizeof(MEMORY_BASIC_INFORMATION));
  ASSERT(result_size >= sizeof(MEMORY_BASIC_INFORMATION));
  *lower = reinterpret_cast<uword>(stack_info.AllocationBase);
  ASSERT(*upper > *lower);
  // When the third last page of the reserved stack is accessed as a
  // guard page, the second last page will be committed (along with removing
  // the guard bit on the third last) _and_ a stack overflow exception
  // is raised.
  //
  // http://blogs.msdn.com/b/satyem/archive/2012/08/13/thread-s-stack-memory-management.aspx
  // explains the details.
  ASSERT((*upper - *lower) >= (4u * 0x1000));
  *lower += 4 * 0x1000;
  return true;
}

#if defined(USING_SAFE_STACK)
NO_SANITIZE_ADDRESS
NO_SANITIZE_SAFE_STACK
uword OSThread::GetCurrentSafestackPointer() {
#error "SAFE_STACK is unsupported on this platform"
  return 0;
}

NO_SANITIZE_ADDRESS
NO_SANITIZE_SAFE_STACK
void OSThread::SetCurrentSafestackPointer(uword ssp) {
#error "SAFE_STACK is unsupported on this platform"
}
#endif

void OSThread::SetThreadLocal(ThreadLocalKey key, uword value) {
  ASSERT(key != kUnsetThreadLocalKey);
  BOOL result = TlsSetValue(key, reinterpret_cast<void*>(value));
  if (!result) {
    FATAL("TlsSetValue failed %d", GetLastError());
  }
}

void ThreadLocalData::AddThreadLocal(ThreadLocalKey key,
                                     ThreadDestructor destructor) {
  ASSERT(thread_locals_ != nullptr);
  if (destructor == nullptr) {
    // We only care about thread locals with destructors.
    return;
  }
  MutexLocker ml(mutex_);
#if defined(DEBUG)
  // Verify that we aren't added twice.
  for (intptr_t i = 0; i < thread_locals_->length(); i++) {
    const ThreadLocalEntry& entry = thread_locals_->At(i);
    ASSERT(entry.key() != key);
  }
#endif
  // Add to list.
  thread_locals_->Add(ThreadLocalEntry(key, destructor));
}

void ThreadLocalData::RemoveThreadLocal(ThreadLocalKey key) {
  ASSERT(thread_locals_ != nullptr);
  MutexLocker ml(mutex_);
  intptr_t i = 0;
  for (; i < thread_locals_->length(); i++) {
    const ThreadLocalEntry& entry = thread_locals_->At(i);
    if (entry.key() == key) {
      break;
    }
  }
  if (i == thread_locals_->length()) {
    // Not found.
    return;
  }
  thread_locals_->RemoveAt(i);
}

// This function is executed on the thread that is exiting. It is invoked
// by |OnDartThreadExit| (see below for notes on TLS destructors on Windows).
void ThreadLocalData::RunDestructors() {
  // If an OS thread is created but ThreadLocalData::Init has not yet been
  // called, this method still runs. If this happens, there's nothing to clean
  // up here. See issue 33826.
  if (thread_locals_ == nullptr) {
    return;
  }
  ASSERT(mutex_ != nullptr);
  MutexLocker ml(mutex_);
  for (intptr_t i = 0; i < thread_locals_->length(); i++) {
    const ThreadLocalEntry& entry = thread_locals_->At(i);
    // We access the exiting thread's TLS variable here.
    void* p = reinterpret_cast<void*>(OSThread::GetThreadLocal(entry.key()));
    // We invoke the constructor here.
    entry.destructor()(p);
  }
}

Mutex* ThreadLocalData::mutex_ = nullptr;
MallocGrowableArray<ThreadLocalEntry>* ThreadLocalData::thread_locals_ =
    nullptr;

void ThreadLocalData::Init() {
  mutex_ = new Mutex();
  thread_locals_ = new MallocGrowableArray<ThreadLocalEntry>();
}

void ThreadLocalData::Cleanup() {
  if (mutex_ != nullptr) {
    delete mutex_;
    mutex_ = nullptr;
  }
  if (thread_locals_ != nullptr) {
    delete thread_locals_;
    thread_locals_ = nullptr;
  }
}

}  // namespace dart

// The following was adapted from Chromium:
// src/base/threading/thread_local_storage_win.cc

// Thread Termination Callbacks.
// Windows doesn't support a per-thread destructor with its
// TLS primitives.  So, we build it manually by inserting a
// function to be called on each thread's exit.
// This magic is from http://www.codeproject.com/threads/tls.asp
// and it works for VC++ 7.0 and later.

// Force a reference to _tls_used to make the linker create the TLS directory
// if it's not already there.  (e.g. if __declspec(thread) is not used).
// Force a reference to p_thread_callback_dart to prevent whole program
// optimization from discarding the variable.
#ifdef _WIN64

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:p_thread_callback_dart")

#else  // _WIN64

#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_p_thread_callback_dart")

#endif  // _WIN64

// Static callback function to call with each thread termination.
void NTAPI OnDartThreadExit(PVOID module, DWORD reason, PVOID reserved) {
  if (!dart::private_flag_windows_run_tls_destructors) {
    return;
  }
  // On XP SP0 & SP1, the DLL_PROCESS_ATTACH is never seen. It is sent on SP2+
  // and on W2K and W2K3. So don't assume it is sent.
  if (DLL_THREAD_DETACH == reason || DLL_PROCESS_DETACH == reason) {
    dart::ThreadLocalData::RunDestructors();
  }
}

// .CRT$XLA to .CRT$XLZ is an array of PIMAGE_TLS_CALLBACK pointers that are
// called automatically by the OS loader code (not the CRT) when the module is
// loaded and on thread creation. They are NOT called if the module has been
// loaded by a LoadLibrary() call. It must have implicitly been loaded at
// process startup.
// By implicitly loaded, I mean that it is directly referenced by the main EXE
// or by one of its dependent DLLs. Delay-loaded DLL doesn't count as being
// implicitly loaded.
//
// See VC\crt\src\tlssup.c for reference.

// extern "C" suppresses C++ name mangling so we know the symbol name for the
// linker /INCLUDE:symbol pragma above.
extern "C" {
// The linker must not discard p_thread_callback_dart.  (We force a reference
// to this variable with a linker /INCLUDE:symbol pragma to ensure that.) If
// this variable is discarded, the OnDartThreadExit function will never be
// called.
#ifdef _WIN64

// .CRT section is merged with .rdata on x64 so it must be constant data.
#pragma const_seg(".CRT$XLB")
// When defining a const variable, it must have external linkage to be sure the
// linker doesn't discard it.
extern const PIMAGE_TLS_CALLBACK p_thread_callback_dart;
const PIMAGE_TLS_CALLBACK p_thread_callback_dart = OnDartThreadExit;

// Reset the default section.
#pragma const_seg()

#else  // _WIN64

#pragma data_seg(".CRT$XLB")
PIMAGE_TLS_CALLBACK p_thread_callback_dart = OnDartThreadExit;

// Reset the default section.
#pragma data_seg()

#endif  // _WIN64
}  // extern "C"

#endif  // defined(DART_HOST_OS_WINDOWS) && !defined(DART_USE_ABSL)
