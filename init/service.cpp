/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "service.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#include <selinux/selinux.h>

#include <base/file.h>
#include <base/stringprintf.h>
#include <cutils/android_reboot.h>
#include <cutils/sockets.h>

#include "action.h"
#include "keywords.h"
#include "log.h"
#include "property_service.h"
#include "init.h"
#include "init_parser.h"
#include "util.h"

#define CRITICAL_CRASH_THRESHOLD    4       /* if we crash >4 times ... */
#define CRITICAL_CRASH_WINDOW       (4*60)  /* ... in 4 minutes, goto recovery */

SocketInfo::SocketInfo(const std::string& name, const std::string& type, uid_t uid,
                       gid_t gid, int perm, const std::string& socketcon)
    : name(name), type(type), uid(uid), gid(gid), perm(perm), socketcon(socketcon)
{
}

ServiceEnvironmentInfo::ServiceEnvironmentInfo(const std::string& name,
                                               const std::string& value)
    : name(name), value(value)
{
}

Service::Service(const std::string& name, const std::string& classname,
                 const std::vector<std::string>& args)
    : name_(name), classname_(classname), flags_(0), pid_(0), time_started_(0),
      time_crashed_(0), nr_crashed_(0), uid_(0), gid_(0), nr_supp_gids_(0),
      seclabel_(""), writepid_files_(nullptr), keycodes_(nullptr), nkeycodes_(0),
      ioprio_class_(IoSchedClass_NONE), ioprio_pri_(0), args_(args)
{
    onrestart_ = new Action();
    onrestart_->InitSingleTrigger("onrestart");
}

void Service::NotifyStateChange(const std::string& new_state) const {
    if (!properties_initialized()) {
        // If properties aren't available yet, we can't set them.
        return;
    }

    if ((flags_ & SVC_EXEC) != 0) {
        // 'exec' commands don't have properties tracking their state.
        return;
    }

    std::string prop_name = android::base::StringPrintf("init.svc.%s", name_.c_str());
    if (prop_name.length() >= PROP_NAME_MAX) {
        // If the property name would be too long, we can't set it.
        ERROR("Property name \"init.svc.%s\" too long; not setting to %s\n",
              name_.c_str(), new_state.c_str());
        return;
    }

    property_set(prop_name.c_str(), new_state.c_str());
}

void Service::Wait()
{
    if (!(flags_ & SVC_ONESHOT) || (flags_ & SVC_RESTART)) {
        NOTICE("Service '%s' (pid %d) killing any children in process group\n",
               name_.c_str(), pid_);
        kill(-pid_, SIGKILL);
    }

    // Remove any sockets we may have created.
    for (const auto& si : sockets_) {
        std::string tmp = android::base::StringPrintf(ANDROID_SOCKET_DIR "/%s",
                                                      si->name.c_str());
        unlink(tmp.c_str());
    }

    if (flags_ & SVC_EXEC) {
        INFO("SVC_EXEC pid %d finished...\n", pid_);
        waiting_for_exec = false;
        ServiceManager::GetInstance().RemoveService(*this);
        delete this;
        return;
    }

    pid_ = 0;
    flags_ &= (~SVC_RUNNING);

    // Oneshot processes go into the disabled state on exit,
    // except when manually restarted.
    if ((flags_ & SVC_ONESHOT) && !(flags_ & SVC_RESTART)) {
        flags_ |= SVC_DISABLED;
    }

    // Disabled and reset processes do not get restarted automatically.
    if (flags_ & (SVC_DISABLED | SVC_RESET))  {
        NotifyStateChange("stopped");
        return;
    }

    time_t now = gettime();
    if ((flags_ & SVC_CRITICAL) && !(flags_ & SVC_RESTART)) {
        if (time_crashed_ + CRITICAL_CRASH_WINDOW >= now) {
            if (++nr_crashed_ > CRITICAL_CRASH_THRESHOLD) {
                ERROR("critical process '%s' exited %d times in %d minutes; "
                      "rebooting into recovery mode\n", name_.c_str(),
                      CRITICAL_CRASH_THRESHOLD, CRITICAL_CRASH_WINDOW / 60);
                android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
                return;
            }
        } else {
            time_crashed_ = now;
            nr_crashed_ = 1;
        }
    }

    flags_ &= (~SVC_RESTART);
    flags_ |= SVC_RESTARTING;

    // Execute all onrestart commands for this service.
    onrestart_->ExecuteAllCommands();

    NotifyStateChange("restarting");
    return;
}

void Service::DumpState() const
{
    INFO("service %s\n", name_.c_str());
    INFO("  class '%s'\n", classname_.c_str());
    INFO("  exec");
    for (const auto& s : args_) {
        INFO(" '%s'", s.c_str());
    }
    INFO("\n");
    for (const auto& si : sockets_) {
        INFO("  socket %s %s 0%o\n", si->name.c_str(), si->type.c_str(), si->perm);
    }
}

bool Service::HandleLine(int kw, const std::vector<std::string>& args, std::string* err)
{
    std::vector<std::string> str_args;

    ioprio_class_ = IoSchedClass_NONE;

    switch (kw) {
    case K_class:
        if (args.size() != 2) {
            *err = "class option requires a classname\n";
            return false;
        } else {
            classname_ = args[1];
        }
        break;
    case K_console:
        flags_ |= SVC_CONSOLE;
        break;
    case K_disabled:
        flags_ |= SVC_DISABLED;
        flags_ |= SVC_RC_DISABLED;
        break;
    case K_ioprio:
        if (args.size() != 3) {
            *err = "ioprio optin usage: ioprio <rt|be|idle> <ioprio 0-7>\n";
            return false;
        } else {
            ioprio_pri_ = std::stoul(args[2], 0, 8);

            if (ioprio_pri_ < 0 || ioprio_pri_ > 7) {
                *err = "priority value must be range 0 - 7\n";
                return false;
            }

            if (!args[1].compare("rt")) {
                ioprio_class_ = IoSchedClass_RT;
            } else if (!args[1].compare("be")) {
                ioprio_class_ = IoSchedClass_BE;
            } else if (!args[1].compare("idle")) {
                ioprio_class_ = IoSchedClass_IDLE;
            } else {
                *err = "ioprio option usage: ioprio <rt|be|idle> <0-7>\n";
                return false;
            }
        }
        break;
    case K_group:
        if (args.size() < 2) {
            *err = "group option requires a group id\n";
            return false;
        } else if (args.size() > NR_SVC_SUPP_GIDS + 2) {
            *err = android::base::StringPrintf("group option accepts at most %d supp. groups\n",
                                               NR_SVC_SUPP_GIDS);
            return false;
        } else {
            std::size_t n;
            gid_ = decode_uid(args[1].c_str());
            for (n = 2; n < args.size(); n++) {
                supp_gids_[n-2] = decode_uid(args[n].c_str());
            }
            nr_supp_gids_ = n - 2;
        }
        break;
    case K_keycodes: //TODO(tomcherry): C++'ify keycodes
        if (args.size() < 2) {
            *err = "keycodes option requires atleast one keycode\n";
            return false;
        } else {
            keycodes_ = (int*) malloc((args.size() - 1) * sizeof(keycodes_[0]));
            if (!keycodes_) {
                *err = "could not allocate keycodes\n";
                return false;
            } else {
                nkeycodes_ = args.size() - 1;
                for (std::size_t i = 1; i < args.size(); i++) {
                    keycodes_[i - 1] = std::stoi(args[i]);
                }
            }
        }
        break;
    case K_oneshot:
        flags_ |= SVC_ONESHOT;
        break;
    case K_onrestart:
        str_args.assign(args.begin() + 1, args.end());
        add_command_to_action(onrestart_, str_args, "", 0, err);
        break;
    case K_critical:
        flags_ |= SVC_CRITICAL;
        break;
    case K_setenv: { /* name value */
        if (args.size() < 3) {
            *err = "setenv option requires name and value arguments\n";
            return false;
        }
        ServiceEnvironmentInfo* ei = new ServiceEnvironmentInfo(args[1], args[2]);
        if (!ei) {
            *err = "out of memory\n";
            return false;
        }
        envvars_.push_back(ei);
        break;
    }
    case K_socket: {/* name type perm [ uid gid context ] */
        if (args.size() < 4) {
            *err = "socket option requires name, type, perm arguments\n";
            return false;
        }
        if (args[2].compare("dgram") && args[2].compare("stream") &&
            args[2].compare("seqpacket")) {
            *err = "socket type must be 'dgram', 'stream' or 'seqpacket'\n";
            return false;
        }

        int perm = std::stoul(args[3], 0, 8);
        uid_t uid = args.size() > 4 ? decode_uid(args[4].c_str()) : 0;
        gid_t gid = args.size() > 5 ? decode_uid(args[5].c_str()) : 0;
        std::string socketcon = args.size() > 6 ? args[6] : "";

        SocketInfo* si = new SocketInfo(args[1], args[2], uid, gid, perm, socketcon);
        if (!si) {
            *err = "out of memory\n";
            return false;
        }
        sockets_.push_back(si);
        break;
    }
    case K_user:
        if (args.size() != 2) {
            *err = "user option requires a user id\n";
            return false;
        } else {
            uid_ = decode_uid(args[1].c_str());
        }
        break;
    case K_seclabel:
        if (args.size() != 2) {
            *err = "seclabel option requires a label string\n";
            return false;
        } else {
            seclabel_ = args[1];
        }
        break;
    case K_writepid:
        if (args.size() < 2) {
            *err = "writepid option requires at least one filename\n";
            return false;
        }
        writepid_files_ = new std::vector<std::string>;
        writepid_files_->assign(args.begin() + 1, args.end());
        break;

    default:
        *err = android::base::StringPrintf("invalid option '%s'\n", args[0].c_str());
        return false;
    }
    return true;
}

void Service::Start(const std::vector<std::string>& dynamic_args)
{
    // Starting a service removes it from the disabled or reset state and
    // immediately takes it out of the restarting state if it was in there.
    flags_ &= (~(SVC_DISABLED|SVC_RESTARTING|SVC_RESET|SVC_RESTART|SVC_DISABLED_START));
    time_started_ = 0;

    // Running processes require no additional work --- if they're in the
    // process of exiting, we've ensured that they will immediately restart
    // on exit, unless they are ONESHOT.
    if (flags_ & SVC_RUNNING) {
        return;
    }

    bool needs_console = (flags_ & SVC_CONSOLE);
    if (needs_console && !have_console) {
        ERROR("service '%s' requires console\n", name_.c_str());
        flags_ |= SVC_DISABLED;
        return;
    }

    struct stat sb;
    if (stat(args_[0].c_str(), &sb) == -1) {
        ERROR("cannot find '%s' (%s), disabling '%s'\n",
              args_[0].c_str(), strerror(errno), name_.c_str());
        flags_ |= SVC_DISABLED;
        return;
    }

    if ((!(flags_ & SVC_ONESHOT)) && !dynamic_args.empty()) {
        ERROR("service '%s' must be one-shot to use dynamic args, disabling\n",
              args_[0].c_str());
        flags_ |= SVC_DISABLED;
        return;
    }

    std::string scon;
    if (!seclabel_.empty()) {
        scon = seclabel_;
    } else {
        char* mycon = nullptr;
        char* fcon = nullptr;

        INFO("computing context for service '%s'\n", args_[0].c_str());
        int rc = getcon(&mycon);
        if (rc < 0) {
            ERROR("could not get context while starting '%s'\n", name_.c_str());
            return;
        }

        rc = getfilecon(args_[0].c_str(), &fcon);
        if (rc < 0) {
            ERROR("could not get context while starting '%s'\n", name_.c_str());
            free(mycon);
            return;
        }

        char* ret_scon = nullptr;
        rc = security_compute_create(mycon, fcon, string_to_security_class("process"),
                                     &ret_scon);
        if (rc == 0) {
            scon = ret_scon;
            free(ret_scon);
        }
        if (rc == 0 && !scon.compare(mycon)) {
            ERROR("Service %s does not have a SELinux domain defined.\n", name_.c_str());
            free(mycon);
            free(fcon);
            return;
        }
        free(mycon);
        free(fcon);
        if (rc < 0) {
            ERROR("could not get context while starting '%s'\n", name_.c_str());
            return;
        }
    }

    NOTICE("Starting service '%s'...\n", name_.c_str());

    pid_t pid = fork();
    if (pid == 0) {
        int fd, sz;

        umask(077);
        if (properties_initialized()) {
            get_property_workspace(&fd, &sz);
            std::string tmp = android::base::StringPrintf("%d,%d", dup(fd), sz);
            add_environment("ANDROID_PROPERTY_WORKSPACE", tmp.c_str());
        }

        for (const auto& ei : envvars_) {
            add_environment(ei->name.c_str(), ei->value.c_str());
        }

        for (const auto& si : sockets_) {
            int socket_type = ((!si->type.compare("stream") ? SOCK_STREAM :
                                (!si->type.compare("dgram") ? SOCK_DGRAM :
                                 SOCK_SEQPACKET)));
            const char* socketcon =
                !si->socketcon.empty() ? si->socketcon.c_str() : scon.c_str();

            int s = create_socket(si->name.c_str(), socket_type, si->perm,
                                  si->uid, si->gid, socketcon);
            if (s >= 0) {
                PublishSocket(si->name, s);
            }
        }

        if (writepid_files_) {
            std::string pid_str = android::base::StringPrintf("%d", pid);
            for (const auto& file : *writepid_files_) {
                if (!android::base::WriteStringToFile(pid_str, file)) {
                    ERROR("couldn't write %s to %s: %s\n",
                          pid_str.c_str(), file.c_str(), strerror(errno));
                }
            }
        }

        if (ioprio_class_ != IoSchedClass_NONE) {
            if (android_set_ioprio(getpid(), ioprio_class_, ioprio_pri_)) {
                ERROR("Failed to set pid %d ioprio = %d,%d: %s\n",
                      getpid(), ioprio_class_, ioprio_pri_, strerror(errno));
            }
        }

        if (needs_console) {
            setsid();
            OpenConsole();
        } else {
            ZapStdio();
        }

        if (false) {
            for (size_t n = 0; !args_[n].empty(); n++) {
                INFO("args[%zu] = '%s'\n", n, args_[n].c_str());
            }
            for (size_t n = 0; ENV[n]; n++) {
                INFO("env[%zu] = '%s'\n", n, ENV[n]);
            }
        }

        setpgid(0, getpid());

        // As requested, set our gid, supplemental gids, and uid.
        if (gid_) {
            if (setgid(gid_) != 0) {
                ERROR("setgid failed: %s\n", strerror(errno));
                _exit(127);
            }
        }
        if (nr_supp_gids_) {
            if (setgroups(nr_supp_gids_, supp_gids_) != 0) {
                ERROR("setgroups failed: %s\n", strerror(errno));
                _exit(127);
            }
        }
        if (uid_) {
            if (setuid(uid_) != 0) {
                ERROR("setuid failed: %s\n", strerror(errno));
                _exit(127);
            }
        }
        if (!seclabel_.empty()) {
            if (setexeccon(seclabel_.c_str()) < 0) {
                ERROR("cannot setexeccon('%s'): %s\n",
                      seclabel_.c_str(), strerror(errno));
                _exit(127);
            }
        }

        std::vector<char*> strs;
        strs.reserve(args_.size());
        for (const auto& s : args_) {
            strs.push_back(const_cast<char*>(s.c_str()));
        }

        if (dynamic_args.empty()) {
            strs.push_back(nullptr);
            if (execve(args_[0].c_str(), (char**) &strs[0], (char**) ENV) < 0) {
                ERROR("cannot execve('%s'): %s\n", args_[0].c_str(), strerror(errno));
            }
        } else {
            strs.reserve(strs.size() + dynamic_args.size() + 1);
            for (const auto& s : dynamic_args) {
                strs.push_back(const_cast<char*>(s.c_str()));
            }
            strs.push_back(nullptr);
            execve(args_[0].c_str(), (char**) &strs[0], (char**) ENV);
        }
        _exit(127);
    }

    if (pid < 0) {
        ERROR("failed to start '%s'\n", name_.c_str());
        pid_ = 0;
        return;
    }

    time_started_ = gettime();
    pid_ = pid;
    flags_ |= SVC_RUNNING;

    if ((flags_ & SVC_EXEC) != 0) {
        INFO("SVC_EXEC pid %d (uid %d gid %d+%zu context %s) started; waiting...\n",
             pid_, uid_, gid_, nr_supp_gids_,
             !seclabel_.empty() ? seclabel_.c_str() : "default");
        waiting_for_exec = true;
    }

    NotifyStateChange("running");
}

void Service::Start()
{
    const std::vector<std::string> null_dynamic_args;
    Start(null_dynamic_args);
}

void Service::StartIfNotDisabled()
{
    if (!(flags_ & SVC_DISABLED)) {
        Start();
    } else {
        flags_ |= SVC_DISABLED_START;
    }
}

void Service::Enable()
{
    flags_ &= ~(SVC_DISABLED | SVC_RC_DISABLED);
    if (flags_ & SVC_DISABLED_START) {
        Start();
    }
}

void Service::Reset()
{
    StopOrReset(SVC_RESET);
}

void Service::Stop()
{
    StopOrReset(SVC_DISABLED);
}

void Service::Restart()
{
    if (flags_ & SVC_RUNNING) {
        /* Stop, wait, then start the service. */
        StopOrReset(SVC_RESTART);
    } else if (!(flags_ & SVC_RESTARTING)) {
        /* Just start the service since it's not running. */
        Start();
    } /* else: Service is restarting anyways. */
}

void Service::RestartIfNeeded(time_t process_needs_restart)
{
    time_t next_start_time = time_started_ + 5;

    if (next_start_time <= gettime()) {
        flags_ &= (~SVC_RESTARTING);
        Start();
        return;
    }

    if ((next_start_time < process_needs_restart) ||
        (process_needs_restart == 0)) {
        process_needs_restart = next_start_time;
    }
}

/* The how field should be either SVC_DISABLED, SVC_RESET, or SVC_RESTART */
void Service::StopOrReset(int how)
{
    /* The service is still SVC_RUNNING until its process exits, but if it has
     * already exited it shoudn't attempt a restart yet. */
    flags_ &= ~(SVC_RESTARTING | SVC_DISABLED_START);

    if ((how != SVC_DISABLED) && (how != SVC_RESET) && (how != SVC_RESTART)) {
        /* Hrm, an illegal flag.  Default to SVC_DISABLED */
        how = SVC_DISABLED;
    }
        /* if the service has not yet started, prevent
         * it from auto-starting with its class
         */
    if (how == SVC_RESET) {
        flags_ |= (flags_ & SVC_RC_DISABLED) ? SVC_DISABLED : SVC_RESET;
    } else {
        flags_ |= how;
    }

    if (pid_) {
        NOTICE("Service '%s' is being killed...\n", name_.c_str());
        kill(-pid_, SIGKILL);
        NotifyStateChange("stopping");
    } else {
        NotifyStateChange("stopped");
    }
}

void Service::ZapStdio() const
{
    int fd;
    fd = open("/dev/null", O_RDWR);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

void Service::OpenConsole() const
{
    int fd;
    if ((fd = open(console_name.c_str(), O_RDWR)) < 0) {
        fd = open("/dev/null", O_RDWR);
    }
    ioctl(fd, TIOCSCTTY, 0);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

void Service::PublishSocket(const std::string& name, int fd) const
{
    std::string key = android::base::StringPrintf(ANDROID_SOCKET_ENV_PREFIX "%s",
                                                  name.c_str());
    std::string val = android::base::StringPrintf("%d", fd);
    add_environment(key.c_str(), val.c_str());

    /* make sure we don't close-on-exec */
    fcntl(fd, F_SETFD, 0);
}

int ServiceManager::exec_count = 0;

ServiceManager::ServiceManager()
{
}

ServiceManager& ServiceManager::GetInstance() {
    static ServiceManager instance;
    return instance;
}

Service* ServiceManager::AddNewService(const std::string& name,
                                       const std::string& classname,
                                       const std::vector<std::string>& args,
                                       std::string* err)
{
    if (!ValidName(name)) {
        *err = android::base::StringPrintf("invalid service name '%s'\n", name.c_str());
        return nullptr;
    }

    Service* svc = ServiceManager::GetInstance().ServiceFindByName(name);
    if (svc) {
        *err = android::base::StringPrintf("ignored duplicate definition of service '%s'\n",
                                           name.c_str());
        return nullptr;
    }

    svc = new Service(name, classname, args);
    if (svc)
        service_list_.push_back(svc);
    return svc;
}

Service* ServiceManager::MakeExecOneshotService(const std::vector<std::string>& args)
{
    // Parse the arguments: exec [SECLABEL [UID [GID]*] --] COMMAND ARGS...
    // SECLABEL can be a - to denote default
    int command_arg = 1;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (!args[i].compare("--")) {
            command_arg = i + 1;
            break;
        }
    }
    if (command_arg > 4 + NR_SVC_SUPP_GIDS) {
        ERROR("exec called with too many supplementary group ids\n");
        return nullptr;
    }

    std::vector<std::string> str_args(args.begin() + command_arg, args.end());
    if (str_args.size() < 1) {
        ERROR("exec called without command\n");
        return nullptr;
    }

    std::string ret_err;
    std::string name = android::base::StringPrintf("exec %d (%s)", exec_count++,
                                                   str_args[0].c_str());
    Service* svc = new Service(name, "default", str_args);
    if (!svc) {
        ERROR("Couldn't allocate service for exec of '%s': %s",
              str_args[0].c_str(), ret_err.c_str());
        return nullptr;
    }

    svc->flags_ = SVC_EXEC | SVC_ONESHOT;

    if (command_arg > 2 && args[1].compare("-")) {
        svc->seclabel_ = args[1];
    }
    if (command_arg > 3) {
        svc->uid_ = decode_uid(args[2].c_str());
    }
    if (command_arg > 4) {
        svc->gid_ = decode_uid(args[3].c_str());
        svc->nr_supp_gids_ = command_arg - 1 /* -- */ - 4 /* exec SECLABEL UID GID */;
        for (size_t i = 0; i < svc->nr_supp_gids_; ++i) {
            svc->supp_gids_[i] = decode_uid(args[4 + i].c_str());
        }
    }

    service_list_.push_back(svc);

    return svc;
}

Service* ServiceManager::ServiceFindByName(const std::string& name) const
{
    auto svc = std::find_if(service_list_.begin(), service_list_.end(),
                            [&name] (Service* s) {
                                return !name.compare(s->name_);
                            });
    if (svc != service_list_.end()) {
        return *svc;
    }
    return nullptr;
}

Service* ServiceManager::ServiceFindByPid(pid_t pid) const
{
    auto svc = std::find_if(service_list_.begin(), service_list_.end(),
                            [&pid] (Service* s) { return s->pid_ == pid; });
    if (svc != service_list_.end()) {
        return *svc;
    }
    return nullptr;
}

Service* ServiceManager::ServiceFindByKeychord(int keychord_id) const
{
    auto svc = std::find_if(service_list_.begin(), service_list_.end(),
                            [&keychord_id] (Service* s) {
                                return s->keychord_id_ == keychord_id;
                            });

    if (svc != service_list_.end()) {
        return *svc;
    }
    return nullptr;
}

void ServiceManager::ServiceForEach(void (*func)(Service* svc)) const
{
    for (auto& s : service_list_) {
        func(s);
    }
}

void ServiceManager::ServiceForEachClass(const std::string& classname,
                                         void (*func)(Service* svc)) const
{
    for (const auto& s : service_list_) {
        if (!classname.compare(s->classname_)) {
            func(s);
        }
    }
}

void ServiceManager::ServiceForEachFlags(unsigned matchflags,
                                         void (*func)(Service* svc)) const
{
    for (const auto& s : service_list_) {
        if (s->flags_ & matchflags) {
            func(s);
        }
    }
}

void ServiceManager::RemoveService(const Service& svc)
{
    auto svc_it = std::find_if(service_list_.begin(), service_list_.end(),
                               [&svc] (Service* s) {
                                   return !svc.name_.compare(s->name_);
                               });
    if (svc_it == service_list_.end()) {
        return;
    }

    service_list_.erase(svc_it);
}

bool ServiceManager::ValidName(const std::string& name) const
{
    if (name.size() > 16) {
        return false;
    }
    for (const auto& c : name) {
        if (!isalnum(c) && (c != '_') && (c != '-')) {
            return false;
        }
    }
    return true;
}

void ServiceManager::DumpState() const
{
    for (const auto& s : service_list_) {
        s->DumpState();
    }
    INFO("\n");
}
