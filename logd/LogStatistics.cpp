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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/logger.h>
#include <private/android_filesystem_config.h>
#include <utils/String8.h>

#include "LogStatistics.h"

LogStatistics::LogStatistics() {
    log_id_for_each(id) {
        mSizes[id] = 0;
        mElements[id] = 0;
        mSizesTotal[id] = 0;
        mElementsTotal[id] = 0;
    }
}

// caller must own and free character string
char *LogStatistics::pidToName(pid_t pid) {
    char *retval = NULL;
    if (pid == 0) { // special case from auditd for kernel
        retval = strdup("logd.auditd");
    } else {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "/proc/%u/cmdline", pid);
        int fd = open(buffer, O_RDONLY);
        if (fd >= 0) {
            ssize_t ret = read(fd, buffer, sizeof(buffer));
            if (ret > 0) {
                buffer[sizeof(buffer)-1] = '\0';
                // frameworks intermediate state
                if (strcmp(buffer, "<pre-initialized>")) {
                    retval = strdup(buffer);
                }
            }
            close(fd);
        }
    }
    return retval;
}

void LogStatistics::add(LogBufferElement *e) {
    log_id_t log_id = e->getLogId();
    unsigned short size = e->getMsgLen();
    mSizes[log_id] += size;
    ++mElements[log_id];

    uid_t uid = e->getUid();
    android::hash_t hash = android::hash_type(uid);
    uidTable_t &table = uidTable[log_id];
    ssize_t index = table.find(-1, hash, uid);
    if (index == -1) {
        UidEntry initEntry(uid);
        initEntry.add(size);
        table.add(hash, initEntry);
    } else {
        UidEntry &entry = table.editEntryAt(index);
        entry.add(size);
    }

    mSizesTotal[log_id] += size;
    ++mElementsTotal[log_id];
}

void LogStatistics::subtract(LogBufferElement *e) {
    log_id_t log_id = e->getLogId();
    unsigned short size = e->getMsgLen();
    mSizes[log_id] -= size;
    --mElements[log_id];

    uid_t uid = e->getUid();
    android::hash_t hash = android::hash_type(uid);
    uidTable_t &table = uidTable[log_id];
    ssize_t index = table.find(-1, hash, uid);
    if (index != -1) {
        UidEntry &entry = table.editEntryAt(index);
        if (entry.subtract(size)) {
            table.removeAt(index);
        }
    }
}

// caller must own and delete UidEntry array
const UidEntry **LogStatistics::sort(size_t n, log_id id) {
    if (!n) {
        return NULL;
    }

    const UidEntry **retval = new const UidEntry* [n];
    memset(retval, 0, sizeof(*retval) * n);

    uidTable_t &table = uidTable[id];
    ssize_t index = -1;
    while ((index = table.next(index)) >= 0) {
        const UidEntry &entry = table.entryAt(index);
        size_t s = entry.getSizes();
        ssize_t i = n - 1;
        while ((!retval[i] || (s > retval[i]->getSizes())) && (--i >= 0));
        if (++i < (ssize_t)n) {
            size_t b = n - i - 1;
            if (b) {
                memmove(&retval[i+1], &retval[i], b * sizeof(retval[0]));
            }
            retval[i] = &entry;
        }
    }
    return retval;
}

// caller must own and free character string
char *LogStatistics::uidToName(uid_t uid) {
    // Local hard coded favourites
    if (uid == AID_LOGD) {
        return strdup("auditd");
    }

    // Android hard coded
    const struct android_id_info *info = android_ids;

    for (size_t i = 0; i < android_id_count; ++i) {
        if (info->aid == uid) {
            return strdup(info->name);
        }
        ++info;
    }

    // No one
    return NULL;
}

void LogStatistics::format(char **buf, uid_t uid, unsigned int logMask) {
    static const unsigned short spaces_total = 19;

    if (*buf) {
        free(*buf);
        *buf = NULL;
    }

    // Report on total logging, current and for all time

    android::String8 string("size/num");
    size_t oldLength;
    short spaces = 1;

    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }
        oldLength = string.length();
        if (spaces < 0) {
            spaces = 0;
        }
        string.appendFormat("%*s%s", spaces, "", android_log_id_to_name(id));
        spaces += spaces_total + oldLength - string.length();
    }

    spaces = 4;
    string.appendFormat("\nTotal");

    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }
        oldLength = string.length();
        if (spaces < 0) {
            spaces = 0;
        }
        string.appendFormat("%*s%zu/%zu", spaces, "",
                            sizesTotal(id), elementsTotal(id));
        spaces += spaces_total + oldLength - string.length();
    }

    spaces = 6;
    string.appendFormat("\nNow");

    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }

        size_t els = elements(id);
        if (els) {
            oldLength = string.length();
            if (spaces < 0) {
                spaces = 0;
            }
            string.appendFormat("%*s%zu/%zu", spaces, "", sizes(id), els);
            spaces -= string.length() - oldLength;
        }
        spaces += spaces_total;
    }

    // Report on Chattiest

    // Chattiest by application (UID)
    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }

        static const size_t maximum_sorted_entries = 32;
        const UidEntry **sorted = sort(maximum_sorted_entries, id);

        if (!sorted) {
            continue;
        }

        bool print = false;
        size_t len = 0;
        for(size_t index = 0; index < maximum_sorted_entries; ++index) {
            const UidEntry *entry = sorted[index];

            if (!entry) {
                continue;
            }

            size_t sizes = entry->getSizes();
            if (!sizes) {
                continue;
            }

            uid_t u = entry->getKey();
            if ((uid != AID_ROOT) && (u != uid)) {
                continue;
            }

            if (!print) {
                if (uid == AID_ROOT) {
                    string.appendFormat(
                        "\n\nChattiest UIDs in %s:\nUID%*s UID%*s\n",
                        android_log_id_to_name(id),
                        (spaces_total * 2) - 4, "Size",
                        (spaces_total * 2) - 4, "Size");
                } else {
                    string.appendFormat(
                        "\n\nLogging for your UID in %s:\n",
                        android_log_id_to_name(id));
                }
                print = true;
                len = 0;
            }

            // Line up content with two headers
            spaces = -len;
            while (spaces < 0) {
                spaces += spaces_total * 2;
            }
            android::String8 s("");
            s.appendFormat("%*s", spaces, "");
            spaces = 0;

            char *name = uidToName(u);
            android::String8 k("");
            if (name) {
                k.appendFormat("%s", name);
                free(name);
            } else {
                k.appendFormat("[%u]", u);
            }
            spaces += (spaces_total * 2) - k.length() - 1;

            android::String8 l("");
            l.appendFormat("%zu", sizes);

            while (spaces <= (ssize_t)l.length()) {
                spaces += spaces_total * 2;
            }

            android::String8 v("");
            v.appendFormat("%s%s%*s", s.string(), k.string(), spaces, l.string());
            s.setTo("");

            // Deal with line wrap
            if ((len + v.length()) > 80) {
                v.setTo("");
                v.appendFormat("%s%*s", k.string(), spaces, l.string());

                if (v.length() > 80) { // Too much to align?
                    v.setTo("");
                    v.appendFormat("%s %s", k.string(), l.string());
                }

                string.appendFormat("\n");
                len = 0;
            }
            l.setTo("");
            k.setTo("");
            string.appendFormat("%s", v.string());
            len += v.length();
        }

        delete [] sorted;
    }

    *buf = strdup(string.string());
}

uid_t LogStatistics::pidToUid(pid_t pid) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "/proc/%u/status", pid);
    FILE *fp = fopen(buffer, "r");
    if (fp) {
        while (fgets(buffer, sizeof(buffer), fp)) {
            int uid;
            if (sscanf(buffer, "Groups: %d", &uid) == 1) {
                fclose(fp);
                return uid;
            }
        }
        fclose(fp);
    }
    return getuid(); // associate this with the logger
}
