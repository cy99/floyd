#ifndef FLOYD_FILE_LOG_H_
#define FLOYD_FILE_LOG_H_

#include <string>
#include <map>
#include "floyd/src/floyd.pb.h"

#include "slash/include/env.h"
#include "slash/include/slash_slice.h"
#include "slash/include/slash_status.h"
#include "slash/include/slash_mutex.h"

using slash::Status;
using slash::Slice;

namespace floyd {

class Logger;
class Iterator;
class Manifest;
class LogFile;

// TODO(anan) 
//    1. we don't remove log files
class Log {
 public:
  Log(const std::string& path, Logger* info_log);
  ~Log();

  std::pair<uint64_t, uint64_t> Append(std::vector<Entry*>& entries);
  //void TruncatePrefix(uint64_t first_index) { first_index = 0; assert(false); }
  bool TruncateSuffix(uint64_t last_index);

  bool GetEntry(uint64_t index, Entry* entry);
  uint64_t GetStartLogIndex();
  uint64_t GetLastLogIndex();
  bool GetLastLogTermAndIndex(uint64_t* last_log_term, uint64_t* last_log_index);

  void UpdateMetadata(uint64_t current_term, std::string voted_for_ip,
                      int32_t voted_for_port, uint64_t apply_index);

  uint64_t current_term();
  std::string voted_for_ip();
  uint32_t voted_for_port(); 

  uint64_t apply_index();
  void set_apply_index(uint64_t apply_index);

 private:
  std::string path_;
  Logger* info_log_;
  Manifest *manifest_;
  LogFile *last_table_;
  int cache_size_;

  slash::Mutex mu_;
  //pthread_rwlock_t rw_;
  std::map<std::string, LogFile*> files_;

  bool Recover();
  bool GetLogFile(const std::string &file, LogFile** LogFile);
  void SplitIfNeeded();

  bool TruncateLastLogFile();

  // No copying allowed
  Log(const Log&);
  void operator=(const Log&);
};

const size_t kIdLength = sizeof(uint64_t);
const size_t kOffsetLength = sizeof(uint64_t);
const size_t kLogFileHeaderLength = 2 * kIdLength + kOffsetLength;
//const size_t kManifestMetaLength = 4 * kIdLength + 2 * sizeof(uint32_t);

//
// Manifest contains Meta
// 
class Manifest {
 public:
  struct Meta {
    // Log needed
    uint64_t file_num;
    uint64_t entry_start;
    uint64_t entry_end;

    // Raft needed
    uint64_t current_term;
    uint32_t voted_for_ip;
    uint32_t voted_for_port;
    uint64_t apply_index;
    
    Meta()
      : file_num(0LL), entry_start(1LL), entry_end(0LL), 
        current_term(1), voted_for_ip(0), voted_for_port(0),
        apply_index(0LL) { }
  };

  explicit Manifest(slash::RandomRWFile* file)
      : file_(file) { }

  bool Recover();
  void Update(uint64_t entry_start, uint64_t entry_end);
  bool Save();
  
  void Dump();
  std::string ToString();

  slash::RandomRWFile *file_;
  Meta meta_;

 private:
  char scratch[256];

  // No copying allowed
  Manifest(const Manifest&);
  void operator=(const Manifest&);
};

//
// LogFile structure:
//    Header :  | entry_start(uint64)  |  entry_end(uint64)  | EOF offset(int32) |
//    Body   :  | Entry i |  Entry i+1 | ... |
// Entry structure:
//    | entry_id(uint64) | length(int32) | pb format msg(length bytes) | begin_offset(int32) |
//
class LogFile {
 public:
  struct Header {
    uint64_t entry_start;
    uint64_t entry_end;
    uint64_t filesize;

    Header() : entry_start(1), entry_end(0), filesize(kLogFileHeaderLength) {}
  };

  struct Message {
    uint64_t entry_id;
    int32_t length;
    const char *pb;
    int32_t begin_offset;
  };

  //static bool Open(slash::RandomRWFile* file, LogFile** LogFile);
  static bool Open(const std::string &filename, LogFile** LogFile);
  static bool ReadHeader(slash::RandomRWFile* file, Header *header);

  int ReadMessage(int offset, Message *msg, bool from_end = false);
  int AppendEntry(uint64_t index, Entry& entry);

  void TruncateEntry(uint64_t index, int offset) {
    header_->entry_end = index - 1;
    header_->filesize = offset;
  }

  bool GetEntry(uint64_t index, Entry* entry);
  bool Sync();

  Iterator* NewIterator();
  ~LogFile() {
    if (file_ != NULL) {
      //file_->Sync();
      Sync();
    }
    delete file_;

    if (backing_store_ != NULL)
      delete backing_store_;
    delete header_;
  }

  //slash::RandomRWFile *file() { return file_; }

  // TODO maybe need atomic
  Header *header_;
  slash::RandomRWFile *file_;

 private:
  LogFile(slash::RandomRWFile* file, Header *header)
      : header_(header),
        file_(file),
        backing_store_(NULL) {}

  int Serialize(uint64_t index, int length, Entry &entry, Slice *result, char *scratch);


  char scratch[1024 * 4];
  char *backing_store_;

  // No copying allowed
  LogFile(const LogFile&);
  void operator=(const LogFile&);
};

// Single LogFile Iterator
class Iterator {
 public:
  Iterator(LogFile *LogFile)
      : table_(LogFile),
      file_(table_->file_),
      //header_(header),
      offset_(0), valid_(false) {} 

  ~Iterator() {}

  bool Valid() {
    return valid_;
  }
  void SeekToFirst() {
    offset_ = kLogFileHeaderLength;
    valid_ =  offset_ < table_->header_->filesize ? true : false;
    Next();
  }
  
  void SeekToLast() {
    offset_ = table_->header_->filesize;
    valid_ =  offset_ > kLogFileHeaderLength ? true : false;
    Prev();
  }

  void Next() {
    if (!valid_ || offset_ >= table_->header_->filesize) {
      valid_ = false;
      return;
    }

    int nread = table_->ReadMessage(offset_, &msg);
    if (nread <= 0) {
      valid_ = false;
    } else {
      offset_ += nread;
    }
  }

  void Prev() {
    if (!valid_ || offset_ - kOffsetLength <= kLogFileHeaderLength) {
      valid_ = false;
      return;
    }

    int nread = table_->ReadMessage(offset_, &msg, true);
    if (nread <= 0) {
      valid_ = false;
    } else {
      offset_ -= nread;
    }
  }

  void TruncateEntry() {
    table_->TruncateEntry(msg.entry_id, offset_);
  }

  LogFile::Message msg;

private:

  LogFile *table_;
  slash::RandomRWFile *file_;
  //LogFile::Header* header_;
  uint64_t offset_;
  bool valid_;

  // No copying allowed
  Iterator(const Iterator&);
  void operator=(const Iterator&);
};

} // namespace floyd
#endif
