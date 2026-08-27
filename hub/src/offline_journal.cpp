#include "offline_journal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static constexpr uint32_t JOURNAL_MAGIC = 0x42504A31UL; // "BPJ1"
static constexpr const char *JOURNAL_DIR = "/journal";

uint32_t OfflineJournal::crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
        }
    }
    return ~crc;
}

void OfflineJournal::seal(bp_journal_record_t &record) {
    record.magic = JOURNAL_MAGIC;
    record.version = BP_JOURNAL_VERSION;
    record.reserved = 0;
    record.crc32 = 0;
    record.crc32 = crc32(reinterpret_cast<const uint8_t *>(&record),
                         offsetof(bp_journal_record_t, crc32));
}

bool OfflineJournal::valid(const bp_journal_record_t &record) {
    if (record.magic != JOURNAL_MAGIC || record.version != BP_JOURNAL_VERSION
        || record.packet_len < BP_MIN_PACKET_SIZE
        || record.packet_len > BP_MAX_PACKET_SIZE
        || record.source_id == 0) {
        return false;
    }
    uint32_t expected = crc32(reinterpret_cast<const uint8_t *>(&record),
                              offsetof(bp_journal_record_t, crc32));
    return expected == record.crc32;
}

String OfflineJournal::pathFor(uint16_t sourceId) const {
    char path[32];
    snprintf(path, sizeof(path), "%s/%04X.bin", JOURNAL_DIR, sourceId);
    return String(path);
}

void OfflineJournal::noteDevice(uint16_t sourceId) {
    for (uint8_t i = 0; i < deviceCount_; ++i) {
        if (deviceIds_[i] == sourceId) return;
    }
    if (deviceCount_ < BP_JOURNAL_MAX_DEVICES) {
        deviceIds_[deviceCount_++] = sourceId;
    }
}

bool OfflineJournal::begin(fs::FS &filesystem) {
    fs_ = &filesystem;
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    if (!fs_->exists(JOURNAL_DIR) && !fs_->mkdir(JOURNAL_DIR)) return false;

    nextId_ = 1;
    totalRecords_ = 0;
    File dir = fs_->open(JOURNAL_DIR);
    if (!dir || !dir.isDirectory()) return false;
    for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
        String name = file.name();
        if (!name.endsWith(".bin")) continue;
        int slash = name.lastIndexOf('/');
        String stem = name.substring(slash + 1, name.length() - 4);
        uint16_t sourceId = static_cast<uint16_t>(strtoul(stem.c_str(), nullptr, 16));
        if (sourceId == 0) continue;
        noteDevice(sourceId);
        file.seek(0);
        bp_journal_record_t record{};
        while (file.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) == sizeof(record)) {
            if (!valid(record) || record.source_id != sourceId) continue;
            totalRecords_++;
            if (record.local_id >= nextId_) nextId_ = record.local_id + 1;
        }
    }
    dir.close();
    return true;
}

uint32_t OfflineJournal::nextLocalId() {
    if (!mutex_ || xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    uint32_t value = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    xSemaphoreGive(mutex_);
    return value;
}

bool OfflineJournal::append(const bp_journal_record_t &input) {
    if (!fs_ || !mutex_) return false;
    bp_journal_record_t record = input;
    seal(record);
    if (!valid(record)) return false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(250)) != pdTRUE) return false;

    noteDevice(record.source_id);
    String path = pathFor(record.source_id);
    File file = fs_->open(path, fs_->exists(path) ? "r+" : "w+");
    bool ok = false;
    if (file) {
        uint16_t slot = static_cast<uint16_t>(record.local_id % BP_JOURNAL_PER_DEVICE);
        size_t oldSize = file.size();
        size_t offset = static_cast<size_t>(slot) * sizeof(record);
        file.seek(offset);
        ok = file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) == sizeof(record);
        file.flush();
        if (ok && oldSize < BP_JOURNAL_PER_DEVICE * sizeof(record)) totalRecords_++;
        if (totalRecords_ > BP_JOURNAL_MAX_DEVICES * BP_JOURNAL_PER_DEVICE) {
            totalRecords_ = BP_JOURNAL_MAX_DEVICES * BP_JOURNAL_PER_DEVICE;
        }
        file.close();
    }
    xSemaphoreGive(mutex_);
    return ok;
}

bool OfflineJournal::readSlot(uint16_t sourceId, uint16_t slot,
                              bp_journal_record_t &record) const {
    if (!fs_ || slot >= BP_JOURNAL_PER_DEVICE) return false;
    File file = fs_->open(pathFor(sourceId), "r");
    if (!file) return false;
    size_t offset = static_cast<size_t>(slot) * sizeof(record);
    if (!file.seek(offset)) { file.close(); return false; }
    bool ok = file.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) == sizeof(record)
        && valid(record) && record.source_id == sourceId;
    file.close();
    return ok;
}

bool OfflineJournal::locate(uint32_t localId, uint16_t sourceId,
                            uint16_t &slot, bp_journal_record_t &record) const {
    for (uint16_t i = 0; i < BP_JOURNAL_PER_DEVICE; ++i) {
        if (readSlot(sourceId, i, record) && record.local_id == localId) {
            slot = i;
            return true;
        }
    }
    return false;
}

bool OfflineJournal::updateSyncState(uint32_t localId, uint16_t sourceId,
                                     bp_journal_sync_state_t state) {
    if (!fs_ || !mutex_) return false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(250)) != pdTRUE) return false;
    uint16_t slot = 0;
    bp_journal_record_t record{};
    bool found = locate(localId, sourceId, slot, record);
    bool ok = false;
    if (found) {
        record.sync_state = static_cast<uint8_t>(state);
        seal(record);
        File file = fs_->open(pathFor(sourceId), "r+");
        if (file && file.seek(static_cast<size_t>(slot) * sizeof(record))) {
            ok = file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) == sizeof(record);
            file.flush();
        }
        if (file) file.close();
    }
    xSemaphoreGive(mutex_);
    return ok;
}

bool OfflineJournal::latest(uint16_t sourceId, bp_journal_record_t &record) const {
    bool found = false;
    bp_journal_record_t candidate{};
    for (uint16_t slot = 0; slot < BP_JOURNAL_PER_DEVICE; ++slot) {
        if (readSlot(sourceId, slot, candidate)
            && (!found || candidate.local_id > record.local_id)) {
            record = candidate;
            found = true;
        }
    }
    return found;
}

uint16_t OfflineJournal::collectIds(uint16_t *ids, uint16_t capacity) const {
    uint16_t count = deviceCount_ < capacity ? deviceCount_ : capacity;
    memcpy(ids, deviceIds_, count * sizeof(uint16_t));
    return count;
}

uint16_t OfflineJournal::collectLocalIds(uint16_t sourceId, uint32_t *ids,
                                         uint16_t capacity) const {
    uint16_t count = 0;
    bp_journal_record_t record{};
    for (uint16_t slot = 0; slot < BP_JOURNAL_PER_DEVICE && count < capacity; ++slot) {
        if (readSlot(sourceId, slot, record)) ids[count++] = record.local_id;
    }
    for (uint16_t i = 1; i < count; ++i) {
        uint32_t value = ids[i];
        int j = i - 1;
        while (j >= 0 && ids[j] > value) {
            ids[j + 1] = ids[j];
            --j;
        }
        ids[j + 1] = value;
    }
    return count;
}

bool OfflineJournal::find(uint32_t localId, uint16_t sourceId,
                          bp_journal_record_t &record) const {
    uint16_t slot = 0;
    return locate(localId, sourceId, slot, record);
}

uint16_t OfflineJournal::count(uint16_t sourceId) const {
    uint16_t value = 0;
    bp_journal_record_t record{};
    for (uint16_t slot = 0; slot < BP_JOURNAL_PER_DEVICE; ++slot) {
        if (readSlot(sourceId, slot, record)) value++;
    }
    return value;
}

uint16_t OfflineJournal::pendingCount() const {
    uint16_t value = 0;
    bp_journal_record_t record{};
    for (uint8_t d = 0; d < deviceCount_; ++d) {
        for (uint16_t slot = 0; slot < BP_JOURNAL_PER_DEVICE; ++slot) {
            if (readSlot(deviceIds_[d], slot, record)
                && record.sync_state == BP_JOURNAL_PENDING) value++;
        }
    }
    return value;
}

bool OfflineJournal::oldestPending(bp_journal_record_t &record) const {
    bool found = false;
    bp_journal_record_t candidate{};
    for (uint8_t d = 0; d < deviceCount_; ++d) {
        for (uint16_t slot = 0; slot < BP_JOURNAL_PER_DEVICE; ++slot) {
            if (!readSlot(deviceIds_[d], slot, candidate)
                || candidate.sync_state != BP_JOURNAL_PENDING) {
                continue;
            }
            if (!found || candidate.local_id < record.local_id) {
                record = candidate;
                found = true;
            }
        }
    }
    return found;
}

uint16_t OfflineJournal::collectPending(bp_journal_record_t *records,
                                        uint16_t capacity) const {
    if (!records || capacity == 0) return 0;
    uint16_t count = 0;
    bp_journal_record_t candidate{};
    for (uint8_t d = 0; d < deviceCount_; ++d) {
        for (uint16_t slot = 0; slot < BP_JOURNAL_PER_DEVICE; ++slot) {
            if (!readSlot(deviceIds_[d], slot, candidate)
                || candidate.sync_state != BP_JOURNAL_PENDING) continue;
            if (count < capacity) {
                records[count++] = candidate;
            } else if (candidate.local_id < records[count - 1].local_id) {
                records[count - 1] = candidate;
            }
            // Keep the bounded result oldest-first as entries arrive.
            for (int i = count - 1; i > 0
                 && records[i].local_id < records[i - 1].local_id; --i) {
                bp_journal_record_t swap = records[i - 1];
                records[i - 1] = records[i];
                records[i] = swap;
            }
        }
    }
    return count;
}
