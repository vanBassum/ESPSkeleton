#pragma once

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <cstddef>
#include <cstdint>

// Writes a byte stream into ONE flash partition, addressed by label, hiding the
// app-vs-data branch behind a single write/finish surface.
//
//   • App  partition -> esp_ota_* (image validation, set-boot, sequential
//                       writes that erase sectors internally as needed).
//   • Data partition -> raw esp_partition_write, erasing each flash sector
//                       just before the first write that lands in it (no
//                       up-front full-partition erase).
//
// Setup happens in the constructor; ok() reports whether it succeeded. write()
// appends sequentially; finish() commits (validate + set-boot for an app image,
// no-op for data). The destructor aborts a half-written app image if finish()
// was never reached. One upload is one instance — no shared state, no mutex.
class PartitionWriter
{
    static constexpr const char* TAG = "PartitionWriter";

public:
    // Setup: find the partition, open the OTA handle (app) or arm the offsets
    // (data). On failure *err points at a static reason string and ok() is false.
    PartitionWriter(const char* label, const char** err);
    ~PartitionWriter();

    PartitionWriter(const PartitionWriter&)            = delete;
    PartitionWriter& operator=(const PartitionWriter&) = delete;

    bool   ok() const { return p_ != nullptr; }
    size_t written() const { return written_; }

    // Append `size` bytes at the current offset. False on flash error/overflow.
    bool write(const void* data, size_t size);

    // End-of-stream. App: validate image + set bootable. Data: no-op.
    // Returns nullptr on success or a static error string.
    const char* finish();

private:
    const esp_partition_t* p_        = nullptr;   // nullptr == setup failed
    bool                   isApp_    = false;
    bool                   began_    = false;     // app OTA handle open & not yet ended
    esp_ota_handle_t       otaHandle_ = 0;
    size_t                 written_  = 0;
    size_t                 erasedTo_ = 0;         // data only: bytes erased so far (sector multiple)
};
