#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rex::graphics::xnative {

// DXT textures transcoded to ETC2 on the GPU (texture_detile_cs.glsl), kept in the
// phone's storage. The title streams the same textures in and out as the player moves, and each time a
// compute pass converted every block again; a texture converted once is now read back from the
// cache and uploaded as it is.
// Files in the cache directory:
//   textures.pack:  records appended one after another (RecordHeader, then the converted bytes);
//   textures.index: one IndexEntry per record, written only after its record's bytes are synced,
//                   so an entry never names bytes that a crash lost.
// Both start with a FileHeader whose version covers this layout and the conversion shader; any
// mismatch starts the cache over. One thread does all the file work: reads asked for when a texture
// is captured (Prefetch) are usually done before the recorder needs them (if not, the texture is
// converted on the GPU as before), and stores come from the recorder once the GPU has completed the
// frame that converted them.
class NativeTextureDiskCache {
 public:
  struct Key {
    uint64_t low = 0;
    uint64_t high = 0;
    bool operator==(const Key&) const = default;
    explicit operator bool() const { return low || high; }
  };

  // A record being read. data is complete once ready is set (release); it stays empty when the
  // record could not be read.
  struct Read {
    std::atomic<bool> ready{false};
    std::vector<uint8_t> data;
  };

  // Null when the directory or its files cannot be used. byte_limit bounds the pack file.
  static std::unique_ptr<NativeTextureDiskCache> Open(const std::string& directory,
                                                      uint64_t version, uint64_t byte_limit);
  ~NativeTextureDiskCache();

  // Starts reading key's record when the cache has one of `size` bytes; null otherwise.
  std::shared_ptr<Read> Prefetch(const Key& key, size_t size);
  // Whether storing `size` bytes for key would be kept: not cached or queued yet, room left.
  bool WantsStore(const Key& key, size_t size);
  void Store(const Key& key, std::vector<uint8_t> data);

  // Counters since the last call (for the periodic log line).
  struct Stats {
    uint64_t prefetches = 0;
    uint64_t read_bytes = 0;
    uint64_t stores = 0;
    uint64_t stored_bytes = 0;
    uint64_t records = 0;
    uint64_t pack_bytes = 0;
  };
  Stats TakeStats();

 private:
  struct KeyHash {
    size_t operator()(const Key& key) const { return size_t(key.low); }
  };
  struct Entry {
    uint64_t offset = 0;  // of the record header
    uint32_t size = 0;    // converted bytes after the header
  };
  struct PendingRead {
    Key key;
    Entry entry;
    std::shared_ptr<Read> read;
  };
  struct PendingWrite {
    Key key;
    std::vector<uint8_t> data;
  };

  NativeTextureDiskCache() = default;
  bool Load(const std::string& directory, uint64_t version);
  void ThreadMain();
  void DoRead(PendingRead& read);
  void DoWrite(PendingWrite& write);
  void SyncAndPublish();

  int pack_fd_ = -1;
  int index_fd_ = -1;
  uint64_t byte_limit_ = 0;

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  // Records whose index entries are written (guarded by mutex_).
  std::unordered_map<Key, Entry, KeyHash> entries_;
  // Stores queued or written but not published yet (guarded by mutex_).
  std::unordered_set<Key, KeyHash> pending_keys_;
  std::deque<PendingRead> reads_;
  std::deque<PendingWrite> writes_;
  uint64_t queued_write_bytes_ = 0;
  // End of the pack file as the writes will leave it (guarded by mutex_).
  uint64_t planned_pack_end_ = 0;
  Stats stats_{};

  // Cache thread only (and Load): where the next record and index entry go, and records not
  // synced yet.
  uint64_t pack_end_ = 0;
  uint64_t index_end_ = 0;
  std::vector<std::pair<Key, Entry>> unsynced_;
  uint64_t unsynced_bytes_ = 0;
  uint64_t first_unsynced_ms_ = 0;

  std::thread thread_;
};

}  // namespace rex::graphics::xnative
