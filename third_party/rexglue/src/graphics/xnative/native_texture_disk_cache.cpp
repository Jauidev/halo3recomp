#include "native_texture_disk_cache.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <rex/logging.h>

namespace rex::graphics::xnative {
namespace {

constexpr uint32_t kPackMagic = 0x50585447;    // "GTXP"
constexpr uint32_t kIndexMagic = 0x49585447;   // "GTXI"
constexpr uint32_t kRecordMagic = 0x52585447;  // "GTXR"
constexpr uint32_t kLayoutVersion = 1;

struct FileHeader {
  uint32_t magic;
  uint32_t layout;
  uint64_t version;
};
static_assert(sizeof(FileHeader) == 16);

struct RecordHeader {
  uint32_t magic;
  uint32_t size;
  uint64_t key_low;
  uint64_t key_high;
  uint64_t reserved;
};
static_assert(sizeof(RecordHeader) == 32);

struct IndexEntry {
  uint64_t key_low;
  uint64_t key_high;
  uint64_t offset;
  uint64_t size;
};
static_assert(sizeof(IndexEntry) == 32);

// Written records are published by one sync after this long, or once this many bytes wait.
constexpr uint64_t kSyncIntervalMs = 2000;
constexpr uint64_t kSyncBytes = 16ull << 20;
// Stores beyond this much queued data are dropped: the texture is stored when it comes back.
constexpr uint64_t kMaximumQueuedWriteBytes = 64ull << 20;
constexpr size_t kMaximumQueuedReads = 512;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

#if !defined(_WIN32)
bool ReadExact(int fd, void* data, size_t size, uint64_t offset) {
  auto* bytes = static_cast<uint8_t*>(data);
  while (size) {
    const ssize_t done = pread(fd, bytes, size, off_t(offset));
    if (done < 0 && errno == EINTR) continue;
    if (done <= 0) return false;
    bytes += done;
    size -= size_t(done);
    offset += uint64_t(done);
  }
  return true;
}

bool WriteExact(int fd, const void* data, size_t size, uint64_t offset) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size) {
    const ssize_t done = pwrite(fd, bytes, size, off_t(offset));
    if (done < 0 && errno == EINTR) continue;
    if (done <= 0) return false;
    bytes += done;
    size -= size_t(done);
    offset += uint64_t(done);
  }
  return true;
}

uint64_t FileSize(int fd) {
  struct stat status {};
  return fstat(fd, &status) == 0 ? uint64_t(status.st_size) : 0;
}

// The bytes were copied out (reads) or are on the storage (synced writes): the kernel need not
// keep them in the page cache. The phone runs short of memory in game (kswapd at 63 % of a core,
// 4 GB in swap; 2026-10-26), and the pack file reaches hundreds of MB.
void DropPageCache(int fd, uint64_t offset, uint64_t size) {
  posix_fadvise(fd, off_t(offset), off_t(size), POSIX_FADV_DONTNEED);
}
#endif

}  // namespace

std::unique_ptr<NativeTextureDiskCache> NativeTextureDiskCache::Open(const std::string& directory,
                                                                     uint64_t version,
                                                                     uint64_t byte_limit) {
#if defined(_WIN32)
  return nullptr;
#else
  std::unique_ptr<NativeTextureDiskCache> cache(new NativeTextureDiskCache());
  cache->byte_limit_ = byte_limit;
  if (!cache->Load(directory, version)) {
    return nullptr;
  }
  cache->thread_ = std::thread([self = cache.get()] { self->ThreadMain(); });
  return cache;
#endif
}

bool NativeTextureDiskCache::Load(const std::string& directory, uint64_t version) {
#if defined(_WIN32)
  return false;
#else
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  pack_fd_ = open((directory + "/textures.pack").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  index_fd_ = open((directory + "/textures.index").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (pack_fd_ < 0 || index_fd_ < 0) {
    REXLOG_WARN("xport-texture-cache: cannot open {} ({}); textures are converted every time",
                directory, std::strerror(errno));
    return false;
  }
  const FileHeader pack_expected{kPackMagic, kLayoutVersion, version};
  const FileHeader index_expected{kIndexMagic, kLayoutVersion, version};
  FileHeader pack_header{};
  FileHeader index_header{};
  const bool valid = ReadExact(pack_fd_, &pack_header, sizeof(pack_header), 0) &&
                     ReadExact(index_fd_, &index_header, sizeof(index_header), 0) &&
                     !std::memcmp(&pack_header, &pack_expected, sizeof(FileHeader)) &&
                     !std::memcmp(&index_header, &index_expected, sizeof(FileHeader));
  pack_end_ = sizeof(FileHeader);
  index_end_ = sizeof(FileHeader);
  if (valid) {
    const uint64_t pack_size = FileSize(pack_fd_);
    std::vector<IndexEntry> index(size_t((FileSize(index_fd_) - sizeof(FileHeader)) /
                                         sizeof(IndexEntry)));
    if (!index.empty() && !ReadExact(index_fd_, index.data(), index.size() * sizeof(IndexEntry),
                                     sizeof(FileHeader))) {
      index.clear();
    }
    for (const IndexEntry& entry : index) {
      const uint64_t end = entry.offset + sizeof(RecordHeader) + entry.size;
      if (entry.offset < sizeof(FileHeader) || !entry.size || entry.size > UINT32_MAX ||
          end > pack_size) {
        break;
      }
      // A key stored again (its first record could not be read) takes its latest record.
      entries_[{entry.key_low, entry.key_high}] = {entry.offset, uint32_t(entry.size)};
      pack_end_ = std::max(pack_end_, end);
      index_end_ += sizeof(IndexEntry);
    }
    // Drops records written after the last sync (their entries were never written) and a
    // partly written index entry.
    if (ftruncate(pack_fd_, off_t(pack_end_)) || ftruncate(index_fd_, off_t(index_end_))) {
      REXLOG_WARN("xport-texture-cache: cannot trim {} ({})", directory, std::strerror(errno));
      return false;
    }
  } else if (ftruncate(pack_fd_, 0) || ftruncate(index_fd_, 0) ||
             !WriteExact(pack_fd_, &pack_expected, sizeof(FileHeader), 0) ||
             !WriteExact(index_fd_, &index_expected, sizeof(FileHeader), 0)) {
    REXLOG_WARN("xport-texture-cache: cannot create {} ({})", directory, std::strerror(errno));
    return false;
  }
  planned_pack_end_ = pack_end_;
  REXLOG_INFO("xport-texture-cache: {} textures, {} MB of {} MB in {}{}", entries_.size(),
              pack_end_ >> 20, byte_limit_ >> 20, directory, valid ? "" : " (new)");
  return true;
#endif
}

NativeTextureDiskCache::~NativeTextureDiskCache() {
  {
    // Queued stores are dropped (they would delay shutdown); written ones are still published.
    std::lock_guard lock(mutex_);
    stop_ = true;
    for (const PendingWrite& write : writes_) pending_keys_.erase(write.key);
    writes_.clear();
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
#if !defined(_WIN32)
  if (pack_fd_ >= 0) close(pack_fd_);
  if (index_fd_ >= 0) close(index_fd_);
#endif
}

std::shared_ptr<NativeTextureDiskCache::Read> NativeTextureDiskCache::Prefetch(const Key& key,
                                                                              size_t size) {
  std::lock_guard lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end() || found->second.size != size || reads_.size() >= kMaximumQueuedReads) {
    return nullptr;
  }
  auto read = std::make_shared<Read>();
  reads_.push_back({key, found->second, read});
  ++stats_.prefetches;
  wake_.notify_one();
  return read;
}

bool NativeTextureDiskCache::WantsStore(const Key& key, size_t size) {
  std::lock_guard lock(mutex_);
  return !entries_.contains(key) && !pending_keys_.contains(key) &&
         queued_write_bytes_ + size <= kMaximumQueuedWriteBytes &&
         planned_pack_end_ + sizeof(RecordHeader) + size <= byte_limit_;
}

void NativeTextureDiskCache::Store(const Key& key, std::vector<uint8_t> data) {
  std::lock_guard lock(mutex_);
  const uint64_t record_size = sizeof(RecordHeader) + data.size();
  // The same texture converted twice before its first store was published is stored once.
  if (stop_ || data.empty() || data.size() > UINT32_MAX || entries_.contains(key) ||
      pending_keys_.contains(key) || queued_write_bytes_ + data.size() > kMaximumQueuedWriteBytes ||
      planned_pack_end_ + record_size > byte_limit_) {
    return;
  }
  pending_keys_.insert(key);
  queued_write_bytes_ += data.size();
  planned_pack_end_ += record_size;
  writes_.push_back({key, std::move(data)});
  wake_.notify_one();
}

NativeTextureDiskCache::Stats NativeTextureDiskCache::TakeStats() {
  std::lock_guard lock(mutex_);
  Stats stats = stats_;
  stats.records = entries_.size();
  stats.pack_bytes = planned_pack_end_;
  stats_ = {};
  return stats;
}

void NativeTextureDiskCache::ThreadMain() {
#if !defined(_WIN32)
  pthread_setname_np(pthread_self(), "XpTexCache");
#endif
  std::unique_lock lock(mutex_);
  while (true) {
    // Reads first: the recorder may be about to need them.
    if (!reads_.empty()) {
      PendingRead read = std::move(reads_.front());
      reads_.pop_front();
      lock.unlock();
      DoRead(read);
      lock.lock();
      continue;
    }
    const uint64_t now = NowMs();
    if (!unsynced_.empty() && (stop_ || unsynced_bytes_ >= kSyncBytes ||
                               now - first_unsynced_ms_ >= kSyncIntervalMs)) {
      lock.unlock();
      SyncAndPublish();
      lock.lock();
      continue;
    }
    if (!writes_.empty()) {
      PendingWrite write = std::move(writes_.front());
      writes_.pop_front();
      lock.unlock();
      DoWrite(write);
      lock.lock();
      queued_write_bytes_ -= write.data.size();
      continue;
    }
    if (stop_) {
      break;
    }
    if (unsynced_.empty()) {
      wake_.wait(lock);
    } else {
      wake_.wait_for(lock, std::chrono::milliseconds(kSyncIntervalMs - (now - first_unsynced_ms_)));
    }
  }
}

void NativeTextureDiskCache::DoRead(PendingRead& pending) {
#if !defined(_WIN32)
  RecordHeader header{};
  std::vector<uint8_t> data(pending.entry.size);
  const bool read = ReadExact(pack_fd_, &header, sizeof(header), pending.entry.offset) &&
                    header.magic == kRecordMagic && header.size == pending.entry.size &&
                    header.key_low == pending.key.low && header.key_high == pending.key.high &&
                    ReadExact(pack_fd_, data.data(), data.size(),
                              pending.entry.offset + sizeof(header));
  DropPageCache(pack_fd_, pending.entry.offset, sizeof(header) + pending.entry.size);
  {
    std::lock_guard lock(mutex_);
    if (read) {
      stats_.read_bytes += data.size();
    } else {
      // Not read again this session; the next store of the key appends a new record.
      entries_.erase(pending.key);
      REXLOG_WARN("xport-texture-cache: record at {} unreadable, dropped", pending.entry.offset);
    }
  }
  if (read) {
    pending.read->data = std::move(data);
  }
#endif
  pending.read->ready.store(true, std::memory_order_release);
}

void NativeTextureDiskCache::DoWrite(PendingWrite& write) {
#if !defined(_WIN32)
  const RecordHeader header{kRecordMagic, uint32_t(write.data.size()), write.key.low,
                            write.key.high, 0};
  const uint64_t offset = pack_end_;
  if (!WriteExact(pack_fd_, &header, sizeof(header), offset) ||
      !WriteExact(pack_fd_, write.data.data(), write.data.size(), offset + sizeof(header))) {
    // Storage full or failing: stop storing; what is cached still loads.
    REXLOG_WARN("xport-texture-cache: write failed ({}); no more textures are stored",
                std::strerror(errno));
    std::lock_guard lock(mutex_);
    pending_keys_.erase(write.key);
    for (const PendingWrite& queued : writes_) {
      pending_keys_.erase(queued.key);
      queued_write_bytes_ -= queued.data.size();
    }
    writes_.clear();
    byte_limit_ = 0;
    return;
  }
  pack_end_ = offset + sizeof(header) + write.data.size();
  if (unsynced_.empty()) {
    first_unsynced_ms_ = NowMs();
  }
  unsynced_.push_back({write.key, {offset, uint32_t(write.data.size())}});
  unsynced_bytes_ += sizeof(header) + write.data.size();
#endif
}

void NativeTextureDiskCache::SyncAndPublish() {
#if !defined(_WIN32)
  // The records' bytes are on the storage before an index entry names them.
  bool published = fdatasync(pack_fd_) == 0;
  if (published) {
    const uint64_t first = unsynced_.front().second.offset;
    DropPageCache(pack_fd_, first, pack_end_ - first);
    std::vector<IndexEntry> index;
    index.reserve(unsynced_.size());
    for (const auto& [key, entry] : unsynced_) {
      index.push_back({key.low, key.high, entry.offset, entry.size});
    }
    published = WriteExact(index_fd_, index.data(), index.size() * sizeof(IndexEntry), index_end_);
    if (published) {
      index_end_ += index.size() * sizeof(IndexEntry);
    }
  }
  if (!published) {
    REXLOG_WARN("xport-texture-cache: sync failed ({}); {} textures not stored",
                std::strerror(errno), unsynced_.size());
  }
  std::lock_guard lock(mutex_);
  for (const auto& [key, entry] : unsynced_) {
    pending_keys_.erase(key);
    if (published) {
      entries_[key] = entry;
      ++stats_.stores;
      stats_.stored_bytes += entry.size;
    }
  }
#endif
  unsynced_.clear();
  unsynced_bytes_ = 0;
}

}  // namespace rex::graphics::xnative
