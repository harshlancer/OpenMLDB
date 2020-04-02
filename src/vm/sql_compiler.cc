/*
 * sql_compiler.cc
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
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

#include "vm/sql_compiler.h"
#include <memory>
#include <utility>
#include <vector>
#include "codegen/ir_base_builder.h"
#include "glog/logging.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "parser/parser.h"
#include "plan/planner.h"
#include "storage/type_native_fn.h"
#include "udf/udf.h"
#include "vm/transform.h"

namespace fesql {
namespace vm {
using ::fesql::base::Status;

SQLCompiler::SQLCompiler(const std::shared_ptr<Catalog>& cl,
                         ::fesql::node::NodeManager* nm, bool keep_ir)
    : cl_(cl), nm_(nm), keep_ir_(keep_ir) {}

SQLCompiler::~SQLCompiler() {}

void SQLCompiler::KeepIR(SQLContext& ctx, llvm::Module* m) {
    if (m == NULL) {
        LOG(WARNING) << "module is null";
        return;
    }
    ctx.ir.reserve(1024);
    llvm::raw_string_ostream buf(ctx.ir);
    llvm::WriteBitcodeToFile(*m, buf);
    buf.flush();
}

bool SQLCompiler::Compile(SQLContext& ctx, Status& status) {  // NOLINT
    DLOG(INFO) << "start to compile sql " << ctx.sql;
    ::fesql::node::PlanNodeList trees;
    bool ok = Parse(ctx, (*nm_), trees, status);
    if (!ok) {
        return false;
    }

    auto llvm_ctx = ::llvm::make_unique<::llvm::LLVMContext>();
    auto m = ::llvm::make_unique<::llvm::Module>("sql", *llvm_ctx);
    ::fesql::udf::RegisterUDFToModule(m.get());

    if (ctx.is_batch_mode) {
        vm::BatchModeTransformer transformer(nm_, ctx.db, cl_, m.get());
        transformer.AddDefaultPasses();
        if (!transformer.TransformPhysicalPlan(trees, &ctx.plan, status)) {
            LOG(WARNING) << "fail to generate physical plan (batch mode): "
                         << status.msg << " for sql: \n"
                         << ctx.sql;
            return false;
        }
    } else {
        vm::RequestModeransformer transformer(nm_, ctx.db, cl_, m.get());
        transformer.AddDefaultPasses();
        if (!transformer.TransformPhysicalPlan(trees, &ctx.plan, status)) {
            LOG(WARNING) << "fail to generate physical plan (request mode) "
                            "for sql: \n"
                         << ctx.sql;
            return false;
        }
    }

    if (nullptr == ctx.plan) {
        status.msg = "error: generate null physical plan";
        status.code = common::kPlanError;
        LOG(WARNING) << status.msg;
        return false;
    }

    ::llvm::Expected<std::unique_ptr<FeSQLJIT>> jit_expected(
        FeSQLJITBuilder().create());
    {
        ::llvm::Error e = jit_expected.takeError();
        if (e) {
            status.msg = "fail to init jit let";
            status.code = common::kJitError;
            LOG(WARNING) << status.msg;
            return false;
        }
    }
    ctx.jit = std::move(*jit_expected);
    ctx.jit->Init();
    if (false == ctx.jit->OptModule(m.get())) {
        LOG(WARNING) << "fail to opt ir module for sql " << ctx.sql;
        return false;
    }


    m->print(::llvm::errs(), NULL);
    if (keep_ir_) {
        KeepIR(ctx, m.get());
    }

    ::llvm::Error e = ctx.jit->addIRModule(std::move(
        ::llvm::orc::ThreadSafeModule(std::move(m), std::move(llvm_ctx))));
    if (e) {
        LOG(WARNING) << "fail to add ir module  for sql " << ctx.sql;
        return false;
    }

    storage::InitCodecSymbol(ctx.jit.get());
    udf::InitUDFSymbol(ctx.jit.get());

    if (!ResolvePlanFnAddress(ctx.plan, ctx.jit, status)) {
        return false;
    }

    ctx.schema = ctx.plan->output_schema;
    return true;
}

bool SQLCompiler::Parse(SQLContext& ctx, ::fesql::node::NodeManager& node_mgr,
                        ::fesql::node::PlanNodeList& plan_trees,  // NOLINT
                        ::fesql::base::Status& status) {          // NOLINT
    ::fesql::node::NodePointVector parser_trees;
    ::fesql::parser::FeSQLParser parser;
    ::fesql::plan::SimplePlanner planer(&node_mgr, ctx.is_batch_mode);

    int ret = parser.parse(ctx.sql, parser_trees, &node_mgr, status);
    if (ret != 0) {
        LOG(WARNING) << "fail to parse sql " << ctx.sql << " with error "
                     << status.msg;
        return false;
    }

    ret = planer.CreatePlanTree(parser_trees, plan_trees, status);
    if (ret != 0) {
        LOG(WARNING) << "Fail create sql plan: " << status.msg;
        return false;
    }

    return true;
}
bool SQLCompiler::ResolvePlanFnAddress(PhysicalOpNode* node,
                                       std::unique_ptr<FeSQLJIT>& jit,
                                       Status& status) {
    if (nullptr == node) {
        status.msg = "fail to resolve project fn address: node is null";
    }

    if (!node->GetProducers().empty()) {
        for (auto iter = node->GetProducers().cbegin();
             iter != node->GetProducers().cend(); iter++) {
            if (!ResolvePlanFnAddress(*iter, jit, status)) {
                return false;
            }
        }
    }

    if (!node->GetFnName().empty()) {
        ::llvm::Expected<::llvm::JITEvaluatedSymbol> symbol(
            jit->lookup(node->GetFnName()));
        if (symbol.takeError()) {
            LOG(WARNING) << "fail to resolve fn address " << node->GetFnName()
                         << " not found in jit";
        }
        node->SetFn(reinterpret_cast<int8_t*>(symbol->getAddress()));
    }
    return true;
}

}  // namespace vm
}  // namespace fesql
