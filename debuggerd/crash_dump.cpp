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

#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <unistd.h>

#include <limits>
#include <memory>
#include <set>
#include <vector>

#include <selinux/selinux.h>
#include <log/logger.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <cutils/sockets.h>

#include "backtrace.h"
#include "tombstone.h"
#include "utility.h"

#include "debuggerd/handler.h"
#include "debuggerd/protocol.h"
#include "debuggerd/util.h"

using android::base::unique_fd;

static bool get_process_info(pid_t tid, pid_t* out_pid, uid_t* out_uid, uid_t* out_gid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", tid);

  FILE* fp = fopen(path, "r");
  if (!fp) {
    return false;
  }

  int fields = 0;
  char line[1024];
  while (fgets(line, sizeof(line), fp)) {
    size_t len = strlen(line);
    if (len > 6 && !memcmp(line, "Tgid:\t", 6)) {
      *out_pid = atoi(line + 6);
      fields |= 1;
    } else if (len > 5 && !memcmp(line, "Uid:\t", 5)) {
      *out_uid = atoi(line + 5);
      fields |= 2;
    } else if (len > 5 && !memcmp(line, "Gid:\t", 5)) {
      *out_gid = atoi(line + 5);
      fields |= 4;
    }
  }
  fclose(fp);
  return fields == 7;
}

static std::set<pid_t> get_process_tids(pid_t pid, pid_t skip_pid) {
  std::set<pid_t> result;
  char task_path[PATH_MAX];
  if (snprintf(task_path, PATH_MAX, "/proc/%d/task", pid) >= PATH_MAX) {
    LOG(FATAL) << "task path overflow (pid = " << pid << ")";
  }

  DIR* dir = opendir(task_path);
  struct dirent* dent;
  while ((dent = readdir(dir))) {
    if (strcmp(dent->d_name, ".") != 0 && strcmp(dent->d_name, "..") != 0) {
      pid_t tid;
      if (!android::base::ParseInt(dent->d_name, &tid, 1, std::numeric_limits<pid_t>::max())) {
        LOG(FATAL) << "failed to parse task id: " << dent->d_name;
      }
      if (tid != skip_pid) {
        result.insert(tid);
      }
    }
  }

  return result;
}

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

static void check_ppid(pid_t expected) {
  pid_t ppid = getppid();
  if (ppid != expected) {
    PLOG(FATAL) << "parent process died (expected ppid " << expected << ", actually " << ppid << ")";
  }
}

int main(int argc, char** argv) {
  // If we exit because of LOG(FATAL), don't try to dump ourselves.
  // TODO: Connect to tombstoned in this case anyway, to report errors to `debuggerd`.
  signal(SIGABRT, SIG_DFL);

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

  if (!android::base::ParseInt(argv[1], &main_tid, 1, std::numeric_limits<pid_t>::max())) {
    LOG(FATAL) << "invalid main tid: " << argv[1];
  }

  // Reparent ourselves to init, so that the signal handler can waitpid on the
  // original process to avoid leaving a zombie for non-fatal dumps.
  pid_t forkpid = fork();
  if (forkpid == -1) {
    PLOG(FATAL) << "fork failed";
  } else if (forkpid != 0) {
    // TODO: Wait for a while before exiting, so that our child can double
    // check that our parent hasn't exited.
    exit(0);
  }

  int attach_error = 0;
  if (!ptrace_attach_thread(parent, main_tid)) {
    PLOG(FATAL) << "failed to attach to thread " << main_tid << " in process " << parent;
  }

  // TODO: Check our parent's ppid after we ptrace the process.

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
  if (ptrace(PTRACE_CONT, main_tid, 0, 0) != 0){
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
  int resume_signal = (signo == DEBUGGER_SIGNAL) ? 0 : signo;
  std::set<pid_t> siblings;
  if (resume_signal == 0) {
    siblings = get_process_tids(parent, main_tid);
    for (pid_t sibling_tid : siblings) {
      if (!ptrace_attach_thread(parent, sibling_tid)) {
        PLOG(FATAL) << "failed to attach to thread " << main_tid << " in process " << parent;
      }
    }
  }

  // TODO: Check our parent's ppid *again* after attaching all of the threads.
  // ptraced processes can die even when we're attached if they're sent SIGKILL.

  // TODO: Use seccomp to lock ourselves down.

  std::unique_ptr<BacktraceMap> backtrace_map(BacktraceMap::Create(main_tid));
  if (backtrace) {
    dump_backtrace(STDOUT_FILENO, backtrace_map.get(), parent, main_tid, siblings, 0);
  } else {
    engrave_tombstone(STDOUT_FILENO, backtrace_map.get(), getppid(), main_tid, siblings,
                      abort_address, nullptr);
  }

  LOG(INFO) << "finished dump of process " << parent;

  bool wait_for_gdb = android::base::GetBoolProperty("debug.debuggerd.wait_for_gdb", false);
  if (wait_for_gdb) {
    if (resume_signal != 0) {
      // Don't wait_for_gdb when the process didn't actually crash.
      // TODO: Flesh out this message
      LOG(ERROR) << "wait_for_gdb on, attach to " << parent << " and kill -CONT " << parent
                 << " to resume";
      resume_signal = SIGSTOP;
    }
  }

  ptrace(PTRACE_DETACH, main_tid, 0, resume_signal);
  for (pid_t tid : siblings) {
    // Don't send the signal to sibling threads.
    if (ptrace(PTRACE_DETACH, tid, 0, 0) != 0) {
      PLOG(ERROR) << "ptrace detach from " << tid << " failed";
    }
  }

  // TODO: Let activity manager know that the process exploded.

  // Close stdout before we notify tombstoned of completion.
  close(STDOUT_FILENO);
  if (!tombstoned_notify_completion(tombstoned_socket.get())) {
    LOG(ERROR) << "failed to notify tombstoned of completion";
  }

  return 0;
}
