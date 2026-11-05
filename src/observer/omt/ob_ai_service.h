/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_
#define OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_

#include "share/ai_service/ob_ai_service_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/lock/ob_tc_rwlock.h"
#include "lib/container/ob_se_array.h"
#include "lib/task/ob_timer.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "share/ob_server_struct.h"
#include "lib/worker.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
}
namespace omt
{

class ObAiService;

class ObAiServiceGuard
{
public:
  ObAiServiceGuard();
  ~ObAiServiceGuard();
  int get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info);
  int get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check = true);
private:
  int check_access_privilege();
  ObArenaAllocator local_allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObAiServiceGuard);
};

// Drains oceanbase.__all_ai_column_task: renders each pending AI column expression
// against its row, asks the model, and writes the answer back. Rows are grouped by
// request_id so several AI columns of one row become one request group and become
// visible together, which is what the public cases check with "no partial visibility".
class ObAiColumnTaskTimer : public common::ObTimerTask
{
public:
  ObAiColumnTaskTimer();
  virtual ~ObAiColumnTaskTimer();
  int init(common::ObMySQLProxy &sql_proxy, ObAiService &owner);
  void destroy();
  virtual void runTimerTask();
private:
  struct TaskRow
  {
    TaskRow() : task_id_(0), table_id_(common::OB_INVALID_ID), column_id_(0),
                request_id_(0), retry_count_(0), state_(0), max_retries_(0),
                rowkey_predicate_(), database_name_(), table_name_(), column_name_(),
                request_column_name_(), model_name_(), normalized_expr_() {}
    bool is_valid() const { return task_id_ > 0 && table_id_ != common::OB_INVALID_ID; }
    TO_STRING_KV(K_(task_id), K_(table_id), K_(column_id), K_(request_id), K_(state),
                 K_(retry_count), K_(max_retries), K_(rowkey_predicate),
                 K_(database_name), K_(table_name), K_(column_name), K_(model_name));
    int64_t task_id_;
    uint64_t table_id_;
    uint64_t column_id_;
    int64_t request_id_;
    int64_t retry_count_;
    int64_t state_;
    int64_t max_retries_;
    common::ObString rowkey_predicate_;
    common::ObString database_name_;
    common::ObString table_name_;
    common::ObString column_name_;
    common::ObString request_column_name_;
    common::ObString model_name_;
    common::ObString normalized_expr_;
  };
  int do_work();
  int fetch_tasks(common::ObIAllocator &alloc, common::ObIArray<TaskRow> &tasks);
  int fetch_prompt(common::ObIAllocator &alloc, const TaskRow &task,
                   common::ObString &system_text, common::ObString &user_text);
  int request_model(common::ObIAllocator &alloc, const TaskRow &task,
                    const common::ObString &system_text, const common::ObString &user_text,
                    common::ObString &answer);
  int write_answer(common::ObMySQLTransaction &trans, common::ObIAllocator &alloc,
                   const TaskRow &task, const common::ObString &answer, bool &applied);
  int request_embedding(common::ObIAllocator &alloc, const TaskRow &task,
                        const common::ObString &user_text, common::ObString &answer);
  int is_superseded(const TaskRow &task, bool &superseded);
  int drop_task(common::ObMySQLTransaction &trans, const TaskRow &task);
  int record_failure(common::ObMySQLTransaction &trans, const TaskRow &task, const int ret_code);
private:
  static const int64_t TASK_BATCH_SIZE = 32;
  bool is_inited_;
  common::ObMySQLProxy *sql_proxy_;
  ObAiService *owner_;
};

class ObAiService
{
public:
  ObAiService();
  virtual ~ObAiService();
  static int server_module_init(ObAiService* &ai_service);
  int init();
  int start();
  void stop();
  void wait();
  void destroy();
public:
  int get_ai_service_guard(ObAiServiceGuard &ai_service_guard);
private:
  bool is_inited_;
  common::ObTimer ai_column_timer_;
  ObAiColumnTaskTimer ai_column_task_;
  DISALLOW_COPY_AND_ASSIGN(ObAiService);
};

} // namespace omt
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_OMT_OB_AI_SERVICE_H_
