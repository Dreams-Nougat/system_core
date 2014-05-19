/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "netd_client/NetdClient.h"

#include "FwmarkClient.h"
#include "netd_client/FwmarkCommands.h"
#include "resolv_netid.h"

#include <sys/socket.h>
#include <unistd.h>

namespace {

int closeFdAndRestoreErrno(int fd) {
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

typedef int (*ConnectFunctionType)(int, const sockaddr*, socklen_t);
typedef int (*AcceptFunctionType)(int, sockaddr*, socklen_t*);
typedef unsigned (*NetIdForResolvFunctionType)(unsigned);

// These variables are only modified at startup (when libc.so is loaded) and never afterwards, so
// it's okay that they are read later at runtime without a lock.
ConnectFunctionType libcConnect = 0;
AcceptFunctionType libcAccept = 0;

int netdClientConnect(int sockfd, const sockaddr* addr, socklen_t addrlen) {
    if (FwmarkClient::shouldSetFwmark(sockfd, addr)) {
        char data[] = {FWMARK_COMMAND_ON_CONNECT};
        if (!FwmarkClient().send(data, sizeof(data), sockfd)) {
            return -1;
        }
    }
    return libcConnect(sockfd, addr, addrlen);
}

int netdClientAccept(int sockfd, sockaddr* addr, socklen_t* addrlen) {
    int acceptedSocket = libcAccept(sockfd, addr, addrlen);
    if (acceptedSocket == -1) {
        return -1;
    }
    sockaddr socketAddress;
    if (!addr) {
        socklen_t socketAddressLen = sizeof(socketAddress);
        if (getsockname(acceptedSocket, &socketAddress, &socketAddressLen) == -1) {
            return closeFdAndRestoreErrno(acceptedSocket);
        }
        addr = &socketAddress;
    }
    if (FwmarkClient::shouldSetFwmark(acceptedSocket, addr)) {
        char data[] = {FWMARK_COMMAND_ON_ACCEPT};
        if (!FwmarkClient().send(data, sizeof(data), acceptedSocket)) {
            return closeFdAndRestoreErrno(acceptedSocket);
        }
    }
    return acceptedSocket;
}

volatile sig_atomic_t netIdForProcess = NETID_UNSET;
volatile sig_atomic_t netIdForResolv = NETID_UNSET;

unsigned getNetworkForResolv(unsigned netId) {
    if (netId != NETID_UNSET) {
        return netId;
    }
    netId = netIdForProcess;
    if (netId != NETID_UNSET) {
        return netId;
    }
    return netIdForResolv;
}

bool setNetworkForTarget(unsigned netId, volatile sig_atomic_t* target) {
    if (netId == NETID_UNSET) {
        *target = netId;
        return true;
    }
    // Don't create an AF_INET socket, because that might cause a useless IPC to the fwmark server.
    int socketFd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socketFd < 0) {
        return false;
    }
    bool status = setNetworkForSocket(netId, socketFd);
    closeFdAndRestoreErrno(socketFd);
    if (status) {
        *target = netId;
    }
    return status;
}

}  // namespace

extern "C" void netdClientInitConnect(ConnectFunctionType* function) {
    if (function && *function) {
        libcConnect = *function;
        *function = netdClientConnect;
    }
}

extern "C" void netdClientInitAccept(AcceptFunctionType* function) {
    if (function && *function) {
        libcAccept = *function;
        *function = netdClientAccept;
    }
}

extern "C" void netdClientInitNetIdForResolv(NetIdForResolvFunctionType* function) {
    if (function) {
        *function = getNetworkForResolv;
    }
}

extern "C" bool setNetworkForSocket(unsigned netId, int socketFd) {
    if (socketFd < 0) {
        errno = EBADF;
        return false;
    }
    char data[1 + sizeof(netId)] = {FWMARK_COMMAND_SELECT_NETWORK};
    memcpy(&data[1], &netId, sizeof(netId));
    return FwmarkClient().send(data, sizeof(data), socketFd);
}

extern "C" bool setNetworkForProcess(unsigned netId) {
    return setNetworkForTarget(netId, &netIdForProcess);
}

extern "C" bool setNetworkForResolv(unsigned netId) {
    return setNetworkForTarget(netId, &netIdForResolv);
}
