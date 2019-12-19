/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "mediametrics::Item"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <mutex>
#include <set>

#include <binder/Parcel.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/SortedVector.h>
#include <utils/threads.h>

#include <binder/IServiceManager.h>
#include <media/IMediaMetricsService.h>
#include <media/MediaMetricsItem.h>
#include <private/android_filesystem_config.h>

// Max per-property string size before truncation in toString().
// Do not make too large, as this is used for dumpsys purposes.
static constexpr size_t kMaxPropertyStringSize = 4096;

namespace android::mediametrics {

#define DEBUG_SERVICEACCESS     0
#define DEBUG_API               0
#define DEBUG_ALLOCATIONS       0

// after this many failed attempts, we stop trying [from this process] and just say that
// the service is off.
#define SVC_TRIES               2

mediametrics::Item* mediametrics::Item::convert(mediametrics_handle_t handle) {
    mediametrics::Item *item = (android::mediametrics::Item *) handle;
    return item;
}

mediametrics_handle_t mediametrics::Item::convert(mediametrics::Item *item ) {
    mediametrics_handle_t handle = (mediametrics_handle_t) item;
    return handle;
}

mediametrics::Item::~Item() {
    if (DEBUG_ALLOCATIONS) {
        ALOGD("Destroy  mediametrics::Item @ %p", this);
    }
}

mediametrics::Item &mediametrics::Item::setTimestamp(nsecs_t ts) {
    mTimestamp = ts;
    return *this;
}

nsecs_t mediametrics::Item::getTimestamp() const {
    return mTimestamp;
}

mediametrics::Item &mediametrics::Item::setPid(pid_t pid) {
    mPid = pid;
    return *this;
}

pid_t mediametrics::Item::getPid() const {
    return mPid;
}

mediametrics::Item &mediametrics::Item::setUid(uid_t uid) {
    mUid = uid;
    return *this;
}

uid_t mediametrics::Item::getUid() const {
    return mUid;
}

mediametrics::Item &mediametrics::Item::setPkgName(const std::string &pkgName) {
    mPkgName = pkgName;
    return *this;
}

mediametrics::Item &mediametrics::Item::setPkgVersionCode(int64_t pkgVersionCode) {
    mPkgVersionCode = pkgVersionCode;
    return *this;
}

int64_t mediametrics::Item::getPkgVersionCode() const {
    return mPkgVersionCode;
}

// remove indicated keys and their values
// return value is # keys removed
size_t mediametrics::Item::filter(size_t n, const char *attrs[]) {
    size_t zapped = 0;
    for (size_t i = 0; i < n; ++i) {
        zapped += mProps.erase(attrs[i]);
    }
    return zapped;
}

// remove any keys NOT in the provided list
// return value is # keys removed
size_t mediametrics::Item::filterNot(size_t n, const char *attrs[]) {
    std::set<std::string> check(attrs, attrs + n);
    size_t zapped = 0;
    for (auto it = mProps.begin(); it != mProps.end();) {
        if (check.find(it->first) != check.end()) {
            ++it;
        } else {
           it = mProps.erase(it);
           ++zapped;
        }
    }
    return zapped;
}

// Parcel / serialize things for binder calls
//

status_t mediametrics::Item::readFromParcel(const Parcel& data) {
    int32_t version;
    status_t status = data.readInt32(&version);
    if (status != NO_ERROR) return status;

    switch (version) {
    case 0:
      return readFromParcel0(data);
    default:
      ALOGE("%s: unsupported parcel version: %d", __func__, version);
      return INVALID_OPERATION;
    }
}

status_t mediametrics::Item::readFromParcel0(const Parcel& data) {
    const char *s = data.readCString();
    mKey = s == nullptr ? "" : s;
    int32_t pid, uid;
    status_t status = data.readInt32(&pid) ?: data.readInt32(&uid);
    if (status != NO_ERROR) return status;
    mPid = (pid_t)pid;
    mUid = (uid_t)uid;
    s = data.readCString();
    mPkgName = s == nullptr ? "" : s;
    int32_t count;
    int64_t version, timestamp;
    status = data.readInt64(&version) ?: data.readInt64(&timestamp) ?: data.readInt32(&count);
    if (status != NO_ERROR) return status;
    if (count < 0) return BAD_VALUE;
    mPkgVersionCode = version;
    mTimestamp = timestamp;
    for (int i = 0; i < count; i++) {
        Prop prop;
        status_t status = prop.readFromParcel(data);
        if (status != NO_ERROR) return status;
        mProps[prop.getName()] = std::move(prop);
    }
    return NO_ERROR;
}

status_t mediametrics::Item::writeToParcel(Parcel *data) const {
    if (data == nullptr) return BAD_VALUE;

    const int32_t version = 0;
    status_t status = data->writeInt32(version);
    if (status != NO_ERROR) return status;

    switch (version) {
    case 0:
      return writeToParcel0(data);
    default:
      ALOGE("%s: unsupported parcel version: %d", __func__, version);
      return INVALID_OPERATION;
    }
}

status_t mediametrics::Item::writeToParcel0(Parcel *data) const {
    status_t status =
        data->writeCString(mKey.c_str())
        ?: data->writeInt32(mPid)
        ?: data->writeInt32(mUid)
        ?: data->writeCString(mPkgName.c_str())
        ?: data->writeInt64(mPkgVersionCode)
        ?: data->writeInt64(mTimestamp);
    if (status != NO_ERROR) return status;

    data->writeInt32((int32_t)mProps.size());
    for (auto &prop : *this) {
        status = prop.writeToParcel(data);
        if (status != NO_ERROR) return status;
    }
    return NO_ERROR;
}

const char *mediametrics::Item::toCString() {
   return toCString(PROTO_LAST);
}

const char * mediametrics::Item::toCString(int version) {
    std::string val = toString(version);
    return strdup(val.c_str());
}

std::string mediametrics::Item::toString() const {
   return toString(PROTO_LAST);
}

std::string mediametrics::Item::toString(int version) const {
    std::string result;
    char buffer[kMaxPropertyStringSize];

    snprintf(buffer, sizeof(buffer), "[%d:%s:%d:%d:%lld:%s:%zu:",
            version, mKey.c_str(), mPid, mUid, (long long)mTimestamp,
            mPkgName.c_str(), mProps.size());
    result.append(buffer);
    for (auto &prop : *this) {
        prop.toString(buffer, sizeof(buffer));
        result.append(buffer);
    }
    result.append("]");
    return result;
}

// for the lazy, we offer methods that finds the service and
// calls the appropriate daemon
bool mediametrics::Item::selfrecord() {
    ALOGD_IF(DEBUG_API, "%s: delivering %s", __func__, this->toString().c_str());
    sp<IMediaMetricsService> svc = getService();
    if (svc != NULL) {
        status_t status = svc->submit(this);
        if (status != NO_ERROR) {
            ALOGW("%s: failed to record: %s", __func__, this->toString().c_str());
            return false;
        }
        return true;
    } else {
        return false;
    }
}

//static
bool BaseItem::isEnabled() {
    // completely skip logging from certain UIDs. We do this here
    // to avoid the multi-second timeouts while we learn that
    // sepolicy will not let us find the service.
    // We do this only for a select set of UIDs
    // The sepolicy protection is still in place, we just want a faster
    // response from this specific, small set of uids.

    // This is checked only once in the lifetime of the process.
    const uid_t uid = getuid();
    switch (uid) {
    case AID_RADIO:     // telephony subsystem, RIL
        return false;
    }

    int enabled = property_get_int32(Item::EnabledProperty, -1);
    if (enabled == -1) {
        enabled = property_get_int32(Item::EnabledPropertyPersist, -1);
    }
    if (enabled == -1) {
        enabled = Item::EnabledProperty_default;
    }
    return enabled > 0;
}

// monitor health of our connection to the metrics service
class MediaMetricsDeathNotifier : public IBinder::DeathRecipient {
        virtual void binderDied(const wp<IBinder> &) {
            ALOGW("Reacquire service connection on next request");
            BaseItem::dropInstance();
        }
};

static sp<MediaMetricsDeathNotifier> sNotifier;
// static
sp<IMediaMetricsService> BaseItem::sMediaMetricsService;
static std::mutex sServiceMutex;
static int sRemainingBindAttempts = SVC_TRIES;

// static
void BaseItem::dropInstance() {
    std::lock_guard  _l(sServiceMutex);
    sRemainingBindAttempts = SVC_TRIES;
    sMediaMetricsService = nullptr;
}

// static
bool BaseItem::submitBuffer(const char *buffer, size_t size) {
/*
    mediametrics::Item item;
    status_t status = item.readFromByteString(buffer, size);
    ALOGD("%s: status:%d, size:%zu, item:%s", __func__, status, size, item.toString().c_str());
    return item.selfrecord();
    */

    ALOGD_IF(DEBUG_API, "%s: delivering %zu bytes", __func__, size);
    sp<IMediaMetricsService> svc = getService();
    if (svc != nullptr) {
        const status_t status = svc->submitBuffer(buffer, size);
        if (status != NO_ERROR) {
            ALOGW("%s: failed(%d) to record: %zu bytes", __func__, status, size);
            return false;
        }
        return true;
    }
    return false;
}

//static
sp<IMediaMetricsService> BaseItem::getService() {
    static const char *servicename = "media.metrics";
    static const bool enabled = isEnabled(); // singleton initialized

    if (enabled == false) {
        ALOGD_IF(DEBUG_SERVICEACCESS, "disabled");
        return nullptr;
    }
    std::lock_guard _l(sServiceMutex);
    // think of remainingBindAttempts as telling us whether service == nullptr because
    // (1) we haven't tried to initialize it yet
    // (2) we've tried to initialize it, but failed.
    if (sMediaMetricsService == nullptr && sRemainingBindAttempts > 0) {
        const char *badness = "";
        sp<IServiceManager> sm = defaultServiceManager();
        if (sm != nullptr) {
            sp<IBinder> binder = sm->getService(String16(servicename));
            if (binder != nullptr) {
                sMediaMetricsService = interface_cast<IMediaMetricsService>(binder);
                sNotifier = new MediaMetricsDeathNotifier();
                binder->linkToDeath(sNotifier);
            } else {
                badness = "did not find service";
            }
        } else {
            badness = "No Service Manager access";
        }
        if (sMediaMetricsService == nullptr) {
            if (sRemainingBindAttempts > 0) {
                sRemainingBindAttempts--;
            }
            ALOGD_IF(DEBUG_SERVICEACCESS, "%s: unable to bind to service %s: %s",
                    __func__, servicename, badness);
        }
    }
    return sMediaMetricsService;
}


namespace {

template <typename T>
status_t insert(const T& val, char **bufferpptr, char *bufferptrmax)
{
    const size_t size = sizeof(val);
    if (*bufferpptr + size > bufferptrmax) {
        ALOGE("%s: buffer exceeded with size %zu", __func__, size);
        return BAD_VALUE;
    }
    memcpy(*bufferpptr, &val, size);
    *bufferpptr += size;
    return NO_ERROR;
}

template <>
status_t insert(const char * const& val, char **bufferpptr, char *bufferptrmax)
{
    const size_t size = strlen(val) + 1;
    if (size > UINT16_MAX || *bufferpptr + size > bufferptrmax) {
        ALOGE("%s: buffer exceeded with size %zu", __func__, size);
        return BAD_VALUE;
    }
    memcpy(*bufferpptr, val, size);
    *bufferpptr += size;
    return NO_ERROR;
}

template <>
 __unused
status_t insert(char * const& val, char **bufferpptr, char *bufferptrmax)
{
    return insert((const char *)val, bufferpptr, bufferptrmax);
}

template <typename T>
status_t extract(T *val, const char **bufferpptr, const char *bufferptrmax)
{
    const size_t size = sizeof(*val);
    if (*bufferpptr + size > bufferptrmax) {
        ALOGE("%s: buffer exceeded with size %zu", __func__, size);
        return BAD_VALUE;
    }
    memcpy(val, *bufferpptr, size);
    *bufferpptr += size;
    return NO_ERROR;
}

template <>
status_t extract(char **val, const char **bufferpptr, const char *bufferptrmax)
{
    const char *ptr = *bufferpptr;
    while (*ptr != 0) {
        if (ptr >= bufferptrmax) {
            ALOGE("%s: buffer exceeded", __func__);
            return BAD_VALUE;
        }
        ++ptr;
    }
    const size_t size = (ptr - *bufferpptr) + 1;
    *val = (char *)malloc(size);
    memcpy(*val, *bufferpptr, size);
    *bufferpptr += size;
    return NO_ERROR;
}

} // namespace

status_t mediametrics::Item::writeToByteString(char **pbuffer, size_t *plength) const
{
    if (pbuffer == nullptr || plength == nullptr)
        return BAD_VALUE;

    // get size
    const size_t keySizeZeroTerminated = strlen(mKey.c_str()) + 1;
    if (keySizeZeroTerminated > UINT16_MAX) {
        ALOGW("%s: key size %zu too large", __func__, keySizeZeroTerminated);
        return INVALID_OPERATION;
    }
    const uint16_t version = 0;
    const uint32_t header_size =
        sizeof(uint32_t)      // total size
        + sizeof(header_size) // header size
        + sizeof(version)     // encoding version
        + sizeof(uint16_t)    // key size
        + keySizeZeroTerminated // key, zero terminated
        + sizeof(int32_t)     // pid
        + sizeof(int32_t)     // uid
        + sizeof(int64_t)     // timestamp
        ;

    uint32_t size = header_size
        + sizeof(uint32_t) // # properties
        ;
    for (auto &prop : *this) {
        const size_t propSize = prop.getByteStringSize();
        if (propSize > UINT16_MAX) {
            ALOGW("%s: prop %s size %zu too large", __func__, prop.getName(), propSize);
            return INVALID_OPERATION;
        }
        if (__builtin_add_overflow(size, propSize, &size)) {
            ALOGW("%s: item size overflow at property %s", __func__, prop.getName());
            return INVALID_OPERATION;
        }
    }

    // since we fill every byte in the buffer (there is no padding),
    // malloc is used here instead of calloc.
    char * const build = (char *)malloc(size);
    if (build == nullptr) return NO_MEMORY;

    char *filling = build;
    char *buildmax = build + size;
    if (insert((uint32_t)size, &filling, buildmax) != NO_ERROR
            || insert(header_size, &filling, buildmax) != NO_ERROR
            || insert(version, &filling, buildmax) != NO_ERROR
            || insert((uint16_t)keySizeZeroTerminated, &filling, buildmax) != NO_ERROR
            || insert(mKey.c_str(), &filling, buildmax) != NO_ERROR
            || insert((int32_t)mPid, &filling, buildmax) != NO_ERROR
            || insert((int32_t)mUid, &filling, buildmax) != NO_ERROR
            || insert((int64_t)mTimestamp, &filling, buildmax) != NO_ERROR
            || insert((uint32_t)mProps.size(), &filling, buildmax) != NO_ERROR) {
        ALOGE("%s:could not write header", __func__);  // shouldn't happen
        free(build);
        return INVALID_OPERATION;
    }
    for (auto &prop : *this) {
        if (prop.writeToByteString(&filling, buildmax) != NO_ERROR) {
            free(build);
            // shouldn't happen
            ALOGE("%s:could not write prop %s", __func__, prop.getName());
            return INVALID_OPERATION;
        }
    }

    if (filling != buildmax) {
        ALOGE("%s: problems populating; wrote=%d planned=%d",
                __func__, (int)(filling - build), (int)size);
        free(build);
        return INVALID_OPERATION;
    }
    *pbuffer = build;
    *plength = size;
    return NO_ERROR;
}

status_t mediametrics::Item::readFromByteString(const char *bufferptr, size_t length)
{
    if (bufferptr == nullptr) return BAD_VALUE;

    const char *read = bufferptr;
    const char *readend = bufferptr + length;

    uint32_t size;
    uint32_t header_size;
    uint16_t version;
    uint16_t key_size;
    char *key = nullptr;
    int32_t pid;
    int32_t uid;
    int64_t timestamp;
    uint32_t propCount;
    if (extract(&size, &read, readend) != NO_ERROR
            || extract(&header_size, &read, readend) != NO_ERROR
            || extract(&version, &read, readend) != NO_ERROR
            || extract(&key_size, &read, readend) != NO_ERROR
            || extract(&key, &read, readend) != NO_ERROR
            || extract(&pid, &read, readend) != NO_ERROR
            || extract(&uid, &read, readend) != NO_ERROR
            || extract(&timestamp, &read, readend) != NO_ERROR
            || size > length
            || strlen(key) + 1 != key_size
            || header_size > size) {
        free(key);
        ALOGW("%s: invalid header", __func__);
        return INVALID_OPERATION;
    }
    mKey = key;
    free(key);
    const size_t pos = read - bufferptr;
    if (pos > header_size) {
        ALOGW("%s: invalid header pos:%zu > header_size:%u",
                __func__, pos, header_size);
        return INVALID_OPERATION;
    } else if (pos < header_size) {
        ALOGW("%s: mismatched header pos:%zu < header_size:%u, advancing",
                __func__, pos, header_size);
        read += (header_size - pos);
    }
    if (extract(&propCount, &read, readend) != NO_ERROR) {
        ALOGD("%s: cannot read prop count", __func__);
        return INVALID_OPERATION;
    }
    mPid = pid;
    mUid = uid;
    mTimestamp = timestamp;
    for (size_t i = 0; i < propCount; ++i) {
        Prop prop;
        if (prop.readFromByteString(&read, readend) != NO_ERROR) {
            ALOGW("%s: cannot read prop %zu", __func__, i);
            return INVALID_OPERATION;
        }
        mProps[prop.getName()] = std::move(prop);
    }
    return NO_ERROR;
}

status_t mediametrics::Item::Prop::writeToParcel(Parcel *data) const
{
   switch (mType) {
   case mediametrics::kTypeInt32:
       return data->writeCString(mName.c_str())
               ?: data->writeInt32(mType)
               ?: data->writeInt32(u.int32Value);
   case mediametrics::kTypeInt64:
       return data->writeCString(mName.c_str())
               ?: data->writeInt32(mType)
               ?: data->writeInt64(u.int64Value);
   case mediametrics::kTypeDouble:
       return data->writeCString(mName.c_str())
               ?: data->writeInt32(mType)
               ?: data->writeDouble(u.doubleValue);
   case mediametrics::kTypeRate:
       return data->writeCString(mName.c_str())
               ?: data->writeInt32(mType)
               ?: data->writeInt64(u.rate.first)
               ?: data->writeInt64(u.rate.second);
   case mediametrics::kTypeCString:
       return data->writeCString(mName.c_str())
               ?: data->writeInt32(mType)
               ?: data->writeCString(u.CStringValue);
   default:
       ALOGE("%s: found bad type: %d, name %s", __func__, mType, mName.c_str());
       return BAD_VALUE;
   }
}

status_t mediametrics::Item::Prop::readFromParcel(const Parcel& data)
{
    const char *key = data.readCString();
    if (key == nullptr) return BAD_VALUE;
    int32_t type;
    status_t status = data.readInt32(&type);
    if (status != NO_ERROR) return status;
    switch (type) {
    case mediametrics::kTypeInt32:
        status = data.readInt32(&u.int32Value);
        break;
    case mediametrics::kTypeInt64:
        status = data.readInt64(&u.int64Value);
        break;
    case mediametrics::kTypeDouble:
        status = data.readDouble(&u.doubleValue);
        break;
    case mediametrics::kTypeCString: {
        const char *s = data.readCString();
        if (s == nullptr) return BAD_VALUE;
        set(s);
        break;
        }
    case mediametrics::kTypeRate: {
        std::pair<int64_t, int64_t> rate;
        status = data.readInt64(&rate.first)
                ?: data.readInt64(&rate.second);
        if (status == NO_ERROR) {
            set(rate);
        }
        break;
        }
    default:
        ALOGE("%s: reading bad item type: %d", __func__, mType);
        return BAD_VALUE;
    }
    if (status == NO_ERROR) {
        setName(key);
        mType = (mediametrics::Type)type;
    }
    return status;
}

void mediametrics::Item::Prop::toString(char *buffer, size_t length) const
{
    switch (mType) {
    case mediametrics::kTypeInt32:
        snprintf(buffer, length, "%s=%d:", mName.c_str(), u.int32Value);
        break;
    case mediametrics::kTypeInt64:
        snprintf(buffer, length, "%s=%lld:", mName.c_str(), (long long)u.int64Value);
        break;
    case mediametrics::kTypeDouble:
        snprintf(buffer, length, "%s=%e:", mName.c_str(), u.doubleValue);
        break;
    case mediametrics::kTypeRate:
        snprintf(buffer, length, "%s=%lld/%lld:",
                mName.c_str(), (long long)u.rate.first, (long long)u.rate.second);
        break;
    case mediametrics::kTypeCString:
        // TODO sanitize string for ':' '='
        snprintf(buffer, length, "%s=%s:", mName.c_str(), u.CStringValue);
        break;
    default:
        ALOGE("%s: bad item type: %d for %s", __func__, mType, mName.c_str());
        if (length > 0) buffer[0] = 0;
        break;
    }
}

size_t mediametrics::Item::Prop::getByteStringSize() const
{
    const size_t header =
        sizeof(uint16_t)      // length
        + sizeof(uint8_t)     // type
        + mName.size() + 1;  // mName + 0 termination
    size_t payload = 0;
    switch (mType) {
    case mediametrics::kTypeInt32:
        payload = sizeof(u.int32Value);
        break;
    case mediametrics::kTypeInt64:
        payload = sizeof(u.int64Value);
        break;
    case mediametrics::kTypeDouble:
        payload = sizeof(u.doubleValue);
        break;
    case mediametrics::kTypeRate:
        payload = sizeof(u.rate.first) + sizeof(u.rate.second);
        break;
    case mediametrics::kTypeCString:
        payload = strlen(u.CStringValue) + 1;
        break;
    default:
        ALOGE("%s: found bad prop type: %d, name %s",
                __func__, mType, mName.c_str()); // no payload computed
        break;
    }
    return header + payload;
}


// TODO: fold into a template later.
status_t BaseItem::writeToByteString(
        const char *name, int32_t value, char **bufferpptr, char *bufferptrmax)
{
    const size_t len = 2 + 1 + strlen(name) + 1 + sizeof(value);
    if (len > UINT16_MAX) return BAD_VALUE;
    return insert((uint16_t)len, bufferpptr, bufferptrmax)
            ?: insert((uint8_t)mediametrics::kTypeInt32, bufferpptr, bufferptrmax)
            ?: insert(name, bufferpptr, bufferptrmax)
            ?: insert(value, bufferpptr, bufferptrmax);
}

status_t BaseItem::writeToByteString(
        const char *name, int64_t value, char **bufferpptr, char *bufferptrmax)
{
    const size_t len = 2 + 1 + strlen(name) + 1 + sizeof(value);
    if (len > UINT16_MAX) return BAD_VALUE;
    return insert((uint16_t)len, bufferpptr, bufferptrmax)
            ?: insert((uint8_t)mediametrics::kTypeInt64, bufferpptr, bufferptrmax)
            ?: insert(name, bufferpptr, bufferptrmax)
            ?: insert(value, bufferpptr, bufferptrmax);
}

status_t BaseItem::writeToByteString(
        const char *name, double value, char **bufferpptr, char *bufferptrmax)
{
    const size_t len = 2 + 1 + strlen(name) + 1 + sizeof(value);
    if (len > UINT16_MAX) return BAD_VALUE;
    return insert((uint16_t)len, bufferpptr, bufferptrmax)
            ?: insert((uint8_t)mediametrics::kTypeDouble, bufferpptr, bufferptrmax)
            ?: insert(name, bufferpptr, bufferptrmax)
            ?: insert(value, bufferpptr, bufferptrmax);
}

status_t BaseItem::writeToByteString(
        const char *name, const std::pair<int64_t, int64_t> &value, char **bufferpptr, char *bufferptrmax)
{
    const size_t len = 2 + 1 + strlen(name) + 1 + 8 + 8;
    if (len > UINT16_MAX) return BAD_VALUE;
    return insert((uint16_t)len, bufferpptr, bufferptrmax)
            ?: insert((uint8_t)mediametrics::kTypeRate, bufferpptr, bufferptrmax)
            ?: insert(name, bufferpptr, bufferptrmax)
            ?: insert(value.first, bufferpptr, bufferptrmax)
            ?: insert(value.second, bufferpptr, bufferptrmax);
}

status_t BaseItem::writeToByteString(
        const char *name, char * const &value, char **bufferpptr, char *bufferptrmax)
{
    return writeToByteString(name, (const char *)value, bufferpptr, bufferptrmax);
}

status_t BaseItem::writeToByteString(
        const char *name, const char * const &value, char **bufferpptr, char *bufferptrmax)
{
    const size_t len = 2 + 1 + strlen(name) + 1 + strlen(value) + 1;
    if (len > UINT16_MAX) return BAD_VALUE;
    return insert((uint16_t)len, bufferpptr, bufferptrmax)
            ?: insert((uint8_t)mediametrics::kTypeCString, bufferpptr, bufferptrmax)
            ?: insert(name, bufferpptr, bufferptrmax)
            ?: insert(value, bufferpptr, bufferptrmax);
}


status_t BaseItem::writeToByteString(
        const char *name, const none_t &, char **bufferpptr, char *bufferptrmax)
{
    const size_t len = 2 + 1 + strlen(name) + 1;
    if (len > UINT16_MAX) return BAD_VALUE;
    return insert((uint16_t)len, bufferpptr, bufferptrmax)
            ?: insert((uint8_t)mediametrics::kTypeCString, bufferpptr, bufferptrmax)
            ?: insert(name, bufferpptr, bufferptrmax);
}


status_t mediametrics::Item::Prop::writeToByteString(
        char **bufferpptr, char *bufferptrmax) const
{
    switch (mType) {
    case mediametrics::kTypeInt32:
        return BaseItem::writeToByteString(mName.c_str(), u.int32Value, bufferpptr, bufferptrmax);
    case mediametrics::kTypeInt64:
        return BaseItem::writeToByteString(mName.c_str(), u.int64Value, bufferpptr, bufferptrmax);
    case mediametrics::kTypeDouble:
        return BaseItem::writeToByteString(mName.c_str(), u.doubleValue, bufferpptr, bufferptrmax);
    case mediametrics::kTypeRate:
        return BaseItem::writeToByteString(mName.c_str(), u.rate, bufferpptr, bufferptrmax);
    case mediametrics::kTypeCString:
        return BaseItem::writeToByteString(
                mName.c_str(), u.CStringValue, bufferpptr, bufferptrmax);
    case mediametrics::kTypeNone:
        return BaseItem::writeToByteString(mName.c_str(), none_t{}, bufferpptr, bufferptrmax);
    default:
        ALOGE("%s: found bad prop type: %d, name %s",
                __func__, mType, mName.c_str());  // no payload sent
        return BAD_VALUE;
    }
}

status_t mediametrics::Item::Prop::readFromByteString(
        const char **bufferpptr, const char *bufferptrmax)
{
    uint16_t len;
    char *name;
    uint8_t type;
    status_t status = extract(&len, bufferpptr, bufferptrmax)
            ?: extract(&type, bufferpptr, bufferptrmax)
            ?: extract(&name, bufferpptr, bufferptrmax);
    if (status != NO_ERROR) return status;
    mName = name;
    if (mType == mediametrics::kTypeCString) {
        free(u.CStringValue);
        u.CStringValue = nullptr;
    }
    mType = (mediametrics::Type)type;
    switch (mType) {
    case mediametrics::kTypeInt32:
        return extract(&u.int32Value, bufferpptr, bufferptrmax);
    case mediametrics::kTypeInt64:
        return extract(&u.int64Value, bufferpptr, bufferptrmax);
    case mediametrics::kTypeDouble:
        return extract(&u.doubleValue, bufferpptr, bufferptrmax);
    case mediametrics::kTypeRate:
        return extract(&u.rate.first, bufferpptr, bufferptrmax)
                ?: extract(&u.rate.second, bufferpptr, bufferptrmax);
    case mediametrics::kTypeCString:
        status = extract(&u.CStringValue, bufferpptr, bufferptrmax);
        if (status != NO_ERROR) mType = mediametrics::kTypeNone;
        return status;
    case mediametrics::kTypeNone:
        return NO_ERROR;
    default:
        mType = mediametrics::kTypeNone;
        ALOGE("%s: found bad prop type: %d, name %s",
                __func__, mType, mName.c_str());  // no payload sent
        return BAD_VALUE;
    }
}

} // namespace android::mediametrics
