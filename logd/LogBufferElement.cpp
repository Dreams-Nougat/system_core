/*
 * Copyright (C) 2012-2014 The Android Open Source Project
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

#include <endian.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/logger.h>
#include <private/android_logger.h>

#include "LogBufferElement.h"
#include "LogCommand.h"
#include "LogReader.h"

const uint64_t LogBufferElement::FLUSH_ERROR(0);
atomic_int_fast64_t LogBufferElement::sequence;

LogBufferElement::LogBufferElement(log_id_t log_id, log_time realtime,
                                   uid_t uid, pid_t pid, pid_t tid,
                                   const char *msg, unsigned short len)
        : mLogId(log_id)
        , mUid(uid)
        , mPid(pid)
        , mTid(tid)
        , mMsgLen(len)
        , mSequence(sequence.fetch_add(1, memory_order_relaxed))
        , mRealTime(realtime) {
    mMsg = new char[len];
    memcpy(mMsg, msg, len);
}

LogBufferElement::~LogBufferElement() {
    delete [] mMsg;
}

// caller must own and free character string
static char *tidToName(pid_t tid) {
    char *retval = NULL;
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "/proc/%u/comm", tid);
    int fd = open(buffer, O_RDONLY);
    if (fd >= 0) {
        ssize_t ret = read(fd, buffer, sizeof(buffer));
        if (ret > 0) {
            buffer[sizeof(buffer)-1] = '\0';
            retval = strdup(buffer);
        }
        close(fd);
    }
    return retval;
}

// assumption: mMsg == NULL
size_t LogBufferElement::populateDroppedMessage(char *&buffer, SocketClient *reader) {
    static const char format_uid[] = "uid=%u%s too chatty dropped=%u";
    static const size_t unprivileged_offset = 9;
    static const char tag[] = "logd";
    char *name = NULL;
    bool privileged = clientHasLogCredentials(reader) || mUid == reader->getUid();
    size_t len;
    if (privileged) {
        char *n = android::uidToName(mUid);
        if (n) {
            asprintf(&name, "(%s)", n);
            free(n);
        }
        n = tidToName(mTid);
        if (!n) {
            n = tidToName(mPid);
            if (!n) {
                n = android::pidToName(mTid);
                if (!n) {
                    n = android::pidToName(mPid);
                }
            }
        }
        if (n) {
            if (name) {
                char *p = NULL;
                asprintf(&p, "%s comm=%s", name, n);
                if (p) {
                    free(name);
                    name = p;
                }
            } else {
                asprintf(&name, " comm=%s", n);
            }
            free(n);
        }
        len = snprintf(NULL, 0, format_uid, mUid, name ? name : "", mDropped);
    } else {
        len = snprintf(NULL, 0, format_uid + unprivileged_offset, mDropped);
    }

    size_t hdrLen;
    if (mLogId == LOG_ID_EVENTS) {
        hdrLen = sizeof(android_log_event_string_t);
    } else {
        hdrLen = 1 + sizeof(tag);
    }

    buffer = static_cast<char *>(calloc(1, hdrLen + len + 1));
    if (!buffer) {
        return 0;
    }

    size_t retval = hdrLen + len;
    if (mLogId == LOG_ID_EVENTS) {
        android_log_event_string_t *e = reinterpret_cast<android_log_event_string_t *>(buffer);

        e->header.tag = htole32(LOGD_LOG_TAG);
        e->type = EVENT_TYPE_STRING;
        e->length = htole32(len);
    } else {
        ++retval;
        buffer[0] = ANDROID_LOG_INFO;
        strcpy(buffer + 1, tag);
    }

    if (privileged) {
        snprintf(buffer + hdrLen, len + 1, format_uid, mUid, name ? name : "", mDropped);
    } else {
        snprintf(buffer + hdrLen, len + 1, format_uid + unprivileged_offset, mDropped);
    }

    return retval;
}

uint64_t LogBufferElement::flushTo(SocketClient *reader) {
    struct logger_entry_v3 entry;

    memset(&entry, 0, sizeof(struct logger_entry_v3));

    entry.hdr_size = sizeof(struct logger_entry_v3);
    entry.lid = mLogId;
    entry.pid = mPid;
    entry.tid = mTid;
    entry.sec = mRealTime.tv_sec;
    entry.nsec = mRealTime.tv_nsec;

    struct iovec iovec[2];
    iovec[0].iov_base = &entry;
    iovec[0].iov_len = sizeof(struct logger_entry_v3);

    char *buffer = NULL;

    if (!mMsg) {
        entry.len = populateDroppedMessage(buffer, reader);
        if (!entry.len) {
            return mSequence;
        }
        iovec[1].iov_base = buffer;
    } else {
        entry.len = mMsgLen;
        iovec[1].iov_base = mMsg;
    }
    iovec[1].iov_len = entry.len;

    uint64_t retval = reader->sendDatav(iovec, 2) ? FLUSH_ERROR : mSequence;

    if (buffer) {
        free(buffer);
    }

    return retval;
}
