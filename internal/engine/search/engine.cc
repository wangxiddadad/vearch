/**
 * Copyright 2019 The Gamma Authors.
 *
 * This source code is licensed under the Apache License, Version 2.0 license
 * found in the LICENSE file in the root directory of this source tree.
 */

#include "engine.h"

#include <fcntl.h>
#include <locale.h>
#ifndef __APPLE__
#include <malloc.h>
#endif
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <vector>

#include "cjson/cJSON.h"
#include "common/gamma_common_data.h"
#include "io/raw_vector_io.h"
#include "omp.h"
#include "table/table_io.h"
#include "util/bitmap.h"
#include "util/log.h"
#include "util/status.h"
#include "util/utils.h"

namespace vearch {

bool RequestConcurrentController::Acquire(int req_num) {
#ifndef __APPLE__
  int num = __sync_fetch_and_add(&cur_concurrent_num_, req_num);

  if (num < concurrent_threshold_) {
    return true;
  } else {
    LOG(WARNING) << "cur_threads_num [" << num << "] concurrent_threshold ["
                 << concurrent_threshold_ << "]";
    return false;
  }
#else
  return true;
#endif
}

void RequestConcurrentController::Release(int req_num) {
#ifndef __APPLE__
  __sync_fetch_and_sub(&cur_concurrent_num_, req_num);
#else
  return;
#endif
}

RequestConcurrentController::RequestConcurrentController() {
  concurrent_threshold_ = 0;
  max_threads_ = 0;
  cur_concurrent_num_ = 0;
  GetMaxThread();
}

int RequestConcurrentController::GetMaxThread() {
#ifndef __APPLE__
  // Get system config and calculate max threads
  int omp_max_threads = omp_get_max_threads();
  int threads_max = GetSystemInfo("cat /proc/sys/kernel/threads-max");
  int max_map_count = GetSystemInfo("cat /proc/sys/vm/max_map_count");
  int pid_max = GetSystemInfo("cat /proc/sys/kernel/pid_max");
  LOG(INFO) << "System info: threads_max [" << threads_max
            << "] max_map_count [" << max_map_count << "] pid_max [" << pid_max
            << "]";
  max_threads_ = std::min(threads_max, pid_max);
  max_threads_ = std::min(max_threads_, max_map_count / 2);
  // calculate concurrent threshold
  concurrent_threshold_ = (max_threads_ * 0.5) / (omp_max_threads + 1);
  LOG(INFO) << "max_threads [" << max_threads_ << "] concurrent_threshold ["
            << concurrent_threshold_ << "]";
  if (concurrent_threshold_ == 0) {
    LOG(FATAL) << "concurrent_threshold cannot be 0!";
  }
  return max_threads_;
#else
  return 0;
#endif
}

int RequestConcurrentController::GetSystemInfo(const char *cmd) {
  int num = 0;

  char buff[1024];
  memset(buff, 0, sizeof(buff));

  FILE *fstream = popen(cmd, "r");
  if (fstream == nullptr) {
    LOG(ERROR) << "execute command failed: " << strerror(errno);
    num = -1;
  } else {
    fgets(buff, sizeof(buff), fstream);
    num = atoi(buff);
    pclose(fstream);
  }
  return num;
}

#ifndef __APPLE__
static std::thread *gMemTrimThread = nullptr;
void MemTrimHandler() {
  LOG(INFO) << "memory trim thread start......";
  while (1) {
    malloc_trim(0);
    std::this_thread::sleep_for(std::chrono::seconds(60));  // 1 minute
  }
  LOG(INFO) << "memory trim thread exit!";
}
#endif

Engine::Engine(const std::string &index_root_path,
               const std::string &space_name)
    : index_root_path_(index_root_path),
      space_name_(space_name),
      date_time_format_("%Y-%m-%d-%H:%M:%S") {
  table_ = nullptr;
  vec_manager_ = nullptr;
  index_status_ = IndexStatus::UNINDEXED;
  delete_num_ = 0;
  b_running_ = 0;
  is_dirty_ = false;
  field_range_index_ = nullptr;
  created_table_ = false;
  docids_bitmap_ = nullptr;
#ifdef PERFORMANCE_TESTING
  search_num_ = 0;
#endif
  af_exector_ = nullptr;
}

Engine::~Engine() {
  if (b_running_) {
    b_running_ = 0;
    std::mutex running_mutex;
    std::unique_lock<std::mutex> lk(running_mutex);
    running_cv_.wait(lk);
  }

  if (af_exector_) {
    af_exector_->Stop();
    CHECK_DELETE(af_exector_);
  }

  if (vec_manager_) {
    delete vec_manager_;
    vec_manager_ = nullptr;
  }

  if (table_) {
    delete table_;
    table_ = nullptr;
  }

  if (field_range_index_) {
    delete field_range_index_;
    field_range_index_ = nullptr;
  }

  if (docids_bitmap_) {
    delete docids_bitmap_;
    docids_bitmap_ = nullptr;
  }
}

Engine *Engine::GetInstance(const std::string &index_root_path,
                            const std::string &space_name) {
  Engine *engine = new Engine(index_root_path, space_name);
  Status status = engine->Setup();
  if (!status.ok()) {
    LOG(ERROR) << "Build " << space_name << " [" << index_root_path
               << "] failed!";
    return nullptr;
  }
  return engine;
}

Status Engine::Setup() {
  if (!utils::isFolderExist(index_root_path_.c_str())) {
    mkdir(index_root_path_.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  }

  dump_path_ = index_root_path_ + "/retrieval_model_index";
  if (!utils::isFolderExist(dump_path_.c_str())) {
    mkdir(dump_path_.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  }

  docids_bitmap_ = new bitmap::BitmapManager();
  docids_bitmap_->SetDumpFilePath(index_root_path_ + "/bitmap");
  int init_bitmap_size = 5000 * 10000;
  bool is_load = false;
  int file_bytes_size = docids_bitmap_->FileBytesSize();
  if (file_bytes_size != 0) {
    init_bitmap_size = file_bytes_size * 8;
    is_load = true;
  }

  if (docids_bitmap_->Init(init_bitmap_size) != 0) {
    std::string msg = "Cannot create bitmap!";
    LOG(ERROR) << msg;
    return Status::IOError(msg);
  }
  if (is_load) {
    docids_bitmap_->Load();
  } else {
    docids_bitmap_->Dump();
  }

  if (!table_) {
    table_ = new Table(index_root_path_, space_name_);
  }

  if (!vec_manager_) {
    vec_manager_ = new VectorManager(VectorStorageType::RocksDB, docids_bitmap_,
                                     index_root_path_);
  }

#ifndef __APPLE__
  if (gMemTrimThread == nullptr) {
    gMemTrimThread = new std::thread(MemTrimHandler);
    if (gMemTrimThread) {
      gMemTrimThread->detach();
    } else {
      LOG(ERROR) << "create memory trim thread error";
    }
  }
#endif

  max_docid_ = 0;
  LOG(INFO) << space_name_ << " setup successed! bitmap_bytes_size="
            << docids_bitmap_->BytesSize();
  return Status::OK();
}

int Engine::Search(Request &request, Response &response_results,
                   Status &status) {
  int req_num = request.ReqNum();

  if (req_num <= 0) {
    std::string msg = space_name_ + " req_num should not less than 0";
    status = Status::InvalidArgument(msg);
    LOG(ERROR) << msg;
    return status.code();
  }

  bool req_permit = RequestConcurrentController::GetInstance().Acquire(req_num);
  if (not req_permit) {
    std::string msg = "Resource temporarily unavailable";
    LOG(WARNING) << msg;
    RequestConcurrentController::GetInstance().Release(req_num);
    status = Status::ResourceExhausted();
    return status.code();
  }

  int topn = request.TopN();
  bool brute_force_search = request.BruteForceSearch();
  std::vector<struct VectorQuery> &vec_fields = request.VecFields();
  size_t vec_fields_num = vec_fields.size();

  if (vec_fields_num > 0 && (not brute_force_search) &&
      (index_status_ != IndexStatus::INDEXED) &&
      (max_docid_ > brute_force_search_threshold)) {
    std::string msg = "index not trained!";
    LOG(WARNING) << msg;
    for (int i = 0; i < req_num; ++i) {
      SearchResult result;
      result.msg = msg;
      result.result_code = SearchResultCode::INDEX_NOT_TRAINED;
      response_results.AddResults(std::move(result));
    }
    RequestConcurrentController::GetInstance().Release(req_num);
    status = Status::IndexNotTrained();
    return status.subcode();
  }

  GammaQuery gamma_query;
  gamma_query.vec_query = vec_fields;

  gamma_query.condition = new SearchCondition(
      static_cast<PerfTool *>(response_results.GetPerTool()));
  gamma_query.condition->topn = topn;
  gamma_query.condition->multi_vector_rank =
      request.MultiVectorRank() == 1 ? true : false;
  gamma_query.condition->brute_force_search = brute_force_search;
  gamma_query.condition->l2_sqrt = request.L2Sqrt();
  gamma_query.condition->index_params = request.IndexParams();

  gamma_query.condition->range_filters = request.RangeFilters();
  gamma_query.condition->term_filters = request.TermFilters();
  gamma_query.condition->table = table_;
  if (request.Ranker()) {
    gamma_query.condition->ranker = dynamic_cast<WeightedRanker *>(request.Ranker());
    if (gamma_query.condition->ranker == nullptr) {
      std::string msg = "ranker error!";
      LOG(WARNING) << msg;
      for (int i = 0; i < req_num; ++i) {
        SearchResult result;
        result.msg = msg;
        result.result_code = SearchResultCode::SEARCH_ERROR;
        response_results.AddResults(std::move(result));
      }
      RequestConcurrentController::GetInstance().Release(req_num);
      status = Status::InvalidArgument();
      return status.subcode();
    } else {
      status = gamma_query.condition->ranker->Parse();
      if (status.code() != status::Code::kOk) {
        std::string msg = "ranker parse err, ranker: " + gamma_query.condition->ranker->ToString();
        LOG(WARNING) << msg;
        RequestConcurrentController::GetInstance().Release(req_num);
        return status.code();
      }
    }
  }

  MultiRangeQueryResults range_query_result;
  size_t range_filters_num = request.RangeFilters().size();
  size_t term_filters_num = request.TermFilters().size();
  if (range_filters_num > 0 || term_filters_num > 0) {
    int num = MultiRangeQuery(request, gamma_query.condition, response_results,
                              &range_query_result);
    if (num == 0) {
      RequestConcurrentController::GetInstance().Release(req_num);
      return status.code();
    }
  }
#ifdef PERFORMANCE_TESTING
  gamma_query.condition->GetPerfTool().Perf("filter");
#endif

  if (vec_fields_num > 0) {
    GammaResult *gamma_results = new GammaResult[req_num];

    int doc_num = GetDocsNum();

    for (int i = 0; i < req_num; ++i) {
      gamma_results[i].total = doc_num;
    }

    status = vec_manager_->Search(gamma_query, gamma_results);
    if (!status.ok()) {
      std::string msg =
          space_name_ + " search error [" + status.ToString() + "]";
      for (int i = 0; i < req_num; ++i) {
        SearchResult result;
        result.msg = msg;
        result.result_code = SearchResultCode::SEARCH_ERROR;
        response_results.AddResults(std::move(result));
      }
      RequestConcurrentController::GetInstance().Release(req_num);
      delete[] gamma_results;
      return status.code();
    }

#ifdef PERFORMANCE_TESTING
    gamma_query.condition->GetPerfTool().Perf("search total");
#endif
    response_results.SetEngineInfo(table_, vec_manager_, gamma_results,
                                   req_num);
  } else {
    GammaResult *gamma_result = new GammaResult[1];
    gamma_result->init(topn, nullptr, 0);

    for (int docid = 0; docid < max_docid_; ++docid) {
      if (range_query_result.Has(docid) && !docids_bitmap_->Test(docid)) {
        ++(gamma_result->total);
        if (gamma_result->results_count < topn) {
          gamma_result->docs[(gamma_result->results_count)++]->docid = docid;
        } else {
          break;
        }
      }
    }
    response_results.SetEngineInfo(table_, vec_manager_, gamma_result, 1);
  }

  RequestConcurrentController::GetInstance().Release(req_num);
  return status.code();
}

int Engine::MultiRangeQuery(Request &request, SearchCondition *condition,
                            Response &response_results,
                            MultiRangeQueryResults *range_query_result) {
  std::vector<FilterInfo> filters;
  std::vector<struct RangeFilter> &range_filters = request.RangeFilters();
  std::vector<struct TermFilter> &term_filters = request.TermFilters();

  int range_filters_size = range_filters.size();
  int term_filters_size = term_filters.size();

  filters.resize(range_filters_size + term_filters_size);
  int idx = 0;

  for (int i = 0; i < range_filters_size; ++i) {
    struct RangeFilter &filter = range_filters[i];

    filters[idx].field = table_->GetAttrIdx(filter.field);
    filters[idx].lower_value = filter.lower_value;
    filters[idx].upper_value = filter.upper_value;
    filters[idx].include_lower = filter.include_lower;
    filters[idx].include_upper = filter.include_upper;

    ++idx;
  }

  for (int i = 0; i < term_filters_size; ++i) {
    struct TermFilter &filter = term_filters[i];

    filters[idx].field = table_->GetAttrIdx(filter.field);
    filters[idx].lower_value = filter.value;
    filters[idx].is_union = static_cast<FilterOperator>(filter.is_union);

    ++idx;
  }

  int num = field_range_index_->Search(filters, range_query_result);

  if (num == 0) {
    std::string msg =
        space_name_ + " no result: numeric filter return 0 result";
    LOG(DEBUG) << msg;
    for (int i = 0; i < request.ReqNum(); ++i) {
      SearchResult result;
      result.msg = msg;
      result.result_code = SearchResultCode::SUCCESS;
      response_results.AddResults(std::move(result));
    }
  } else if (num < 0) {
    condition->range_query_result = nullptr;
  } else {
    condition->range_query_result = range_query_result;
  }
  return num;
}

Status Engine::CreateTable(TableInfo &table) {
  if (!vec_manager_ || !table_) {
    std::string msg = space_name_ + " vector and table should not be null!";
    LOG(ERROR) << msg;
    return Status::ParamError(msg);
  }

  std::string dump_meta_path = index_root_path_ + "/dump.meta";
  utils::JsonParser *meta_jp = nullptr;
  utils::ScopeDeleter1<utils::JsonParser> del1;
  if (utils::file_exist(dump_meta_path)) {
    long len = utils::get_file_size(dump_meta_path);
    if (len > 0) {
      utils::FileIO fio(dump_meta_path);
      if (fio.Open("r")) {
        std::string msg =
            space_name_ + " open file error, path=" + dump_meta_path;
        LOG(ERROR) << msg;
        return Status::IOError(msg);
      }
      char *buf = new char[len + 1];
      buf[len] = '\0';
      if ((size_t)len != fio.Read(buf, 1, (size_t)len)) {
        std::string msg =
            space_name_ + " read file error, path=" + dump_meta_path;
        LOG(ERROR) << msg;
        delete[] buf;
        buf = nullptr;
        return Status::IOError(msg);
      }
      meta_jp = new utils::JsonParser();
      del1.set(meta_jp);
      if (meta_jp->Parse(buf)) {
        delete[] buf;
        buf = nullptr;
        return Status::ParamError();
      }
      delete[] buf;
      buf = nullptr;
    }
  }

  if (vec_manager_->CreateVectorTable(table, meta_jp) != 0) {
    std::string msg = space_name_ + " cannot create VectorTable!";
    LOG(ERROR) << msg;
    vec_manager_->Close();
    return Status::ParamError(msg);
  }
  TableParams disk_table_params;
  if (meta_jp) {
    utils::JsonParser table_jp;
    meta_jp->GetObject("table", table_jp);
    disk_table_params.Parse(table_jp);
  }
  Status status = table_->CreateTable(table, disk_table_params, docids_bitmap_);
  training_threshold_ = table.TrainingThreshold();
  LOG(INFO) << space_name_
            << " init training_threshold=" << training_threshold_;
  if (!status.ok()) {
    std::string msg = space_name_ + " cannot create table!";
    LOG(ERROR) << msg;
    return Status::ParamError(msg);
  }

  af_exector_ = new AsyncFlushExecutor();

  if (!meta_jp) {
    utils::JsonParser dump_meta_;
    dump_meta_.PutInt("version", 327);

    utils::JsonParser table_jp;
    table_->GetDumpConfig()->ToJson(table_jp);
    dump_meta_.PutObject("table", std::move(table_jp));

    utils::JsonParser vectors_jp;
    for (auto &[key, raw_vector_ptr] : vec_manager_->RawVectors()) {
      if (DumpConfig *dc = raw_vector_ptr->GetDumpConfig()) {
        utils::JsonParser jp;
        dc->ToJson(jp);
        vectors_jp.PutObject(dc->name, std::move(jp));
      }
    }
    dump_meta_.PutObject("vectors", std::move(vectors_jp));

    utils::FileIO fio(dump_meta_path);
    fio.Open("w");
    std::string meta_str = dump_meta_.ToStr(true);
    fio.Write(meta_str.c_str(), 1, meta_str.size());
  }
  for (auto &[key, raw_vector_ptr] : vec_manager_->RawVectors()) {
    RawVectorIO *rio = raw_vector_ptr->GetIO();
    if (rio == nullptr) continue;
    AsyncFlusher *flusher = dynamic_cast<AsyncFlusher *>(rio);
    if (flusher) {
      af_exector_->Add(flusher);
    }
  }

  std::string scalar_index_path = index_root_path_ + "/scalar_index";
  utils::make_dir(scalar_index_path.c_str());
  field_range_index_ = new MultiFieldsRangeIndex(scalar_index_path, table_);
  if ((nullptr == field_range_index_) || (AddNumIndexFields() < 0)) {
    std::string msg = "add numeric index fields error!";
    LOG(ERROR) << msg;
    return Status::ParamError(msg);
  }

  std::string table_name = table.Name();
  std::string path = index_root_path_ + "/" + table_name + ".schema";
  TableSchemaIO tio(path);  // rewrite it if the path is already existed
  if (tio.Write(table)) {
    LOG(ERROR) << "write table schema error, path=" << path;
  }

  af_exector_->Start();

  LOG(INFO) << "create table [" << table_name << "] success!";
  created_table_ = true;
  return Status::OK();
}

int Engine::AddOrUpdate(Doc &doc) {
#ifdef PERFORMANCE_TESTING
  double start = utils::getmillisecs();
#endif
  auto &fields_table = doc.TableFields();
  auto &fields_vec = doc.VectorFields();

  std::string &key = doc.Key();

  // add fields into table
  int docid = -1;
  table_->GetDocIDByKey(key, docid);
  if (docid == -1) {
    int ret = table_->Add(key, fields_table, max_docid_);
    if (ret != 0) return -2;
    for (auto &[name, field] : fields_table) {
      int idx = table_->GetAttrIdx(field.name);
      field_range_index_->Add(max_docid_, idx);
    }
  } else {
    if (Update(docid, fields_table, fields_vec)) {
      LOG(DEBUG) << "update error, key=" << key << ", docid=" << docid;
      return -3;
    }
    is_dirty_ = true;
    return 0;
  }
#ifdef PERFORMANCE_TESTING
  double end_table = utils::getmillisecs();
#endif

  // add vectors by VectorManager
  if (vec_manager_->AddToStore(max_docid_, fields_vec) != 0) {
    LOG(ERROR) << "Add to store error max_docid [" << max_docid_ << "]";
    return -4;
  }
  ++max_docid_;
  docids_bitmap_->SetMaxID(max_docid_);

  if (not b_running_ and index_status_ == UNINDEXED) {
    if (max_docid_ >= training_threshold_) {
      LOG(INFO) << space_name_ << " begin indexing. training_threshold="
                << training_threshold_;
      this->BuildIndex();
    }
  }
#ifdef PERFORMANCE_TESTING
  double end = utils::getmillisecs();
  if (max_docid_ % 10000 == 0) {
    LOG(INFO) << space_name_ << " table cost [" << end_table - start
              << "]ms, vec store cost [" << end - end_table << "]ms";
  }
#endif
  is_dirty_ = true;
  return 0;
}

int Engine::Update(int doc_id,
                   std::unordered_map<std::string, struct Field> &fields_table,
                   std::unordered_map<std::string, struct Field> &fields_vec) {
  int ret = vec_manager_->Update(doc_id, fields_vec);
  if (ret != 0) {
    return ret;
  }

  auto is_equal = table_->CheckFieldIsEqual(fields_table, doc_id);
  for (auto &[name, field] : fields_table) {
    if (is_equal[name]) {
      continue;
    }

    int idx = table_->GetAttrIdx(field.name);
    field_range_index_->Delete(doc_id, idx);
  }

  if (table_->Update(fields_table, doc_id) != 0) {
    LOG(DEBUG) << "table update error";
    return -1;
  }

  for (auto &[name, field] : fields_table) {
    if (is_equal[name]) {
      continue;
    }
    int idx = table_->GetAttrIdx(field.name);
    field_range_index_->Add(doc_id, idx);
  }

  LOG(DEBUG) << "update success! key=" << fields_table["_id"].value;
  is_dirty_ = true;
  return 0;
}

int Engine::Delete(std::string &key) {
  int docid = -1, ret = 0;
  ret = table_->GetDocIDByKey(key, docid);
  if (ret != 0 || docid < 0) return -1;

  if (docids_bitmap_->Test(docid)) {
    return ret;
  }
  ++delete_num_;
  docids_bitmap_->Set(docid);
  docids_bitmap_->Dump(docid, 1);
  const auto &name_to_idx = table_->FieldMap();
  for (const auto &ite : name_to_idx) {
    field_range_index_->Delete(docid, ite.second);
  }
  table_->Delete(key);

  vec_manager_->Delete(docid);
  is_dirty_ = true;

  return ret;
}

int Engine::GetDoc(const std::string &key, Doc &doc) {
  int docid = -1, ret = 0;
  ret = table_->GetDocIDByKey(key, docid);
  if (ret != 0 || docid < 0) {
    LOG(INFO) << space_name_ << " GetDocIDbyKey [" << key << "] not found!";
    return -1;
  }

  return GetDoc(docid, doc);
}

int Engine::GetDoc(int docid, Doc &doc, bool next) {
  int ret = 0;
  if (next) {
    while (++docid < max_docid_) {
      if (!docids_bitmap_->Test(docid)) {
        break;
      }
    }
    if (docid >= max_docid_) {
      return -1;
    }
  } else if (docids_bitmap_->Test(docid)) {
    LOG(DEBUG) << space_name_ << " docid [" << docid << "] is deleted!";
    return -1;
  }
  std::vector<std::string> index_names;
  vec_manager_->VectorNames(index_names);

  std::vector<std::string> table_fields;
  ret = table_->GetDocInfo(docid, doc, table_fields);
  if (ret != 0) {
    return ret;
  }

  if (next) {
    struct Field field;
    field.name = "_docid";
    field.datatype = DataType::INT;
    const char *bytes = reinterpret_cast<const char *>(&docid);
    field.value = std::string(bytes, sizeof(docid));
    doc.AddField(std::move(field));
  }

  std::vector<std::pair<std::string, int>> vec_fields_ids;
  for (size_t i = 0; i < index_names.size(); ++i) {
    vec_fields_ids.emplace_back(std::make_pair(index_names[i], docid));
  }

  std::vector<std::string> vec;
  ret = vec_manager_->GetVector(vec_fields_ids, vec, true);
  if (ret == 0 && vec.size() == vec_fields_ids.size()) {
    for (size_t i = 0; i < index_names.size(); ++i) {
      struct Field field;
      field.name = index_names[i];
      field.datatype = DataType::VECTOR;
      field.value = vec[i];
      doc.AddField(field);
    }
  }
  return 0;
}

int Engine::BuildIndex() {
  int running = __sync_fetch_and_add(&b_running_, 1);
  if (running) {
    if (vec_manager_->TrainIndex(vec_manager_->VectorIndexes()) != 0) {
      LOG(ERROR) << "Create index failed!";
      return -1;
    }
    return 0;
  }

  auto func_indexing = std::bind(&Engine::Indexing, this);
  std::thread t(func_indexing);
  t.detach();
  return 0;
}

// TODO set limit for cpu and should to avoid using vector indexes on the same
// time
int Engine::RebuildIndex(int drop_before_rebuild, int limit_cpu, int describe) {
  int ret = 0;
  if (describe) {
    vec_manager_->DescribeVectorIndexes();
    return ret;
  }
  std::map<std::string, IndexModel *> vector_indexes;

  if (!drop_before_rebuild) {
    ret =
        vec_manager_->CreateVectorIndexes(training_threshold_, vector_indexes);
    if (vec_manager_->TrainIndex(vector_indexes) != 0) {
      LOG(ERROR) << "RebuildIndex TrainIndex failed!";
      return -1;
    }
    LOG(INFO) << "vector manager RebuildIndex TrainIndex success!";
  }

  index_status_ = IndexStatus::UNINDEXED;
  if (b_running_) {
    b_running_ = 0;
    std::mutex running_mutex;
    std::unique_lock<std::mutex> lk(running_mutex);
    running_cv_.wait(lk);
  }

  vec_manager_->DestroyVectorIndexes();

  if (drop_before_rebuild) {
    ret = vec_manager_->CreateVectorIndexes(training_threshold_,
                                            vec_manager_->VectorIndexes());
    if (ret) {
      LOG(ERROR) << "RebuildIndex CreateVectorIndexes failed, ret: " << ret;
      vec_manager_->DestroyVectorIndexes();
      return ret;
    }
    if (vec_manager_->TrainIndex(vec_manager_->VectorIndexes()) != 0) {
      LOG(ERROR) << "RebuildIndex TrainIndex failed!";
      return -1;
    }
    LOG(INFO) << "vector manager RebuildIndex TrainIndex success!";
  } else {
    vec_manager_->SetVectorIndexes(vector_indexes);
    LOG(INFO) << "vector manager SetVectorIndexes success!";
  }

  ret = BuildIndex();
  if (ret) {
    LOG(ERROR) << "ReBuildIndex BuildIndex failed, ret: " << ret;
    return ret;
  }

  return 0;
}

int Engine::Indexing() {
  if (vec_manager_->TrainIndex(vec_manager_->VectorIndexes()) != 0) {
    LOG(ERROR) << "Create index failed!";
    b_running_ = 0;
    return -1;
  }

  LOG(INFO) << "vector manager TrainIndex success!";
  int ret = 0;
  bool has_error = false;
  while (b_running_) {
    if (has_error) {
      usleep(5000 * 1000);  // sleep 5000ms
      continue;
    }
    index_status_ = IndexStatus::INDEXED;
    bool index_is_dirty = false;
    int add_ret = vec_manager_->AddRTVecsToIndex(index_is_dirty);
    if (add_ret < 0) {
      has_error = true;
      LOG(ERROR) << "Add real time vectors to index error!";
      continue;
    }
    if (index_is_dirty == true) {
      is_dirty_ = true;
    }
    usleep(1000 * 1000);  // sleep 5000ms
  }
  running_cv_.notify_one();
  LOG(INFO) << space_name_ << " build index exited!";
  return ret;
}

int Engine::GetDocsNum() { return max_docid_ - delete_num_; }

void Engine::GetIndexStatus(EngineStatus &engine_status) {
  engine_status.SetIndexStatus(index_status_);

  // long table_mem_bytes = table_->GetMemoryBytes();
  // long vec_mem_bytes = 0, index_mem_bytes = 0;
  // vec_manager_->GetTotalMemBytes(index_mem_bytes, vec_mem_bytes);

  // long total_mem_b = 0;
  // long dense_b = 0, sparse_b = 0;
  // if (field_range_index_) {
  //   total_mem_b += field_range_index_->MemorySize(dense_b, sparse_b);
  // }

  // engine_status.SetTableMem(table_mem_bytes);
  // engine_status.SetIndexMem(index_mem_bytes);
  // engine_status.SetVectorMem(vec_mem_bytes);
  // engine_status.SetFieldRangeMem(total_mem_b);
  // engine_status.SetBitmapMem(docids_bitmap_->BytesSize());
  engine_status.SetDocNum(GetDocsNum());
  engine_status.SetMaxDocID(max_docid_ - 1);
  engine_status.SetMinIndexedNum(vec_manager_->MinIndexedNum());
}

void Engine::GetMemoryInfo(MemoryInfo &memory_info) {
  long table_mem_bytes = table_->GetMemoryBytes();
  long vec_mem_bytes = 0, index_mem_bytes = 0;
  vec_manager_->GetTotalMemBytes(index_mem_bytes, vec_mem_bytes);

  long total_mem_b = 0;
  // TODO: add lock in field_range_index_->MemorySize to prevent crash
  // long dense_b = 0, sparse_b = 0;
  // if (field_range_index_) {
  //   total_mem_b += field_range_index_->MemorySize(dense_b, sparse_b);
  // }

  // long total_mem_kb = total_mem_b / 1024;
  // long total_mem_mb = total_mem_kb / 1024;
  // LOG(INFO) << "Field range memory [" << total_mem_kb << "]kb, ["
  //           << total_mem_mb << "]MB, dense [" << dense_b / 1024 / 1024
  //           << "]MB sparse [" << sparse_b / 1024 / 1024
  //           << "]MB";

  memory_info.SetTableMem(table_mem_bytes);
  memory_info.SetIndexMem(index_mem_bytes);
  memory_info.SetVectorMem(vec_mem_bytes);
  memory_info.SetFieldRangeMem(total_mem_b);
  memory_info.SetBitmapMem(docids_bitmap_->BytesSize());
}

int Engine::Dump() {
  int ret = 0;
  if (is_dirty_) {
    int max_docid = max_docid_ - 1;
    std::time_t t = std::time(nullptr);
    char tm_str[100];
    std::strftime(tm_str, sizeof(tm_str), date_time_format_.c_str(),
                  std::localtime(&t));

    std::string path = dump_path_ + "/" + tm_str;
    if (!utils::isFolderExist(path.c_str())) {
      mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }

    ret = vec_manager_->Dump(path, 0, max_docid);
    if (ret != 0) {
      LOG(ERROR) << space_name_ << " dump vector error, ret=" << ret;
      utils::remove_dir(path.c_str());
      LOG(ERROR) << space_name_ << " dumped to [" << path
                 << "] failed, now removed";
      return -1;
    }

    const std::string dump_done_file = path + "/dump.done";
    std::ofstream f_done;
    f_done.open(dump_done_file);
    if (!f_done.is_open()) {
      LOG(ERROR) << "Cannot create file " << dump_done_file;
      return -1;
    }
    f_done << "start_docid " << 0 << std::endl;
    f_done << "end_docid " << max_docid << std::endl;
    f_done.close();

    if (last_dump_dir_ != "" && utils::remove_dir(last_dump_dir_.c_str())) {
      LOG(ERROR) << "remove last dump directory error, path=" << last_dump_dir_;
    }
    LOG(INFO) << space_name_ << " dumped to [" << path
              << "], last dump directory(removed)=" << last_dump_dir_;
    last_dump_dir_ = path;
    is_dirty_ = false;
  }
  return 0;
}

int Engine::CreateTableFromLocal(std::string &table_name) {
  std::vector<std::string> file_paths = utils::ls(index_root_path_);
  for (std::string &file_path : file_paths) {
    std::string::size_type pos = file_path.rfind(".schema");
    if (pos == file_path.size() - 7) {
      std::string::size_type begin = file_path.rfind('/');
      assert(begin != std::string::npos);
      begin += 1;
      table_name = file_path.substr(begin, pos - begin);
      LOG(INFO) << space_name_ << " local table name=" << table_name;
      TableSchemaIO tio(file_path);
      TableInfo table;
      if (tio.Read(table_name, table)) {
        LOG(ERROR) << "read table schema error, path=" << file_path;
        return -1;
      }

      Status status = CreateTable(table);
      if (!status.ok()) {
        LOG(ERROR) << "create table error when loading";
        return -1;
      }
      return 0;
    }
  }
  return -1;
}

int Engine::Load() {
  if (!created_table_) {
    std::string table_name;
    if (CreateTableFromLocal(table_name)) {
      LOG(ERROR) << space_name_ << " create table from local error";
      return -1;
    }
    LOG(INFO) << space_name_
              << " create table from local success, table name=" << table_name;
  }
  af_exector_->Stop();

  std::vector<std::pair<std::time_t, std::string>> folders_tm;
  std::vector<std::string> folders = utils::ls_folder(dump_path_);
  std::vector<std::string> folders_not_done;
  for (const std::string &folder_name : folders) {
    if (folder_name == "") continue;
    std::string folder_path = dump_path_ + "/" + folder_name;
    std::string done_file = folder_path + "/dump.done";
    if (!utils::file_exist(done_file)) {
      LOG(INFO) << "done file is not existed, skip it! path=" << done_file;
      folders_not_done.push_back(folder_path);
      continue;
    }
    struct tm result;
    strptime(folder_name.c_str(), date_time_format_.c_str(), &result);
    std::time_t t = std::mktime(&result);
    folders_tm.push_back(std::make_pair(t, folder_path));
  }
  std::sort(folders_tm.begin(), folders_tm.end(),
            [](const std::pair<std::time_t, std::string> &a,
               const std::pair<std::time_t, std::string> &b) {
              return a.first < b.first;
            });
  if (folders_tm.size() > 0) {
    std::string dump_done_file =
        folders_tm[folders_tm.size() - 1].second + "/dump.done";
    utils::FileIO fio(dump_done_file);
    if (fio.Open("r")) {
      LOG(ERROR) << space_name_ << " cannot read from file " << dump_done_file;
      return -1;
    }
    long fsize = utils::get_file_size(dump_done_file);
    char *buf = new char[fsize];
    fio.Read(buf, 1, fsize);
    std::string buf_str(buf, fsize);
    std::vector<std::string> lines = utils::split(buf_str, "\n");
    assert(lines.size() == 2);
    std::vector<std::string> items = utils::split(lines[1], " ");
    assert(items.size() == 2);
    int index_dump_num = (int)std::strtol(items[1].c_str(), nullptr, 10) + 1;
    LOG(INFO) << space_name_ << "read index_dump_num=" << index_dump_num
              << " from " << dump_done_file;
    delete[] buf;
    buf = nullptr;
  }

  max_docid_ = table_->GetStorageManagerSize();

  std::string last_dir = "";
  std::vector<std::string> dirs;
  if (folders_tm.size() > 0) {
    last_dir = folders_tm[folders_tm.size() - 1].second;
    LOG(INFO) << "Loading from " << last_dir;
    dirs.push_back(last_dir);
  }
  int ret = vec_manager_->Load(dirs, max_docid_);
  if (ret != 0) {
    LOG(ERROR) << space_name_ << " load vector error, ret=" << ret
               << ", path=" << last_dir;
    return ret;
  }

  ret = table_->Load(max_docid_);
  if (ret != 0) {
    LOG(ERROR) << space_name_ << " load profile error, ret=" << ret;
    return ret;
  }

  int field_num = table_->FieldsNum();
  for (int i = 0; i < max_docid_; ++i) {
    for (int j = 0; j < field_num; ++j) {
      while (field_range_index_->PendingTasks() > 100000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      field_range_index_->Add(i, j);
    }
  }

  delete_num_ = 0;
  for (int i = 0; i < max_docid_; ++i) {
    if (docids_bitmap_->Test(i)) {
      ++delete_num_;
    }
  }

  if (not b_running_ and index_status_ == UNINDEXED) {
    if (max_docid_ >= training_threshold_) {
      LOG(INFO) << space_name_ << " begin indexing. training_threshold="
                << training_threshold_;
      this->BuildIndex();
    }
  }
  // remove directorys which are not done
  for (const std::string &folder : folders_not_done) {
    if (utils::remove_dir(folder.c_str())) {
      LOG(ERROR) << space_name_
                 << " clean error, not done directory=" << folder;
    }
  }
  af_exector_->Start();
  last_dump_dir_ = last_dir;
  LOG(INFO) << "load engine success! max docid=" << max_docid_
            << ", load directory=" << last_dir
            << ", clean directorys(not done)="
            << utils::join(folders_not_done, ',');
  return 0;
}

int Engine::LoadFromFaiss() {
  std::map<std::string, IndexModel *> &vec_indexes =
      vec_manager_->VectorIndexes();
  if (vec_indexes.size() != 1) {
    LOG(ERROR) << space_name_ << " load from faiss index should be only one!";
    return -1;
  }

  IndexModel *index = vec_indexes.begin()->second;
  if (index == nullptr) {
    LOG(ERROR) << space_name_ << " cannot find faiss index";
    return -1;
  }
  index_status_ = INDEXED;

  int load_num;
  Status status = index->Load("files", load_num);
  if (!status.ok()) {
    LOG(ERROR) << space_name_ << " vector [faiss] load gamma index failed!";
    return -1;
  }

  int d = index->vector_->MetaInfo()->Dimension();
  int fd = open("files/feature", O_RDONLY);
  size_t mmap_size = load_num * sizeof(float) * d;
  float *feature =
      (float *)mmap(NULL, mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
  for (int i = 0; i < load_num; ++i) {
    Doc doc;
    Field field;
    field.name = "_id";
    field.datatype = DataType::STRING;
    field.value = std::to_string(i);
    doc.SetKey(field.value);
    doc.AddField(std::move(field));
    field.name = "faiss";
    field.datatype = DataType::VECTOR;
    field.value =
        std::string((char *)(feature + (uint64_t)i * d), d * sizeof(float));
    doc.AddField(std::move(field));
    AddOrUpdate(doc);
  }
  munmap(feature, mmap_size);
  close(fd);
  return 0;
}

int Engine::AddNumIndexFields() {
  int retvals = 0;
  std::map<std::string, enum DataType> attr_type;
  retvals = table_->GetAttrType(attr_type);

  std::map<std::string, bool> attr_index;
  retvals = table_->GetAttrIsIndex(attr_index);
  for (const auto &it : attr_type) {
    std::string field_name = it.first;
    const auto &attr_index_it = attr_index.find(field_name);
    if (attr_index_it == attr_index.end()) {
      LOG(ERROR) << space_name_ << " cannot find field [" << field_name << "]";
      continue;
    }
    bool is_index = attr_index_it->second;
    if (not is_index) {
      continue;
    }
    int field_idx = table_->GetAttrIdx(field_name);
    LOG(INFO) << space_name_ << " add range field [" << field_name << "]";
    field_range_index_->AddField(field_idx, it.second, field_name);
  }
  return retvals;
}

int Engine::GetConfig(Config &conf) {
  conf.ClearCacheInfos();
  vec_manager_->GetAllCacheSize(conf);
  int table_cache_size = 0;
  table_->GetCacheSize(table_cache_size);
  conf.AddCacheInfo("table", table_cache_size);
  return 0;
}

int Engine::SetConfig(Config &conf) {
  int table_cache_size = 0;
  for (auto &c : conf.CacheInfos()) {
    if (c.field_name == "table") {
      table_cache_size = c.cache_size;
    } else {
      vec_manager_->AlterCacheSize(c);
    }
  }
  table_->AlterCacheSize(table_cache_size);
  GetConfig(conf);
  return 0;
}

}  // namespace vearch
