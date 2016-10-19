/*
 * Copyright 2016, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syscall.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <limits>
#include <memory>
#include <set>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/process_info.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>
#include <log/logger.h>
#include <selinux/selinux.h>

#include "backtrace.h"
#include "tombstone.h"
#include "utility.h"

#include "debuggerd/handler.h"
#include "debuggerd/protocol.h"
#include "debuggerd/util.h"

using android::base::unique_fd;

static bool pid_contains_tid(pid_t pid, pid_t tid) {
  char task_path[PATH_MAX];
  if (snprintf(task_path, PATH_MAX, "/proc/%d/task/%d", pid, tid) >= PATH_MAX) {
    LOG(FATAL) << "task path overflow (pid = " << pid << " , tid = " << tid << ")";
  }
  return access(task_path, F_OK) == 0;
}

// Attach to a thread, and verify that it's still a member of the given process
static bool ptrace_attach_thread(pid_t pid, pid_t tid) {
  if (ptrace(PTRACE_ATTACH, tid, 0, 0) != 0) {
    return false;
  }

  // Make sure that the task we attached to is actually part of the pid we're dumping.
  if (!pid_contains_tid(pid, tid)) {
    if (ptrace(PTRACE_DETACH, tid, 0, 0) != 0) {
      LOG(FATAL) << "failed to detach from thread " << tid;
    }
    errno = ECHILD;
    return false;
  }
  return true;
}

static bool activity_manager_notify(int pid, int signal, const std::string& amfd_data) {
  android::base::unique_fd amfd(socket_local_client("/data/system/ndebugsocket", ANDROID_SOCKET_NAMESPACE_FILESYSTEM, SOCK_STREAM));
  if (amfd.get() == -1) {
    PLOG(ERROR) << "unable to connect to activity manager";
    return false;
  }

  struct timeval tv = {
    .tv_sec = 1,
    .tv_usec = 0,
  };
  if (setsockopt(amfd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
    PLOG(ERROR) << "failed to set send timeout on activity manager socket";
    return false;
  }
  tv.tv_sec = 3;  // 3 seconds on handshake read
  if (setsockopt(amfd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
    PLOG(ERROR) << "failed to set receive timeout on activity manager socket";
    return false;
  }

  // Activity Manager protocol: binary 32-bit network-byte-order ints for the
  // pid and signal number, followed by the raw text of the dump, culminating
  // in a zero byte that marks end-of-data.
  uint32_t datum = htonl(pid);
  if (!android::base::WriteFully(amfd, &datum, 4)) {
    ALOGE("AM pid write failed: %s\n", strerror(errno));
    return false;
  }
  datum = htonl(signal);
  if (!android::base::WriteFully(amfd, &datum, 4)) {
    ALOGE("AM signal write failed: %s\n", strerror(errno));
    return false;
  }
  if (!android::base::WriteFully(amfd, amfd_data.c_str(), amfd_data.size())) {
    ALOGE("AM data write failed: %s\n", strerror(errno));
    return false;
  }
  // Send EOD to the Activity Manager, then wait for its ack to avoid racing
  // ahead and killing the target out from under it.
  uint8_t eodMarker = 0;
  if (!android::base::WriteFully(amfd, &eodMarker, 1)) {
    ALOGE("AM eod write failed: %s\n", strerror(errno));
    return false;
  }

  // 3 sec timeout reading the ack; we're fine if the read fails.
  android::base::ReadFully(amfd, &eodMarker, 1);
  return true;
}

static bool tombstoned_connect(pid_t pid, unique_fd* tombstoned_socket, unique_fd* output_fd) {
  unique_fd sockfd(socket_local_client(kTombstonedCrashSocketName,
                                       ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_SEQPACKET));
  if (sockfd == -1) {
    PLOG(ERROR) << "failed to connect to tombstoned";
    return false;
  }

  TombstonedCrashPacket packet = {};
  packet.packet_type = CrashPacketType::kDumpRequest;
  packet.packet.dump_request.pid = pid;
  if (TEMP_FAILURE_RETRY(write(sockfd, &packet, sizeof(packet))) != sizeof(packet)) {
    PLOG(ERROR) << "failed to write DumpRequest packet";
    return false;
  }

  unique_fd tmp_output_fd;
  ssize_t rc = recv_fd(sockfd, &packet, sizeof(packet), &tmp_output_fd);
  if (rc == -1) {
    PLOG(ERROR) << "failed to read response to DumpRequest packet";
    return false;
  } else if (rc != sizeof(packet)) {
    LOG(ERROR) << "read DumpRequest response packet of incorrect length (expected "
               << sizeof(packet) << ", got " << rc << ")";
    return false;
  }

  *tombstoned_socket = std::move(sockfd);
  *output_fd = std::move(tmp_output_fd);
  return true;
}

static bool tombstoned_notify_completion(int tombstoned_socket) {
  TombstonedCrashPacket packet = {};
  packet.packet_type = CrashPacketType::kCompletedDump;
  if (TEMP_FAILURE_RETRY(write(tombstoned_socket, &packet, sizeof(packet))) != sizeof(packet)) {
    return false;
  }
  return true;
}

static void abort_handler(const char* abort_message) {
  // If we abort before we get an output fd, contact tombstoned to let any
  // potential listeners know that we failed.

  // TODO: Actually do this.

  // Don't dump ourselves.
  _exit(1);
}

static void check_parent(int proc_fd, pid_t expected_ppid) {
  android::base::ProcessInfo proc_info;
  if (!android::base::GetProcessInfoFromProcPidFd(proc_fd, &proc_info)) {
    LOG(FATAL) << "failed to fetch process info";
  }

  if (proc_info.ppid != expected_ppid) {
    LOG(FATAL) << "ppid mismatch: expected " << expected_ppid << ", actual " << proc_info.ppid;
  }
}

int main(int argc, char** argv) {
  android::base::InitLogging(argv);
  android::base::SetAborter(abort_handler);

  // Transition to the crash_dump selinux domain.
  // We can't do this dynamically, because zygote uses PR_SET_NO_NEW_PRIVS to
  // prevent transitions via execve.
  if (setcon("u:r:crash_dump:s0") != 0) {
    PLOG(FATAL) << "setcon failed";
  }

  if (argc != 2) {
    return 1;
  }

  pid_t parent = getppid();
  pid_t main_tid;

  if (parent == 1) {
    LOG(FATAL) << "parent died before we could attach";
  }

  if (!android::base::ParseInt(argv[1], &main_tid, 1, std::numeric_limits<pid_t>::max())) {
    LOG(FATAL) << "invalid main tid: " << argv[1];
  }

  android::base::ProcessInfo target_info;
  if (!android::base::GetProcessInfo(main_tid, &target_info)) {
    LOG(FATAL) << "failed to fetch process info for target " << main_tid;
  }

  if (main_tid != target_info.tid || parent != target_info.pid) {
    LOG(FATAL) << "target info mismatch, self = " << parent << "(" << main_tid << ") "
               << ", proc returned " << target_info.pid << "(" << target_info.tid << ")";
  }

  // Open /proc/self in the original process, and pass it down to the forked child.
  int proc_fd = open("/proc/self", O_DIRECTORY | O_RDONLY);
  if (proc_fd == -1) {
    PLOG(FATAL) << "failed to open /proc/self";
  }

  // Reparent ourselves to init, so that the signal handler can waitpid on the
  // original process to avoid leaving a zombie for non-fatal dumps.
  unique_fd forkread, forkwrite;
  if (!Pipe(&forkread, &forkwrite)) {
    PLOG(FATAL) << "failed to create pipe";
  }

  pid_t forkpid = fork();
  if (forkpid == -1) {
    PLOG(FATAL) << "fork failed";
  } else if (forkpid != 0) {
    forkwrite.reset();
    char buf;
    ssize_t rc = TEMP_FAILURE_RETRY(read(forkread.get(), &buf, sizeof(buf)));
    if (rc == -1) {
      PLOG(ERROR) << "read failed when waiting in original process";
    }
    exit(0);
  }

  forkread.reset();
  check_parent(proc_fd, parent);

  int attach_error = 0;
  if (!ptrace_attach_thread(parent, main_tid)) {
    PLOG(FATAL) << "failed to attach to thread " << main_tid << " in process " << parent;
  }

  check_parent(proc_fd, parent);

  LOG(INFO) << "obtaining output fd from tombstoned";
  unique_fd tombstoned_socket;
  unique_fd output_fd;
  bool tombstoned_connected = tombstoned_connect(parent, &tombstoned_socket, &output_fd);

  // Write a '\1' to stdout to tell the crashing process to resume.
  if (TEMP_FAILURE_RETRY(write(STDOUT_FILENO, "\1", 1)) == -1) {
    PLOG(ERROR) << "failed to communicate to target process";
  }

  if (tombstoned_connected) {
    if (TEMP_FAILURE_RETRY(dup2(output_fd.get(), STDOUT_FILENO)) == -1) {
      PLOG(ERROR) << "failed to dup2 output fd (" << output_fd.get() << ") to STDOUT_FILENO";
    }
    output_fd.reset();
  } else {
    unique_fd devnull(TEMP_FAILURE_RETRY(open("/dev/null", O_RDWR)));
    TEMP_FAILURE_RETRY(dup2(devnull.get(), STDOUT_FILENO));
  }

  if (attach_error != 0) {
    PLOG(FATAL) << "failed to attach to thread " << main_tid << " in process " << parent;
  }

  LOG(INFO) << "performing dump of process " << parent << " (target tid = " << main_tid << ")";

  // At this point, the thread that made the request has been PTRACE_ATTACHed
  // and has the signal that triggered things queued. Send PTRACE_CONT, and
  // then wait for the signal.
  if (ptrace(PTRACE_CONT, main_tid, 0, 0) != 0) {
    PLOG(ERROR) << "PTRACE_CONT(" << main_tid << ") failed";
    exit(1);
  }

  siginfo_t siginfo = {};
  if (!wait_for_signal(main_tid, &siginfo)) {
    printf("failed to wait for signal in tid %d: %s\n", main_tid, strerror(errno));
    exit(1);
  }

  int signo = siginfo.si_signo;
  bool backtrace = false;
  uintptr_t abort_address = 0;

  // si_value can represent three things:
  //   0: dump tombstone
  //   1: dump backtrace
  //   everything else: abort address, with implicit tombstone
  if (siginfo.si_value.sival_int == 1) {
    backtrace = true;
  } else if (siginfo.si_value.sival_ptr != nullptr) {
    abort_address = reinterpret_cast<uintptr_t>(siginfo.si_value.sival_ptr);
  }

  // Now that we have the signal that kicked things off, attach all of the
  // sibling threads, and then proceed.
  bool fatal_signal = signo != DEBUGGER_SIGNAL;
  int resume_signal = fatal_signal ? signo : 0;
  std::set<pid_t> siblings;
  if (resume_signal == 0) {
    if (!android::base::GetProcessTids(parent, &siblings)) {
      PLOG(FATAL) << "failed to get process siblings";
    }
    siblings.erase(main_tid);

    for (pid_t sibling_tid : siblings) {
      if (!ptrace_attach_thread(parent, sibling_tid)) {
        PLOG(FATAL) << "failed to attach to thread " << main_tid << " in process " << parent;
      }
    }
  }

  check_parent(proc_fd, parent);

  // Tell our parent to die.
  if (TEMP_FAILURE_RETRY(write(forkwrite.get(), "", 1)) != 1) {
    PLOG(FATAL) << "failed to tell parent to continue";
  }
  #pragma GCC poison check_parent

  // TODO: Use seccomp to lock ourselves down.

  std::unique_ptr<BacktraceMap> backtrace_map(BacktraceMap::Create(main_tid));
  std::string amfd_data;
  if (backtrace) {
    dump_backtrace(STDOUT_FILENO, backtrace_map.get(), parent, main_tid, siblings, 0);
  } else {
    engrave_tombstone(STDOUT_FILENO, backtrace_map.get(), parent, main_tid, siblings,
                      abort_address, fatal_signal ? &amfd_data : nullptr);
  }

  bool wait_for_gdb = android::base::GetBoolProperty("debug.debuggerd.wait_for_gdb", false);
  if (wait_for_gdb) {
    // Don't wait_for_gdb when the process didn't actually crash.
    if (!fatal_signal) {
      wait_for_gdb = false;
    } else {
      // Use ALOGI to line up with output from engrave_tombstone.
      ALOGI(
        "***********************************************************\n"
        "* Process %d has been suspended while crashing.\n"
        "* To attach gdbserver and start gdb, run this on the host:\n"
        "*\n"
        "*     gdbclient.py -p %d\n"
        "*\n"
        "* Wait for gdb to start, then press the VOLUME DOWN key\n"
        "* to let the process continue crashing.\n"
        "***********************************************************",
        parent, main_tid);
    }
  }

  for (pid_t tid : siblings) {
    // Don't send the signal to sibling threads.
    if (ptrace(PTRACE_DETACH, tid, 0, wait_for_gdb ? SIGSTOP : 0) != 0) {
      PLOG(ERROR) << "ptrace detach from " << tid << " failed";
    }
  }

  if (ptrace(PTRACE_DETACH, main_tid, 0, wait_for_gdb ? SIGSTOP : resume_signal)) {
    PLOG(ERROR) << "ptrace detach from main thread " << main_tid << " failed";
  }

  if (wait_for_gdb) {
    if (syscall(__NR_tgkill, parent, main_tid, resume_signal)) {
      PLOG(ERROR) << "failed to resend signal to process " << parent;
    }
  }

  if (fatal_signal) {
    activity_manager_notify(parent, signo, amfd_data);
  }

  // Close stdout before we notify tombstoned of completion.
  close(STDOUT_FILENO);
  if (!tombstoned_notify_completion(tombstoned_socket.get())) {
    LOG(ERROR) << "failed to notify tombstoned of completion";
  }

  return 0;
}
