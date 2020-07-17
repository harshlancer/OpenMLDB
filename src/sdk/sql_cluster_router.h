/*
 * sql_cluster_router.h
 * Copyright (C) 4paradigm.com 2020 wangtaize <wangtaize@4paradigm.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_SDK_SQL_CLUSTER_ROUTER_H_
#define SRC_SDK_SQL_CLUSTER_ROUTER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/random.h"
#include "base/spinlock.h"
#include "catalog/schema_adapter.h"
#include "client/tablet_client.h"
#include "parser/parser.h"
#include "sdk/cluster_sdk.h"
#include "sdk/sql_router.h"
#include "vm/engine.h"

namespace rtidb {
namespace sdk {

struct RouterCache {
    RouterCache(std::shared_ptr<::rtidb::nameserver::TableInfo> table_info,
                DefaultValueMap default_map, uint32_t str_length)
        : table_info(table_info),
          default_map(default_map),
          column_schema(),
          str_length(str_length) {}

    explicit RouterCache(std::shared_ptr<::fesql::sdk::Schema> column_schema)
        : table_info(),
          default_map(),
          column_schema(column_schema),
          str_length(0) {}

    std::shared_ptr<::rtidb::nameserver::TableInfo> table_info;
    DefaultValueMap default_map;
    std::shared_ptr<::fesql::sdk::Schema> column_schema;
    uint32_t str_length;
};

class SQLClusterRouter : public SQLRouter {
 public:
    explicit SQLClusterRouter(const SQLRouterOptions& options);
    explicit SQLClusterRouter(ClusterSDK* sdk);

    ~SQLClusterRouter();

    bool Init();

    bool CreateDB(const std::string& db, fesql::sdk::Status* status);

    bool DropDB(const std::string& db, fesql::sdk::Status* status);

    bool ExecuteDDL(const std::string& db, const std::string& sql,
                    fesql::sdk::Status* status);

    bool ExecuteInsert(const std::string& db, const std::string& sql,
                       ::fesql::sdk::Status* status);

    bool ExecuteInsert(const std::string& db, const std::string& sql,
                       std::shared_ptr<SQLInsertRow> row,
                       fesql::sdk::Status* status);

    bool ExecuteInsert(const std::string& db, const std::string& sql,
                       std::shared_ptr<SQLInsertRows> rows,
                       fesql::sdk::Status* status);

    std::shared_ptr<ExplainInfo> Explain(const std::string& db,
                                         const std::string& sql,
                                         ::fesql::sdk::Status* status);

    std::shared_ptr<SQLRequestRow> GetRequestRow(const std::string& db,
                                                 const std::string& sql,
                                                 ::fesql::sdk::Status* status);

    std::shared_ptr<SQLInsertRow> GetInsertRow(const std::string& db,
                                               const std::string& sql,
                                               ::fesql::sdk::Status* status);

    std::shared_ptr<SQLInsertRows> GetInsertRows(const std::string& db,
                                                 const std::string& sql,
                                                 ::fesql::sdk::Status* status);

    std::shared_ptr<fesql::sdk::ResultSet> ExecuteSQL(
        const std::string& db, const std::string& sql,
        std::shared_ptr<SQLRequestRow> row, fesql::sdk::Status* status);

    std::shared_ptr<::fesql::sdk::ResultSet> ExecuteSQL(
        const std::string& db, const std::string& sql,
        ::fesql::sdk::Status* status);

    bool RefreshCatalog();

 private:
    bool GetTablet(
        const std::string& db, const std::string& sql,
        std::vector<std::shared_ptr<::rtidb::client::TabletClient>>* tablets);

    void GetTables(::fesql::vm::PhysicalOpNode* node,
                   std::set<std::string>* tables);

    std::shared_ptr<RouterCache> GetCache(const std::string& db,
                                          const std::string& sql);

    void SetCache(const std::string& db, const std::string& sql,
                  std::shared_ptr<RouterCache> router_cache);

    bool GetSQLPlan(const std::string& sql, ::fesql::node::NodeManager* nm,
                    ::fesql::node::PlanNodeList* plan);

    bool GetInsertInfo(
        const std::string& db, const std::string& sql,
        ::fesql::sdk::Status* status,
        std::shared_ptr<::rtidb::nameserver::TableInfo>* table_info,
        DefaultValueMap* default_map, uint32_t* str_length);

    std::shared_ptr<fesql::node::ConstNode> GetDefaultMapValue(
        const fesql::node::ConstNode& node, rtidb::type::DataType column_type);

    DefaultValueMap GetDefaultMap(
        std::shared_ptr<::rtidb::nameserver::TableInfo> table_info,
        ::fesql::node::ExprListNode* row, uint32_t* str_length);

 private:
    SQLRouterOptions options_;
    ClusterSDK* cluster_sdk_;
    ::fesql::vm::Engine* engine_;
    // TODO(wangtaize) add update strategy
    std::map<std::string, std::map<std::string, std::shared_ptr<RouterCache>>>
        input_cache_;
    ::rtidb::base::SpinMutex mu_;
    ::rtidb::base::Random rand_;
};

}  // namespace sdk
}  // namespace rtidb
#endif  // SRC_SDK_SQL_CLUSTER_ROUTER_H_