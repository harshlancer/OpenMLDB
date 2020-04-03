//
// Created by wangbao on 02/24/20.
//

#include "storage/relational_table.h"
#include "logging.h"
#include "base/hash.h"
#include "base/file_util.h"

using ::baidu::common::INFO;
using ::baidu::common::WARNING;
using ::baidu::common::DEBUG;

DECLARE_bool(disable_wal);
DECLARE_uint32(max_traverse_cnt);

namespace rtidb {
namespace storage {

static rocksdb::Options ssd_option_template;
static rocksdb::Options hdd_option_template;
static bool options_template_initialized = false;

RelationalTable::RelationalTable(const std::string &name, uint32_t id, uint32_t pid,
        ::rtidb::common::StorageMode storage_mode,
        const std::string& db_root_path) :
    storage_mode_(storage_mode), name_(name), id_(id), pid_(pid), idx_cnt_(0),
    last_make_snapshot_time_(0),  
    write_opts_(), offset_(0), db_root_path_(db_root_path){
        if (!options_template_initialized) {
            initOptionTemplate();
        }
        write_opts_.disableWAL = FLAGS_disable_wal;
        db_ = nullptr;
}

RelationalTable::RelationalTable(const ::rtidb::api::TableMeta& table_meta,
        const std::string& db_root_path) :
    storage_mode_(table_meta.storage_mode()), name_(table_meta.name()), id_(table_meta.tid()), pid_(table_meta.pid()),
    is_leader_(false), 
    compress_type_(table_meta.compress_type()), last_make_snapshot_time_(0),   
    write_opts_(), offset_(0), db_root_path_(db_root_path){
    table_meta_.CopyFrom(table_meta);
    if (!options_template_initialized) {
        initOptionTemplate();
    }
    diskused_ = 0;
    write_opts_.disableWAL = FLAGS_disable_wal;
    db_ = nullptr;
}

RelationalTable::~RelationalTable() {
    for (auto handle : cf_hs_) {
        delete handle;
    }
    if (db_ != nullptr) {
        db_->Close();
        delete db_;
    }
}

void RelationalTable::initOptionTemplate() {
    std::shared_ptr<rocksdb::Cache> cache = rocksdb::NewLRUCache(512 << 20, 8); //Can be set by flags
    //SSD options template
    ssd_option_template.max_open_files = -1;
    ssd_option_template.env->SetBackgroundThreads(1, rocksdb::Env::Priority::HIGH); //flush threads
    ssd_option_template.env->SetBackgroundThreads(4, rocksdb::Env::Priority::LOW);  //compaction threads
    ssd_option_template.memtable_prefix_bloom_size_ratio = 0.02;
    ssd_option_template.compaction_style = rocksdb::kCompactionStyleLevel;
    ssd_option_template.level0_file_num_compaction_trigger = 10;
    ssd_option_template.level0_slowdown_writes_trigger = 20;
    ssd_option_template.level0_stop_writes_trigger = 40;
    ssd_option_template.write_buffer_size = 64 << 20;
    ssd_option_template.target_file_size_base = 64 << 20;
    ssd_option_template.max_bytes_for_level_base = 512 << 20;

    rocksdb::BlockBasedTableOptions table_options;
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    table_options.block_cache = cache;
    // table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    table_options.whole_key_filtering = false;
    table_options.block_size = 256 << 10;
    table_options.use_delta_encoding = false;
    ssd_option_template.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    //HDD options template
    hdd_option_template.max_open_files = -1;
    hdd_option_template.env->SetBackgroundThreads(1, rocksdb::Env::Priority::HIGH); //flush threads
    hdd_option_template.env->SetBackgroundThreads(1, rocksdb::Env::Priority::LOW);  //compaction threads
    hdd_option_template.memtable_prefix_bloom_size_ratio = 0.02;
    hdd_option_template.optimize_filters_for_hits = true;
    hdd_option_template.level_compaction_dynamic_level_bytes = true;
    hdd_option_template.max_file_opening_threads = 1; //set to the number of disks on which the db root folder is mounted
    hdd_option_template.compaction_readahead_size = 16 << 20;
    hdd_option_template.new_table_reader_for_compaction_inputs = true;
    hdd_option_template.compaction_style = rocksdb::kCompactionStyleLevel;
    hdd_option_template.level0_file_num_compaction_trigger = 10;
    hdd_option_template.level0_slowdown_writes_trigger = 20;
    hdd_option_template.level0_stop_writes_trigger = 40;
    hdd_option_template.write_buffer_size = 256 << 20;
    hdd_option_template.target_file_size_base = 256 << 20;
    hdd_option_template.max_bytes_for_level_base = 1024 << 20;
    hdd_option_template.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    options_template_initialized = true;
}

bool RelationalTable::InitColumnFamilyDescriptor() {
    cf_ds_.clear();
    cf_ds_.push_back(rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName, 
            rocksdb::ColumnFamilyOptions()));
    const std::vector<std::shared_ptr<IndexDef>>& indexs = table_index_.GetAllIndex();
    for (const auto& index_def : indexs) {
        rocksdb::ColumnFamilyOptions cfo;
        if (storage_mode_ == ::rtidb::common::StorageMode::kSSD) {
            cfo = rocksdb::ColumnFamilyOptions(ssd_option_template);
            options_ = ssd_option_template;
        } else {
            cfo = rocksdb::ColumnFamilyOptions(hdd_option_template);
            options_ = hdd_option_template;
        }
        cf_ds_.push_back(rocksdb::ColumnFamilyDescriptor(index_def->GetName(), cfo));
        PDLOG(DEBUG, "add cf_name %s. tid %u pid %u", index_def->GetName().c_str(), id_, pid_);
    }
    return true;
}

int RelationalTable::InitColumnDesc() {
    if (table_meta_.column_key_size() <= 0) {
        PDLOG(WARNING, "column_key_size is 0, tid %u pid %u", id_, pid_);
        return -1;
    }
    const Schema& schema = table_meta_.column_desc(); 
    uint32_t key_idx = 0;
    for (const auto &column_key : table_meta_.column_key()) {
        const std::string& index_name = column_key.index_name();
        if (table_index_.GetIndex(index_name)) {
            return -1;
        }
        const ::rtidb::type::IndexType index_type = column_key.index_type(); 
        std::map<uint32_t, ::rtidb::common::ColumnDesc> column_idx_map;
        for (int i = 0; i < column_key.col_name_size(); i++) {
            for (int j = 0; j < schema.size(); j++) {
                if (schema.Get(j).name() == column_key.col_name(i)) {
                    column_idx_map.insert(std::make_pair(j, schema.Get(j)));
                }
            }
        }
        if (column_key.flag()) {
            table_index_.AddIndex(std::make_shared<IndexDef>(index_name, key_idx, 
                        ::rtidb::storage::kDeleted, index_type, column_idx_map));
        } else {
            table_index_.AddIndex(std::make_shared<IndexDef>(index_name, key_idx, 
                        ::rtidb::storage::kReady, index_type, column_idx_map));
        }
        key_idx++;
    }
    return 0;
}

bool RelationalTable::InitFromMeta() {
    if (table_meta_.has_mode() && table_meta_.mode() != ::rtidb::api::TableMode::kTableLeader) {
        is_leader_ = false;
    }
    if (InitColumnDesc() < 0) {
        PDLOG(WARNING, "init column desc failed, tid %u pid %u", id_, pid_);
        return false;
    }
    if (table_meta_.has_compress_type()) compress_type_ = table_meta_.compress_type();
    idx_cnt_ = table_index_.Size();
    
    return true;
}

bool RelationalTable::Init() {
    if (!InitFromMeta()) {
        return false;
    }
    InitColumnFamilyDescriptor();
    std::string path = db_root_path_ + "/" + std::to_string(id_) + "_" + std::to_string(pid_) + "/data";
    if (!::rtidb::base::MkdirRecur(path)) {
        PDLOG(WARNING, "fail to create path %s", path.c_str());
        return false;
    }
    options_.create_if_missing = true;
    options_.error_if_exists = true;
    options_.create_missing_column_families = true;
    rocksdb::Status s = rocksdb::DB::Open(options_, path, cf_ds_, &cf_hs_, &db_);
    if (!s.ok()) {
        PDLOG(WARNING, "rocksdb open failed. tid %u pid %u error %s", id_, pid_, s.ToString().c_str());
        return false;
    } 
    PDLOG(INFO, "Open DB. tid %u pid %u ColumnFamilyHandle size %d with data path %s", id_, pid_, idx_cnt_,
            path.c_str());
    return true;
}

bool RelationalTable::Put(const std::string& value) {
    std::string pk = "";
    const Schema& schema = table_meta_.column_desc(); 
    if (table_index_.HasAutoGen()) {
        ::rtidb::base::RowBuilder builder(schema);
        builder.SetBuffer(reinterpret_cast<int8_t*>(const_cast<char*>(&(value[0]))), value.size());
        int64_t auto_gen_pk = id_generator_.Next();
        builder.AppendInt64(auto_gen_pk);
        pk = std::to_string(auto_gen_pk);
    } else {
        ::rtidb::base::RowView view(schema, reinterpret_cast<int8_t*>(const_cast<char*>(&(value[0]))), value.size());
        std::shared_ptr<IndexDef> index_def = table_index_.GetPkIndex();
        for (auto& kv : index_def->GetColumnIdxMap()) {
            uint32_t idx = kv.first;
            ::rtidb::type::DataType data_type = kv.second.data_type();
            if (!GetStr(view, idx, data_type, &pk)) {
                return false;
            }
        }
    }
    return PutDB(pk, value.c_str(), value.size());
}

bool RelationalTable::PutDB(const std::string &pk, const char *data, uint32_t size) {
    rocksdb::WriteBatch batch;
    rocksdb::Status s;
    rocksdb::Slice spk = rocksdb::Slice(pk);
    std::shared_ptr<IndexDef> index_def = table_index_.GetPkIndex();
    batch.Put(cf_hs_[index_def->GetId() + 1], spk, rocksdb::Slice(data, size));

    const Schema& schema = table_meta_.column_desc(); 
    ::rtidb::base::RowView view(schema, reinterpret_cast<int8_t*>(const_cast<char*>(data)), size);
    for (int i = 0; i < (int)(table_index_.Size()); i++) {
        std::shared_ptr<IndexDef> index_def = table_index_.GetIndex(i);
        for (auto& kv : index_def->GetColumnIdxMap()) {
            uint32_t idx = kv.first;
            ::rtidb::type::DataType data_type = kv.second.data_type();
            std::string key = "";
            if (index_def->GetType() == ::rtidb::type::kUnique) {
                if (!GetStr(view, idx, data_type, &key)) {
                    return false;
                }
                rocksdb::Slice unique = rocksdb::Slice(key);
                batch.Put(cf_hs_[index_def->GetId() + 1], unique, spk);
            } else if (index_def->GetType() == ::rtidb::type::kNoUnique) {
                if (!GetStr(view, idx, data_type, &key)) {
                    return false;
                }
                std::string no_unique = CombineNoUniqueAndPk(key, pk);
                batch.Put(cf_hs_[index_def->GetId() + 1], rocksdb::Slice(no_unique), rocksdb::Slice());
            }
            //TODO: combine key
            break;
        }
    }
    s = db_->Write(write_opts_, &batch);
    if (s.ok()) {
        offset_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } else {
        PDLOG(DEBUG, "Put failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
        return false;
    }
}

bool RelationalTable::GetStr(::rtidb::base::RowView& view, uint32_t idx, 
        const ::rtidb::type::DataType& data_type, std::string* key) {
    if (data_type == ::rtidb::type::kSmallInt) {  
        int16_t si_val = 0;
        view.GetInt16(idx, &si_val);
        *key = std::to_string(si_val);
    } else if (data_type == ::rtidb::type::kInt) { 
        int32_t i_val = 0;
        view.GetInt32(idx, &i_val);
        *key = std::to_string(i_val);
    } else if (data_type == ::rtidb::type::kBigInt) { 
        int64_t bi_val = 0;
        view.GetInt64(idx, &bi_val);
        *key = std::to_string(bi_val);
    } else if (data_type == ::rtidb::type::kVarchar) {  
        char* ch = NULL;
        uint32_t length = 0;
        view.GetString(idx, &ch, &length);
        key->assign(ch, length);
    } else {
        //TODO: other data type 
        PDLOG(WARNING, "unsupported data type %s", 
                rtidb::type::DataType_Name(data_type).c_str());
        return false;
    }
    return true;
}

bool RelationalTable::Delete(const std::string& pk, uint32_t idx) {
    rocksdb::WriteBatch batch;
    batch.Delete(cf_hs_[idx+1], rocksdb::Slice(pk));
    rocksdb::Status s = db_->Write(write_opts_, &batch);
    if (s.ok()) {
        offset_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } else {
        PDLOG(DEBUG, "Delete failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
        return false;
    }
}

rocksdb::Iterator* RelationalTable::Seek(uint32_t idx, const std::string& key) {
    rocksdb::ReadOptions ro = rocksdb::ReadOptions();
    const rocksdb::Snapshot* snapshot = db_->GetSnapshot();
    ro.snapshot = snapshot;
    ro.prefix_same_as_start = true;
    ro.pin_data = true;

    rocksdb::Iterator* it = db_->NewIterator(ro, cf_hs_[idx + 1]);
    if (it == NULL) {
        return NULL;
    }
    it->Seek(rocksdb::Slice(key));
    if (!it->Valid()) {
        return NULL;
    }
    return it;
}

bool RelationalTable::Get(const std::string& idx_name, const std::string& key, 
        rtidb::base::Slice& slice) {
    std::shared_ptr<IndexDef> index_def = table_index_.GetIndex(idx_name);
    if (!index_def) return false;
    return Get(index_def, key, slice);     
}

bool RelationalTable::Get(const std::shared_ptr<IndexDef> index_def, const std::string& key, 
        rtidb::base::Slice& slice) {
    uint32_t idx = index_def->GetId();
    ::rtidb::type::IndexType index_type = index_def->GetType(); 

    if (index_type == ::rtidb::type::kPrimaryKey ||
            index_type == ::rtidb::type::kAutoGen) {
        rocksdb::Iterator* it = Seek(idx, key);
        if (it == NULL) {
            delete it;
            return false;
        }
        if (it->key() != key) {
            delete it;
            return false;
        }
        slice = rtidb::base::Slice(it->value().data(), it->value().size());
        delete it;
    } else if (index_type == ::rtidb::type::kUnique) {
        rocksdb::Iterator* it = Seek(idx, key);
        if (it == NULL) {
            delete it;
            return false;
        }
        if (it->key() != key) {
            delete it;
            return false;
        }
        const std::string& pk = std::string(it->value().data(), it->value().size());
        
        rtidb::base::Slice value; 
        Get(table_index_.GetPkIndex(), pk, value); 
        slice = rtidb::base::Slice(value.data(), value.size());
        delete it;
    } else if (index_type == ::rtidb::type::kNoUnique) {
        //TODO multi records
        rocksdb::Iterator* it = Seek(idx, key);
        if (it == NULL) {
            delete it;
            return false;
        }
        std::string pk = "";
        int ret = ParsePk(it->key(), key, &pk);
        if (ret < 0) {
            PDLOG(WARNING,"ParsePk failed, tid %u pid %u, return %d", id_, pid_, ret);
            delete it;
            return false;
        }

        rtidb::base::Slice value; 
        Get(table_index_.GetPkIndex(), pk, value); 
        slice = rtidb::base::Slice(value.data(), value.size());
        delete it;
    }
    return true;
}

bool RelationalTable::Update(const ::rtidb::api::Columns& cd_columns, 
        const ::rtidb::api::Columns& col_columns) {
    const std::string& cd_value = cd_columns.value();
    const std::string& col_value = col_columns.value(); 
    std::map<std::string, int> cd_idx_map;
    Schema condition_schema;
    UpdateInternel(cd_columns, cd_idx_map, condition_schema);
    std::map<std::string, int> col_idx_map;
    Schema value_schema;
    UpdateInternel(col_columns, col_idx_map, value_schema);
    bool ok = UpdateDB(cd_idx_map, col_idx_map, condition_schema, value_schema, 
            cd_value, col_value);
    return ok;
}

void RelationalTable::UpdateInternel(const ::rtidb::api::Columns& cd_columns, 
        std::map<std::string, int>& cd_idx_map, 
        Schema& condition_schema) {
    const Schema& schema = table_meta_.column_desc();
    std::map<std::string, ::rtidb::type::DataType> cd_type_map;
    for (int i = 0; i < cd_columns.name_size(); i++) {
        cd_type_map.insert(std::make_pair(cd_columns.name(i), ::rtidb::type::kBool));
        cd_idx_map.insert(std::make_pair(cd_columns.name(i), i));
    }
    for (int i = 0; i < schema.size(); i++) {
        auto idx_iter = cd_type_map.find(schema.Get(i).name());
        if (idx_iter != cd_type_map.end()) {
            idx_iter->second = schema.Get(i).data_type(); 
        }
    }
    for (int i = 0; i < cd_columns.name_size(); i++) {
        ::rtidb::common::ColumnDesc* col = condition_schema.Add();
        col->set_name(cd_columns.name(i));
        col->set_data_type(cd_type_map.find(cd_columns.name(i))->second);
    }
}

bool RelationalTable::UpdateDB(const std::map<std::string, int>& cd_idx_map, const std::map<std::string, int>& col_idx_map,  
        const Schema& condition_schema, const Schema& value_schema, 
        const std::string& cd_value, const std::string& col_value) {
    const Schema& schema = table_meta_.column_desc();
    uint32_t cd_value_size = cd_value.length();
    ::rtidb::base::RowView cd_view(condition_schema, reinterpret_cast<int8_t*>(const_cast<char*>(&(cd_value[0]))), cd_value_size);
    ::rtidb::type::DataType pk_data_type;
    std::string pk;
    //TODO if condition columns size is more than 1
    for (int i = 0; i < condition_schema.size(); i++) {
        pk_data_type = condition_schema.Get(i).data_type();
        switch(pk_data_type) {
            case ::rtidb::type::kSmallInt:
            { 
                int16_t val1 = 0;
                cd_view.GetInt16(i, &val1);
                pk = std::to_string(val1);
                break;
            }
            case ::rtidb::type::kInt:
            { 
                int32_t val2 = 0;
                cd_view.GetInt32(i, &val2);
                pk = std::to_string(val2);
                break;
            }
            case ::rtidb::type::kBigInt:
            {  
                int64_t val3 = 0;
                cd_view.GetInt64(i, &val3);
                pk = std::to_string(val3);
                break;
            }
            case ::rtidb::type::kVarchar:
            {
                char* ch = NULL;
                uint32_t length = 0;
                cd_view.GetString(i, &ch, &length);
                pk.assign(ch, length);
                break;
            }
            default:
            {
                PDLOG(WARNING, "unsupported data type %s", 
                    rtidb::type::DataType_Name(pk_data_type).c_str());
                return false;
            }
                //TODO: other data type
        }
        break;
    }

    std::lock_guard<std::mutex> lock(mu_);
    rtidb::base::Slice slice;
    std::shared_ptr<IndexDef> index_def = table_index_.GetPkIndex();
    bool ok = Get(index_def, pk, slice);
    if (!ok) {
        PDLOG(WARNING, "get failed, update table tid %u pid %u failed", id_, pid_);
        return false;
    }
    ::rtidb::base::RowView row_view(schema, reinterpret_cast<int8_t*>(const_cast<char*>(slice.data())), slice.size());
    uint32_t col_value_size = col_value.length();
    ::rtidb::base::RowView value_view(value_schema, reinterpret_cast<int8_t*>(const_cast<char*>(&(col_value[0]))), col_value_size);
    uint32_t string_length = 0; 
    for (int i = 0; i < schema.size(); i++) {
        if (schema.Get(i).data_type() == rtidb::type::kVarchar) {
            auto col_iter = col_idx_map.find(schema.Get(i).name());
            if (col_iter != col_idx_map.end()) {
                char* ch = NULL;
                uint32_t length = 0;
                value_view.GetString(col_iter->second, &ch, &length);
                string_length += length;
            } else {
                char* ch = NULL;
                uint32_t length = 0;
                row_view.GetString(i, &ch, &length);
                string_length += length;
            }
        }
    }
    ::rtidb::base::RowBuilder builder(schema);
    uint32_t size = builder.CalTotalLength(string_length);
    std::string row;
    row.resize(size);
    builder.SetBuffer(reinterpret_cast<int8_t*>(&(row[0])), size);
    for (int i = 0; i < schema.size(); i++) {
        auto col_iter = col_idx_map.find(schema.Get(i).name());
        if (col_iter != col_idx_map.end()) {
            if (schema.Get(i).not_null() && value_view.IsNULL(col_iter->second)) {
                PDLOG(WARNING, "not_null is true but value is null ,update table tid %u pid %u failed", id_, pid_);
                return false;
            } else if (value_view.IsNULL(col_iter->second)) {
                builder.AppendNULL(); 
                continue;
            }
        }
        if (schema.Get(i).data_type() == rtidb::type::kBool) {
            bool val = true;
            if (col_iter != col_idx_map.end()) {
                value_view.GetBool(col_iter->second, &val);
            } else {
                row_view.GetBool(i, &val);
            }
            builder.AppendBool(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kSmallInt) {
            int16_t val = 0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetInt16(col_iter->second, &val);
            } else {
                row_view.GetInt16(i, &val);
            }
            builder.AppendInt16(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kInt) {
            int32_t val = 0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetInt32(col_iter->second, &val);
            } else {
                row_view.GetInt32(i, &val);
            }
            builder.AppendInt32(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kBigInt) {
            int64_t val = 0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetInt64(col_iter->second, &val);
            } else {
                row_view.GetInt64(i, &val);
            }
            builder.AppendInt64(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kTimestamp) {
            int64_t val = 0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetTimestamp(col_iter->second, &val);
            } else {
                row_view.GetTimestamp(i, &val);
            }
            builder.AppendTimestamp(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kFloat) {
            float val = 0.0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetFloat(col_iter->second, &val);
            } else {
                row_view.GetFloat(i, &val);
            }
            builder.AppendFloat(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kDouble) {
            double val = 0.0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetDouble(col_iter->second, &val);
            } else {
                row_view.GetDouble(i, &val);
            }
            builder.AppendDouble(val);
        } else if (schema.Get(i).data_type() == rtidb::type::kVarchar) {
            char* ch = NULL;
            uint32_t length = 0;
            if (col_iter != col_idx_map.end()) {
                value_view.GetString(col_iter->second, &ch, &length);
            } else {
                row_view.GetString(i, &ch, &length);
            }
            builder.AppendString(ch, length);
        } else {
            PDLOG(WARNING, "unsupported data type %s", 
                    rtidb::type::DataType_Name(schema.Get(i).data_type()).c_str());
            return false;
        }
    } 
    ok = PutDB(pk, row.c_str(), row.length());
    if (!ok) {
        PDLOG(WARNING, "put failed, update table tid %u pid %u failed", id_, pid_);
        return false;
    }
    return true;
}

RelationalTableTraverseIterator* RelationalTable::NewTraverse(uint32_t idx) {
    if (idx >= idx_cnt_) {
        PDLOG(WARNING, "idx greater than idx_cnt_, failed getting table tid %u pid %u", id_, pid_);
        return NULL;
    }
    rocksdb::ReadOptions ro = rocksdb::ReadOptions();
    const rocksdb::Snapshot* snapshot = db_->GetSnapshot();
    ro.snapshot = snapshot;
    ro.pin_data = true;
    rocksdb::Iterator* it = db_->NewIterator(ro, cf_hs_[idx + 1]);
    return new RelationalTableTraverseIterator(db_, it, snapshot);
}

RelationalTableTraverseIterator::RelationalTableTraverseIterator(rocksdb::DB* db, rocksdb::Iterator* it,
        const rocksdb::Snapshot* snapshot):db_(db), it_(it), snapshot_(snapshot), traverse_cnt_(0) {
}

RelationalTableTraverseIterator::~RelationalTableTraverseIterator() {
    delete it_;
    db_->ReleaseSnapshot(snapshot_);
}

bool RelationalTableTraverseIterator::Valid() {
    return  it_->Valid();
}

void RelationalTableTraverseIterator::Next() {
    traverse_cnt_++;
    it_->Next();
}

void RelationalTableTraverseIterator::SeekToFirst() {
    return it_->SeekToFirst();
}

void RelationalTableTraverseIterator::Seek(const std::string& pk) {
    rocksdb::Slice spk(pk);
    it_->Seek(spk);
}

uint64_t RelationalTableTraverseIterator::GetCount() {
    return traverse_cnt_;
}

rtidb::base::Slice RelationalTableTraverseIterator::GetValue() {
    rocksdb::Slice spk = it_->value();
    return rtidb::base::Slice(spk.data(), spk.size());
}

}
}
