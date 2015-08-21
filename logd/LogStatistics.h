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

#ifndef _LOGD_LOG_STATISTICS_H__
#define _LOGD_LOG_STATISTICS_H__

#include <memory>
#include <stdlib.h>
#include <sys/types.h>

#include <algorithm> // std::max
#include <string>    // std::string
#include <unordered_map>

#include <base/stringprintf.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>

#include "LogBufferElement.h"

#define log_id_for_each(i) \
    for (log_id_t i = LOG_ID_MIN; i < LOG_ID_MAX; i = (log_id_t) (i + 1))

class LogStatistics;

template <typename TKey, typename TEntry>
class LogHashtable {

    std::unordered_map<TKey, TEntry> map;

public:

    typedef typename std::unordered_map<TKey, TEntry>::iterator iterator;
    typedef typename std::unordered_map<TKey, TEntry>::const_iterator const_iterator;

    std::unique_ptr<const TEntry *[]> sort(size_t n) const {
        if (!n) {
            std::unique_ptr<const TEntry *[]> sorted(NULL);
            return sorted;
        }

        const TEntry **retval = new const TEntry* [n];
        memset(retval, 0, sizeof(*retval) * n);

        for(const_iterator it = map.begin(); it != map.end(); ++it) {
            const TEntry &entry = it->second;
            size_t s = entry.getSizes();
            ssize_t i = n - 1;
            while ((!retval[i] || (s > retval[i]->getSizes())) && (--i >= 0))
                ;
            if (++i < (ssize_t)n) {
                size_t b = n - i - 1;
                if (b) {
                    memmove(&retval[i+1], &retval[i], b * sizeof(retval[0]));
                }
                retval[i] = &entry;
            }
        }
        std::unique_ptr<const TEntry *[]> sorted(retval);
        return sorted;
    }

    inline iterator add(TKey key, LogBufferElement *e) {
        iterator it = map.find(key);
        if (it == map.end()) {
            it = map.insert(std::make_pair(key, TEntry(e))).first;
        } else {
            it->second.add(e);
        }
        return it;
    }

    inline iterator add(TKey key) {
        iterator it = map.find(key);
        if (it == map.end()) {
            it = map.insert(std::make_pair(key, TEntry(key))).first;
        } else {
            it->second.add(key);
        }
        return it;
    }

    void subtract(TKey key, LogBufferElement *e) {
        iterator it = map.find(key);
        if ((it != map.end()) && it->second.subtract(e)) {
            map.erase(it);
        }
    }

    inline void drop(TKey key, LogBufferElement *e) {
        iterator it = map.find(key);
        if (it != map.end()) {
            it->second.drop(e);
        }
    }

    inline iterator begin() { return map.begin(); }
    inline const_iterator begin() const { return map.begin(); }
    inline iterator end() { return map.end(); }
    inline const_iterator end() const { return map.end(); }

    std::string format(
            const LogStatistics &stat,
            uid_t uid,
            const std::string &name = std::string(""),
            log_id_t id = LOG_ID_MAX) const {
        static const size_t maximum_sorted_entries = 32;
        std::string output;
        std::unique_ptr<const TEntry *[]> sorted = sort(maximum_sorted_entries);

        if (!sorted.get()) {
            return output;
        }
        bool headerPrinted = false;
        for (size_t index = 0; index < maximum_sorted_entries; ++index) {
            const TEntry *entry = sorted[index];
            if (!entry) {
                break;
            }
            if (entry->getSizes() <= (sorted[0]->getSizes() / 100)) {
                break;
            }
            if ((uid != AID_ROOT) && (uid != entry->getUid())) {
                continue;
            }
            if (!headerPrinted) {
                output += "\n\n";
                output += entry->formatHeader(name, id);
                headerPrinted = true;
            }
            output += entry->format(stat, id);
        }
        return output;
    }
};

namespace EntryBaseConstants {
    static constexpr size_t pruned_len = 14;
    static constexpr size_t total_len = 18;
}

struct EntryBase {
    size_t size;

    EntryBase():size(0) { }
    EntryBase(LogBufferElement *e):size(e->getMsgLen()) { }

    size_t getSizes() const { return size; }

    inline void add(LogBufferElement *e) { size += e->getMsgLen(); }
    inline bool subtract(LogBufferElement *e) { size -= e->getMsgLen(); return !size; }

    static std::string formatLine(
            const std::string &name,
            const std::string &size,
            const std::string &pruned) {
        ssize_t drop_len = std::max(pruned.length() + 1,
                                    EntryBaseConstants::pruned_len);
        ssize_t size_len = std::max(size.length() + 1,
                                    EntryBaseConstants::total_len
                                        - name.length() - drop_len - 1);

        if (pruned.length()) {
            return android::base::StringPrintf("%s%*s%*s\n", name.c_str(),
                                               (int)size_len, size.c_str(),
                                               (int)drop_len, pruned.c_str());
        } else {
            return android::base::StringPrintf("%s%*s\n", name.c_str(),
                                               (int)size_len, size.c_str());
        }
    }
};

struct EntryBaseDropped : public EntryBase {
    size_t dropped;

    EntryBaseDropped():dropped(0) { }
    EntryBaseDropped(LogBufferElement *e):EntryBase(e),dropped(e->getDropped()){ }

    size_t getDropped() const { return dropped; }

    inline void add(LogBufferElement *e) {
        dropped += e->getDropped();
        EntryBase::add(e);
    }
    inline bool subtract(LogBufferElement *e) {
        dropped -= e->getDropped();
        return EntryBase::subtract(e) && !dropped;
    }
    inline void drop(LogBufferElement *e) {
        dropped += 1;
        EntryBase::subtract(e);
    }
};

struct UidEntry : public EntryBaseDropped {
    const uid_t uid;

    UidEntry(LogBufferElement *e):EntryBaseDropped(e),uid(e->getUid()) { }

    inline const uid_t&getKey() const { return uid; }
    inline const uid_t&getUid() const { return uid; }

    std::string formatHeader(const std::string &name, log_id_t id) const;
    std::string format(const LogStatistics &stat, log_id_t id) const;
};

namespace android {
uid_t pidToUid(pid_t pid);
}

struct PidEntry : public EntryBaseDropped {
    const pid_t pid;
    uid_t uid;
    char *name;

    PidEntry(pid_t p):
        EntryBaseDropped(),
        pid(p),
        uid(android::pidToUid(p)),
        name(android::pidToName(pid)) { }
    PidEntry(LogBufferElement *e):
        EntryBaseDropped(e),
        pid(e->getPid()),
        uid(e->getUid()),
        name(android::pidToName(e->getPid())) { }
    PidEntry(const PidEntry &c):
        EntryBaseDropped(c),
        pid(c.pid),
        uid(c.uid),
        name(c.name ? strdup(c.name) : NULL) { }
    ~PidEntry() { free(name); }

    const pid_t&getKey() const { return pid; }
    const uid_t&getUid() const { return uid; }
    const char*getName() const { return name; }

    inline void add(pid_t p) {
        if (name && !strncmp(name, "zygote", 6)) {
            free(name);
            name = NULL;
        }
        if (!name) {
            char *n = android::pidToName(p);
            if (n) {
                name = n;
            }
        }
    }

    inline void add(LogBufferElement *e) {
        uid_t u = e->getUid();
        if (getUid() != u) {
            uid = u;
            free(name);
            name = android::pidToName(e->getPid());
        } else {
            add(e->getPid());
        }
        EntryBaseDropped::add(e);
    }

    std::string formatHeader(const std::string &name, log_id_t id) const;
    std::string format(const LogStatistics &stat, log_id_t id) const;
};

struct TidEntry : public EntryBaseDropped {
    const pid_t tid;
    uid_t uid;
    char *name;

    TidEntry(pid_t t):
        EntryBaseDropped(),
        tid(t),
        uid(android::pidToUid(t)),
        name(android::tidToName(tid)) { }
    TidEntry(LogBufferElement *e):
        EntryBaseDropped(e),
        tid(e->getTid()),
        uid(e->getUid()),
        name(android::tidToName(e->getTid())) { }
    TidEntry(const TidEntry &c):
        EntryBaseDropped(c),
        tid(c.tid),
        uid(c.uid),
        name(c.name ? strdup(c.name) : NULL) { }
    ~TidEntry() { free(name); }

    const pid_t&getKey() const { return tid; }
    const uid_t&getUid() const { return uid; }
    const char*getName() const { return name; }

    inline void add(pid_t t) {
        if (name && !strncmp(name, "zygote", 6)) {
            free(name);
            name = NULL;
        }
        if (!name) {
            char *n = android::tidToName(t);
            if (n) {
                name = n;
            }
        }
    }

    inline void add(LogBufferElement *e) {
        uid_t u = e->getUid();
        if (getUid() != u) {
            uid = u;
            free(name);
            name = android::tidToName(e->getTid());
        } else {
            add(e->getTid());
        }
        EntryBaseDropped::add(e);
    }

    std::string formatHeader(const std::string &name, log_id_t id) const;
    std::string format(const LogStatistics &stat, log_id_t id) const;
};

struct TagEntry : public EntryBase {
    const uint32_t tag;
    uid_t uid;

    TagEntry(LogBufferElement *e):
        EntryBase(e),
        tag(e->getTag()),
        uid(e->getUid()) { }

    const uint32_t&getKey() const { return tag; }
    const uid_t&getUid() const { return uid; }
    const char*getName() const { return android::tagToName(tag); }

    inline void add(LogBufferElement *e) {
        uid_t u = e->getUid();
        if (uid != u) {
            uid = -1;
        }
        EntryBase::add(e);
    }

    std::string formatHeader(const std::string &name, log_id_t id) const;
    std::string format(const LogStatistics &stat, log_id_t id) const;
};

// Log Statistics
class LogStatistics {
    size_t mSizes[LOG_ID_MAX];
    size_t mElements[LOG_ID_MAX];
    size_t mSizesTotal[LOG_ID_MAX];
    size_t mElementsTotal[LOG_ID_MAX];
    bool enable;

    // uid to size list
    typedef LogHashtable<uid_t, UidEntry> uidTable_t;
    uidTable_t uidTable[LOG_ID_MAX];

    // pid to uid list
    typedef LogHashtable<pid_t, PidEntry> pidTable_t;
    pidTable_t pidTable;

    // tid to uid list
    typedef LogHashtable<pid_t, TidEntry> tidTable_t;
    tidTable_t tidTable;

    // tag list
    typedef LogHashtable<uint32_t, TagEntry> tagTable_t;
    tagTable_t tagTable;

public:
    LogStatistics();

    void enableStatistics() { enable = true; }

    void add(LogBufferElement *entry);
    void subtract(LogBufferElement *entry);
    // entry->setDropped(1) must follow this call
    void drop(LogBufferElement *entry);
    // Correct for merging two entries referencing dropped content
    void erase(LogBufferElement *e) { --mElements[e->getLogId()]; }

    std::unique_ptr<const UidEntry *[]> sort(size_t n, log_id i) { return uidTable[i].sort(n); }

    // fast track current value by id only
    size_t sizes(log_id_t id) const { return mSizes[id]; }
    size_t elements(log_id_t id) const { return mElements[id]; }
    size_t sizesTotal(log_id_t id) const { return mSizesTotal[id]; }
    size_t elementsTotal(log_id_t id) const { return mElementsTotal[id]; }

    std::string format(uid_t uid, unsigned int logMask) const;

    // helper (must be locked directly or implicitly by mLogElementsLock)
    const char *pidToName(pid_t pid) const;
    uid_t pidToUid(pid_t pid);
    const char *uidToName(uid_t uid) const;
};

#endif // _LOGD_LOG_STATISTICS_H__
