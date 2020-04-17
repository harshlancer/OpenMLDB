//
// Copyright (C) 2020 4paradigm.com
// Author kongquan
// Date 2020-04-14

#include "storage/object_store.h"

namespace rtidb {
namespace storage {
ObjectStore::ObjectStore(const ::rtidb::api::TableMeta& table_meta,
                         const std::string& db_root_path)
    : db_(NULL),
      tid_(table_meta.tid()),
      pid_(table_meta.pid()),
      name_(table_meta.name()),
      db_root_path_(db_root_path),
      is_leader_(false),
      storage_mode_(table_meta.storage_mode()) {}

bool ObjectStore::Init() {
    db_root_path_ = db_root_path_ + "/" + std::to_string(tid_) + "_" +
                    std::to_string(pid_) + "/data";
    char* path = const_cast<char*>(db_root_path_.data());
    db_ = hs_open(path, 1, 0, 16);
    if (db_ != NULL) {
        return true;
    }
    return false;
}

ObjectStore::~ObjectStore() {
    DoFlash();
    hs_close(db_);
}

bool ObjectStore::Store(const std::string& key, const std::string& value) {
    char* hs_key = const_cast<char*>(key.data());
    char* hs_value = const_cast<char*>(value.data());
    return hs_set(db_, hs_key, hs_value, value.length(), 0, 0);
}

rtidb::base::Slice ObjectStore::Get(const std::string& key) {
    char* hs_key = const_cast<char*>(key.data());
    uint32_t vlen = 0, flag;
    char* ch = hs_get(db_, hs_key, &vlen, &flag);
    if (ch == NULL) {
        return rtidb::base::Slice();
    }
    return rtidb::base::Slice(ch, vlen);
}

void ObjectStore::DoFlash() { hs_flush(db_, 1024, 60 * 10); }

::rtidb::common::StorageMode ObjectStore::GetStorageMode() const {
    return storage_mode_;
}

}  // namespace storage
}  // namespace rtidb
