/*
 * Copyright [2012-2015] DaSE@ECNU
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * /CLAIMS/stmt_handler/select_exec.cpp
 *
 *  Created on: Sep 23, 2015
 *      Author: fzh
 *       Email: fzhedu@gmail.com
 *
 * Description:
 *
 */

#include "../stmt_handler/select_exec.h"
#include <glog/logging.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <stack>
#include <vector>
#include <string>
#include <utility>

#include "../common/error_define.h"
#include "../common/ids.h"
#include "../exec_tracker/stmt_exec_tracker.h"
#include "../Environment.h"
#include "../logical_operator/logical_query_plan_root.h"
#include "../physical_operator/exchange_sender.h"
#include "../physical_operator/exchange_sender_pipeline.h"
#include "../physical_operator/physical_aggregation.h"
#include "../physical_operator/physical_nest_loop_join.h"
#include "../physical_operator/physical_operator_base.h"
#include "../stmt_handler/stmt_handler.h"
using claims::logical_operator::LogicalQueryPlanRoot;
using claims::physical_operator::ExchangeSender;
using claims::physical_operator::ExchangeSenderPipeline;
using claims::physical_operator::PhysicalAggregation;
using claims::physical_operator::PhysicalNestLoopJoin;
using claims::physical_operator::PhysicalOperatorBase;
using claims::physical_operator::PhysicalOperatorType;
using std::endl;
using std::vector;
using std::string;
using std::cout;
using std::make_pair;
using claims::common::rStmtCancelled;

namespace claims {
namespace stmt_handler {
//#define PRINTCONTEXT
SelectExec::SelectExec(AstNode* stmt, string raw_sql)
    : StmtExec(stmt), raw_sql_(raw_sql) {
  select_ast_ = reinterpret_cast<AstSelectStmt*>(stmt_);
}
SelectExec::SelectExec(AstNode* stmt) : StmtExec(stmt), raw_sql_("") {
  select_ast_ = reinterpret_cast<AstSelectStmt*>(stmt_);
}
SelectExec::~SelectExec() {
  //  if (NULL != select_ast_) {
  //    delete select_ast_;
  //    select_ast_ = NULL;
  //  }
  while (!all_segments_.empty()) {
    delete all_segments_.top();
    all_segments_.pop();
  }
}
RetCode SelectExec::Execute(ExecutedResult* exec_result) {
#ifdef PRINTCONTEXT
  select_ast_->Print();
  cout << "--------------begin semantic analysis---------------" << endl;
#endif
  SemanticContext sem_cnxt;
  RetCode ret = rSuccess;
  ret = select_ast_->SemanticAnalisys(&sem_cnxt);
  if (rSuccess != ret) {
    exec_result->error_info_ =
        "semantic analysis error \n" + sem_cnxt.error_msg_;
    exec_result->status_ = false;
    LOG(ERROR) << "semantic analysis error result= : " << ret;
    cout << "semantic analysis error result= : " << ret << endl;
    return ret;
  }
#ifdef PRINTCONTEXT
  select_ast_->Print();
  cout << "--------------begin push down condition ------------" << endl;
#endif
  ret = select_ast_->PushDownCondition(NULL);
  if (rSuccess != ret) {
    exec_result->error_info_ = "push down condition error";
    exec_result->status_ = false;
    exec_result->result_ = NULL;
    ELOG(ret, exec_result->error_info_);
    cout << exec_result->error_info_;
    return ret;
  }
#ifndef PRINTCONTEXT
  select_ast_->Print();
  cout << "--------------begin logical plan -------------------" << endl;
#endif

  LogicalOperator* logic_plan = NULL;
  ret = select_ast_->GetLogicalPlan(logic_plan);
  if (rSuccess != ret) {
    exec_result->error_info_ = "get logical plan error";
    exec_result->status_ = false;
    exec_result->result_ = NULL;
    ELOG(ret, exec_result->error_info_);
    cout << exec_result->error_info_;
    return ret;
  }
  logic_plan = new LogicalQueryPlanRoot(0, logic_plan, raw_sql_,
                                        LogicalQueryPlanRoot::kResultCollector);
  logic_plan->GetPlanContext();
#ifndef PRINTCONTEXT
  logic_plan->Print();
  cout << "--------------begin physical plan -------------------" << endl;
#endif

  PhysicalOperatorBase* physical_plan = logic_plan->GetPhysicalPlan(64 * 1024);
#ifndef PRINTCONTEXT
  physical_plan->Print();
  cout << "--------------begin output result -------------------" << endl;
#endif
  // collect all plan segments
  physical_plan->GetAllSegments(&all_segments_);
  // create thread to send all segments
  pthread_t tid = 0;
  if (all_segments_.size() > 0) {
    int ret = pthread_create(&tid, NULL, SendAllSegments, this);
  }
  SegmentExecStatus* seg_exec_status = new SegmentExecStatus(make_pair(0, 0));

  physical_plan->Open(seg_exec_status);

  while (physical_plan->Next(seg_exec_status, NULL)) {
  }
  exec_result->result_ = physical_plan->GetResultSet();
  physical_plan->Close();

  if (tid != 0) {
    pthread_join(tid, NULL);
  }
  delete logic_plan;
  delete physical_plan;
  return rSuccess;
}

RetCode SelectExec::Execute() {
#ifdef PRINTCONTEXT
  select_ast_->Print();
  cout << "--------------begin semantic analysis---------------" << endl;
#endif
  SemanticContext sem_cnxt;
  RetCode ret = rSuccess;
  ret = select_ast_->SemanticAnalisys(&sem_cnxt);
  if (rSuccess != ret) {
    stmt_exec_status_->set_exec_info("semantic analysis error \n" +
                                     sem_cnxt.error_msg_);
    stmt_exec_status_->set_exec_status(StmtExecStatus::ExecStatus::kError);
    LOG(ERROR) << "semantic analysis error result= : " << ret;
    return ret;
  }
#ifdef PRINTCONTEXT
  select_ast_->Print();
  cout << "--------------begin push down condition ------------" << endl;
#endif
  ret = select_ast_->PushDownCondition(NULL);
  if (rSuccess != ret) {
    stmt_exec_status_->set_exec_info("push down condition error");
    stmt_exec_status_->set_exec_status(StmtExecStatus::ExecStatus::kError);
    stmt_exec_status_->set_query_result(NULL);
    ELOG(ret, stmt_exec_status_->get_exec_info());
    cout << stmt_exec_status_->get_exec_info();
    return ret;
  }
#ifndef PRINTCONTEXT
  select_ast_->Print();
  cout << "--------------begin logical plan -------------------" << endl;
#endif

  LogicalOperator* logic_plan = NULL;
  ret = select_ast_->GetLogicalPlan(logic_plan);
  if (rSuccess != ret) {
    stmt_exec_status_->set_exec_info("get logical plan error");
    stmt_exec_status_->set_exec_status(StmtExecStatus::ExecStatus::kError);
    stmt_exec_status_->set_query_result(NULL);
    ELOG(ret, stmt_exec_status_->get_exec_info());
    cout << stmt_exec_status_->get_exec_info();
    delete logic_plan;
    return ret;
  }
  logic_plan = new LogicalQueryPlanRoot(0, logic_plan, raw_sql_,
                                        LogicalQueryPlanRoot::kResultCollector);
  logic_plan->GetPlanContext();
#ifndef PRINTCONTEXT
  logic_plan->Print();
  cout << "--------------begin physical plan -------------------" << endl;
#endif

  PhysicalOperatorBase* physical_plan = logic_plan->GetPhysicalPlan(64 * 1024);
#ifndef PRINTCONTEXT
  physical_plan->Print();
  cout << "--------------begin output result -------------------" << endl;
#endif
  // collect all plan segments
  physical_plan->GetAllSegments(&all_segments_);
  // create thread to send all segments
  pthread_t tid = 0;
  // add segment_exec_status to stmt_exec_status_
  SegmentExecStatus* seg_exec_status_1 = new SegmentExecStatus(
      make_pair(stmt_exec_status_->get_query_id(),
                Environment::getInstance()->get_slave_node()->get_node_id()));

  stmt_exec_status_->AddSegExecStatus(seg_exec_status_1);

  if (all_segments_.size() > 0) {
    int ret = pthread_create(&tid, NULL, SendAllSegments, this);
  }
  // this segment_exec_status for reporting status
  SegmentExecStatus* seg_exec_status = new SegmentExecStatus(
      make_pair(stmt_exec_status_->get_query_id(),
                Environment::getInstance()->get_slave_node()->get_node_id()),
      Environment::getInstance()->get_slave_node()->GetNodeAddr());
  seg_exec_status->RegisterToTracker();

  physical_plan->Open(seg_exec_status);
  seg_exec_status->UpdateStatus(SegmentExecStatus::ExecStatus::kOk,
                                "physical plan Open() succeed", 0, true);
  while (physical_plan->Next(seg_exec_status, NULL)) {
  }
  seg_exec_status->UpdateStatus(SegmentExecStatus::ExecStatus::kOk,
                                "physical plan next() succeed", 0, true);
  stmt_exec_status_->set_query_result(physical_plan->GetResultSet());
  stmt_exec_status_->set_exec_info("execute a query successfully");
  physical_plan->Close();
  seg_exec_status->UpdateStatus(SegmentExecStatus::ExecStatus::kDone,
                                "physical plan close() succeed", 0, true);
  if (tid != 0) {
    pthread_join(tid, NULL);
  }
  seg_exec_status->UnRegisterFromTracker();

  ret = rSuccess;
  //  if (seg_exec_status->is_cancelled()) {
  //    ret = rStmtCancelled;
  //  }
  delete seg_exec_status;
  delete logic_plan;
  delete physical_plan;
  return ret;
}
//!!!return ret by global variant
void* SelectExec::SendAllSegments(void* arg) {
  RetCode ret = 0;
  SelectExec* select_exec = reinterpret_cast<SelectExec*>(arg);
  short segment_id = 0;
  while (!select_exec->all_segments_.empty()) {
    auto a_plan_segment = select_exec->all_segments_.top();
    // make sure upper exchanges are prepared
    ret = select_exec->IsUpperExchangeRegistered(
        a_plan_segment->upper_node_id_list_, a_plan_segment->exchange_id_);
    if (rSuccess == ret) {
      auto physical_sender_oper = a_plan_segment->get_plan_segment();
      for (int i = 0; i < a_plan_segment->lower_node_id_list_.size(); ++i) {
        // set partition offset for each segment
        reinterpret_cast<ExchangeSender*>(physical_sender_oper)
            ->SetPartitionOffset(i);
        segment_id = select_exec->get_stmt_exec_status()->GenSegmentId();
        if (Environment::getInstance()
                ->get_iterator_executor_master()
                ->ExecuteBlockStreamIteratorsOnSite(
                    physical_sender_oper,
                    a_plan_segment->lower_node_id_list_[i],
                    select_exec->get_stmt_exec_status()->get_query_id(),
                    segment_id) == false) {
          LOG(ERROR) << "send plan error!!" << endl;
          ret = -1;
          return &ret;
        }
        // new SegmentExecStatus and add it to StmtExecStatus
        SegmentExecStatus* seg_exec_status = new SegmentExecStatus(make_pair(
            select_exec->get_stmt_exec_status()->get_query_id(),
            segment_id * kMaxNodeNum + a_plan_segment->lower_node_id_list_[i]));

        select_exec->get_stmt_exec_status()->AddSegExecStatus(seg_exec_status);

        LOG(INFO) << "send plan succeed!!!" << endl;
      }
    } else {
      LOG(ERROR) << "asking upper exchange failed!" << endl;
      return &ret;
    }
    select_exec->all_segments_.pop();
  }
  return &ret;
}
RetCode SelectExec::IsUpperExchangeRegistered(
    vector<NodeID>& upper_node_id_list, const u_int64_t exchange_id) {
  RetCode ret = rSuccess;
  NodeAddress node_addr;
  /// TODO(fzh)should release the strong synchronization
  for (int i = 0; i < upper_node_id_list.size(); ++i) {
    while (Environment::getInstance()
               ->getExchangeTracker()
               ->AskForSocketConnectionInfo(ExchangeID(exchange_id, i),
                                            upper_node_id_list[i],
                                            node_addr) != true) {
      usleep(200);
    }
  }
  return ret;
}

}  // namespace stmt_handler
}  // namespace claims
