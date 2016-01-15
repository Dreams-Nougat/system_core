/*
 * Copyright (C) 2006 The Android Open Source Project
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

#ifndef __CUTILS_SOCKETS_H
#define __CUTILS_SOCKETS_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>

typedef int  socklen_t;
typedef SOCKET cutils_socket_t;

#else

#include <sys/socket.h>

typedef int cutils_socket_t;
#define INVALID_SOCKET (-1)

#endif

#define ANDROID_SOCKET_ENV_PREFIX	"ANDROID_SOCKET_"
#define ANDROID_SOCKET_DIR		"/dev/socket"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * android_get_control_socket - simple helper function to get the file
 * descriptor of our init-managed Unix domain socket. `name' is the name of the
 * socket, as given in init.rc. Returns -1 on error.
 *
 * This is inline and not in libcutils proper because we want to use this in
 * third-party daemons with minimal modification.
 */
static inline int android_get_control_socket(const char* name)
{
	char key[64];
	snprintf(key, sizeof(key), ANDROID_SOCKET_ENV_PREFIX "%s", name);

	const char* val = getenv(key);
	if (!val) {
		return -1;
	}

	errno = 0;
	int fd = strtol(val, NULL, 10);
	if (errno) {
		return -1;
	}

	return fd;
}

/*
 * See also android.os.LocalSocketAddress.Namespace
 */
// Linux "abstract" (non-filesystem) namespace
#define ANDROID_SOCKET_NAMESPACE_ABSTRACT 0
// Android "reserved" (/dev/socket) namespace
#define ANDROID_SOCKET_NAMESPACE_RESERVED 1
// Normal filesystem namespace
#define ANDROID_SOCKET_NAMESPACE_FILESYSTEM 2

/*
 * Functions to create sockets for some common usages.
 *
 * All these functions are implemented for Unix, but only a few are implemented
 * for Windows. Those which are can be identified by the cutils_socket_t
 * return type. The idea is to be able to use this return value with the
 * standard Unix socket functions on any platform.
 *
 * On Unix the returned cutils_socket_t is a standard int file descriptor and
 * can always be used as normal with all file descriptor functions.
 *
 * On Windows utils_socket_t is an unsigned int pointer, and is only valid
 * with functions that specifically take a socket, e.g. send(), sendto(),
 * recv(), and recvfrom(). General file descriptor functions such as read(),
 * write(), and close() will not work with utils_socket_t and will require
 * special handling.
 *
 * These functions return INVALID_SOCKET (-1) on failure for all platforms.
 */
int socket_loopback_client(int port, int type);
cutils_socket_t socket_network_client(const char* host, int port, int type);
int socket_network_client_timeout(const char* host, int port, int type,
                                  int timeout, int* getaddrinfo_error);
int socket_loopback_server(int port, int type);
int socket_local_server(const char* name, int namespaceId, int type);
int socket_local_server_bind(int s, const char* name, int namespaceId);
int socket_local_client_connect(int fd, const char *name, int namespaceId,
                                int type);
int socket_local_client(const char* name, int namespaceId, int type);
cutils_socket_t socket_inaddr_any_server(int port, int type);

/*
 * Closes a cutils_socket_t. Windows doesn't allow calling close() on a socket
 * so this is a cross-platform way to close a cutils_socket_t.
 *
 * Returns 0 on success.
 */
int socket_close(cutils_socket_t sock);

// Sets socket receive timeout using SO_RCVTIMEO.
int socket_set_receive_timeout(cutils_socket_t sock, int timeout_ms);

/*
 * socket_peer_is_trusted - Takes a socket which is presumed to be a
 * connected local socket (e.g. AF_LOCAL) and returns whether the peer
 * (the userid that owns the process on the other end of that socket)
 * is one of the two trusted userids, root or shell.
 *
 * Note: This only works as advertised on the Android OS and always
 * just returns true when called on other operating systems.
 */
extern bool socket_peer_is_trusted(int fd);

#ifdef __cplusplus
}
#endif

#endif /* __CUTILS_SOCKETS_H */
