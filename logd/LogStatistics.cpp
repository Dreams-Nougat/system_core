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

#include <algorithm> // std::max
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <log/logger.h>
#include <private/android_filesystem_config.h>
#include <utils/String8.h>

#include "LogStatistics.h"

LogStatistics::LogStatistics()
        : enable(false) {
    log_id_for_each(id) {
        mSizes[id] = 0;
        mElements[id] = 0;
        mSizesTotal[id] = 0;
        mElementsTotal[id] = 0;
    }
}

namespace android {

// caller must own and free character string
char *pidToName(pid_t pid) {
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

}

void LogStatistics::add(LogBufferElement *e) {
    log_id_t log_id = e->getLogId();
    unsigned short size = e->getMsgLen();
    mSizes[log_id] += size;
    ++mElements[log_id];

    uidTable[log_id].add(e->getUid(), e);

    mSizesTotal[log_id] += size;
    ++mElementsTotal[log_id];

    if (!enable) {
        return;
    }

    pidTable.add(e->getPid(), e);

    uint32_t tag = e->getTag();
    if (tag) {
        tagTable.add(tag, e);
    }
}

void LogStatistics::subtract(LogBufferElement *e) {
    log_id_t log_id = e->getLogId();
    unsigned short size = e->getMsgLen();
    mSizes[log_id] -= size;
    --mElements[log_id];

    uidTable[log_id].subtract(e->getUid(), e);

    if (!enable) {
        return;
    }

    pidTable.subtract(e->getPid(), e);

    uint32_t tag = e->getTag();
    if (tag) {
        tagTable.subtract(tag, e);
    }
}

// Atomically set an entry to drop
// entry->setDropped(1) must follow this call, caller should do this explicitly.
void LogStatistics::drop(LogBufferElement *e) {
    log_id_t log_id = e->getLogId();
    unsigned short size = e->getMsgLen();
    mSizes[log_id] -= size;

    uidTable[log_id].drop(e->getUid(), e);

    if (!enable) {
        return;
    }

    pidTable.drop(e->getPid(), e);
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

    // Parse /data/system/packages.list
    char *name = android::uidToName(uid);
    if (name) {
        return name;
    }

    // report uid -> pid(s) -> pidToName if unique
    ssize_t index = -1;
    while ((index = pidTable.next(index)) != -1) {
        const PidEntry &entry = pidTable.entryAt(index);

        if (entry.getUid() == uid) {
            const char *n = entry.getName();

            if (n) {
                if (!name) {
                    name = strdup(n);
                } else if (strcmp(name, n)) {
                    free(name);
                    return NULL;
                }
            }
        }
    }

    // No one
    return name;
}

static void format_line(android::String8 &output,
        android::String8 &name, android::String8 &size, android::String8 &pruned) {
    static const size_t pruned_len = 6;
    static const size_t total_len = 70 + pruned_len;

    ssize_t drop_len = std::max(pruned.length() + 1, pruned_len);
    ssize_t size_len = std::max(size.length() + 1,
                                total_len - name.length() - drop_len - 1);

    if (pruned.length()) {
        output.appendFormat("%s%*s%*s\n", name.string(),
                                          (int)size_len, size.string(),
                                          (int)drop_len, pruned.string());
    } else {
        output.appendFormat("%s%*s\n", name.string(),
                                       (int)size_len, size.string());
    }
}

void LogStatistics::format(char **buf, uid_t uid, unsigned int logMask) {
    static const unsigned short spaces_total = 19;

    if (*buf) {
        free(*buf);
        *buf = NULL;
    }

    // Report on total logging, current and for all time

    android::String8 output("size/num");
    size_t oldLength;
    short spaces = 1;

    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }
        oldLength = output.length();
        if (spaces < 0) {
            spaces = 0;
        }
        output.appendFormat("%*s%s", spaces, "", android_log_id_to_name(id));
        spaces += spaces_total + oldLength - output.length();
    }

    spaces = 4;
    output.appendFormat("\nTotal");

    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }
        oldLength = output.length();
        if (spaces < 0) {
            spaces = 0;
        }
        output.appendFormat("%*s%zu/%zu", spaces, "",
                            sizesTotal(id), elementsTotal(id));
        spaces += spaces_total + oldLength - output.length();
    }

    spaces = 6;
    output.appendFormat("\nNow");

    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }

        size_t els = elements(id);
        if (els) {
            oldLength = output.length();
            if (spaces < 0) {
                spaces = 0;
            }
            output.appendFormat("%*s%zu/%zu", spaces, "", sizes(id), els);
            spaces -= output.length() - oldLength;
        }
        spaces += spaces_total;
    }

    // Report on Chattiest

    // Chattiest by application (UID)
    static const size_t maximum_sorted_entries = 32;
    log_id_for_each(id) {
        if (!(logMask & (1 << id))) {
            continue;
        }

        bool headerPrinted = false;
        std::unique_ptr<const UidEntry *[]> sorted = sort(maximum_sorted_entries, id);
        ssize_t index = -1;
        while ((index = uidTable_t::next(index, sorted, maximum_sorted_entries)) >= 0) {
            const UidEntry *entry = sorted[index];
            uid_t u = entry->getKey();
            if ((uid != AID_ROOT) && (u != uid)) {
                continue;
            }

            if (!headerPrinted) {
                if (uid == AID_ROOT) {
                    output.appendFormat(
                        "\n\nChattiest UIDs in %s:\n",
                        android_log_id_to_name(id));
                } else {
                    output.appendFormat(
                        "\n\nLogging for your UID in %s:\n",
                        android_log_id_to_name(id));
                }
                android::String8 name("UID");
                android::String8 size("Size");
                android::String8 pruned("Pruned");
                if (!worstUidEnabledForLogid(id)) {
                    pruned.setTo("");
                }
                format_line(output, name, size, pruned);
                headerPrinted = true;
            }

            android::String8 name("");
            name.appendFormat("%u", u);
            char *n = uidToName(u);
            if (n) {
                name.appendFormat("%*s%s", (int)std::max(6 - name.length(), (size_t)1), "", n);
                free(n);
            }

            android::String8 size("");
            size.appendFormat("%zu", entry->getSizes());

            android::String8 pruned("");
            size_t dropped = entry->getDropped();
            if (dropped) {
                pruned.appendFormat("%zu", dropped);
            }

            format_line(output, name, size, pruned);
        }
    }

    if (enable) {
        // Pid table
        bool headerPrinted = false;
        std::unique_ptr<const PidEntry *[]> sorted = pidTable.sort(maximum_sorted_entries);
        ssize_t index = -1;
        while ((index = pidTable.next(index, sorted, maximum_sorted_entries)) >= 0) {
            const PidEntry *entry = sorted[index];
            uid_t u = entry->getUid();
            if ((uid != AID_ROOT) && (u != uid)) {
                continue;
            }

            if (!headerPrinted) {
                if (uid == AID_ROOT) {
                    output.appendFormat("\n\nChattiest PIDs:\n");
                } else {
                    output.appendFormat("\n\nLogging for this PID:\n");
                }
                android::String8 name("  PID/UID");
                android::String8 size("Size");
                android::String8 pruned("Pruned");
                format_line(output, name, size, pruned);
                headerPrinted = true;
            }

            android::String8 name("");
            name.appendFormat("%5u/%u", entry->getKey(), u);
            const char *n = entry->getName();
            if (n) {
                name.appendFormat("%*s%s", (int)std::max(12 - name.length(), (size_t)1), "", n);
            } else {
                char *un = uidToName(u);
                if (un) {
                    name.appendFormat("%*s%s", (int)std::max(12 - name.length(), (size_t)1), "", un);
                    free(un);
                }
            }

            android::String8 size("");
            size.appendFormat("%zu", entry->getSizes());

            android::String8 pruned("");
            size_t dropped = entry->getDropped();
            if (dropped) {
                pruned.appendFormat("%zu", dropped);
            }

            format_line(output, name, size, pruned);
        }
    }

    if (enable && (logMask & (1 << LOG_ID_EVENTS))) {
        // Tag table
        bool headerPrinted = false;
        std::unique_ptr<const TagEntry *[]> sorted = tagTable.sort(maximum_sorted_entries);
        ssize_t index = -1;
        while ((index = tagTable.next(index, sorted, maximum_sorted_entries)) >= 0) {
            const TagEntry *entry = sorted[index];
            uid_t u = entry->getUid();
            if ((uid != AID_ROOT) && (u != uid)) {
                continue;
            }

            android::String8 pruned("");

            if (!headerPrinted) {
                output.appendFormat("\n\nChattiest events buffer TAGs:\n");
                android::String8 name("    TAG/UID");
                android::String8 size("Size");
                format_line(output, name, size, pruned);
                headerPrinted = true;
            }

            android::String8 name("");
            if (u == (uid_t)-1) {
                name.appendFormat("%7u", entry->getKey());
            } else {
                name.appendFormat("%7u/%u", entry->getKey(), u);
            }
            const char *n = entry->getName();
            if (n) {
                name.appendFormat("%*s%s", (int)std::max(14 - name.length(), (size_t)1), "", n);
            }

            android::String8 size("");
            size.appendFormat("%zu", entry->getSizes());

            format_line(output, name, size, pruned);
        }
    }

    *buf = strdup(output.string());
}

namespace android {

uid_t pidToUid(pid_t pid) {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "/proc/%u/status", pid);
    FILE *fp = fopen(buffer, "r");
    if (fp) {
        while (fgets(buffer, sizeof(buffer), fp)) {
            int uid;
            if (sscanf(buffer, "Uid: %d", &uid) == 1) {
                fclose(fp);
                return uid;
            }
        }
        fclose(fp);
    }
    return AID_LOGD; // associate this with the logger
}

}

uid_t LogStatistics::pidToUid(pid_t pid) {
    return pidTable.entryAt(pidTable.add(pid)).getUid();
}

// caller must free character string
char *LogStatistics::pidToName(pid_t pid) {
    const char *name = pidTable.entryAt(pidTable.add(pid)).getName();
    if (!name) {
        return NULL;
    }
    return strdup(name);
}
