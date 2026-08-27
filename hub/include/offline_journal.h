#ifndef BLUEPAWS_OFFLINE_JOURNAL_H
#define BLUEPAWS_OFFLINE_JOURNAL_H

#include <Arduino.h>
#include <FS.h>
#include <bp_protocol.h>

// Fixed-size, crash-tolerant LittleFS journal. Each collar owns a 100-slot
// circular file, so a noisy collar cannot evict another collar's history.
// A record is trusted only when its magic/version/length and CRC32 all match.

static constexpr uint8_t BP_JOURNAL_VERSION = 1;
static constexpr uint16_t BP_JOURNAL_PER_DEVICE = 100;
static constexpr uint8_t BP_JOURNAL_MAX_DEVICES = 16;

enum bp_journal_sync_state_t : uint8_t {
    BP_JOURNAL_PENDING = 0,
    BP_JOURNAL_VALIDATED = 1,
    BP_JOURNAL_REJECTED = 2,
};

#pragma pack(push, 1)
struct bp_journal_record_t {
    uint32_t magic;
    uint8_t version;
    uint8_t packet_len;
    uint8_t sync_state;
    uint8_t reserved;
    uint32_t local_id;
    uint16_t source_id;
    uint32_t gateway_rx_time_unix;
    int16_t rssi_dbm;
    int16_t snr_x10;
    uint8_t packet[BP_MAX_PACKET_SIZE];
    uint32_t crc32;
};
#pragma pack(pop)

class OfflineJournal {
public:
    bool begin(fs::FS &filesystem);
    uint32_t nextLocalId();
    bool append(const bp_journal_record_t &record);
    bool updateSyncState(uint32_t localId, uint16_t sourceId,
                         bp_journal_sync_state_t state);
    bool readSlot(uint16_t sourceId, uint16_t slot,
                  bp_journal_record_t &record) const;
    bool latest(uint16_t sourceId, bp_journal_record_t &record) const;
    uint16_t collectIds(uint16_t *ids, uint16_t capacity) const;
    uint16_t collectLocalIds(uint16_t sourceId, uint32_t *ids,
                             uint16_t capacity) const;
    bool find(uint32_t localId, uint16_t sourceId,
              bp_journal_record_t &record) const;
    uint16_t count(uint16_t sourceId) const;
    uint16_t pendingCount() const;
    bool oldestPending(bp_journal_record_t &record) const;
    uint16_t collectPending(bp_journal_record_t *records, uint16_t capacity) const;
    uint32_t totalValidRecords() const { return totalRecords_; }
    uint32_t nextIdValue() const { return nextId_; }
    static bool valid(const bp_journal_record_t &record);
    static void seal(bp_journal_record_t &record);

private:
    fs::FS *fs_ = nullptr;
    uint16_t deviceIds_[BP_JOURNAL_MAX_DEVICES] = {};
    uint8_t deviceCount_ = 0;
    uint32_t nextId_ = 1;
    uint32_t totalRecords_ = 0;
    SemaphoreHandle_t mutex_ = nullptr;

    String pathFor(uint16_t sourceId) const;
    void noteDevice(uint16_t sourceId);
    bool locate(uint32_t localId, uint16_t sourceId,
                uint16_t &slot, bp_journal_record_t &record) const;
    static uint32_t crc32(const uint8_t *data, size_t length);
};

#endif
