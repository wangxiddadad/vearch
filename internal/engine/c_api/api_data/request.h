/**
 * Copyright 2019 The Gamma Authors.
 *
 * This source code is licensed under the Apache License, Version 2.0 license
 * found in the LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "common/common_query_data.h"
#include "idl/fbs-gen/c/request_generated.h"
#include "raw_data.h"
#include "util/status.h"

namespace vearch {

class Request : public RawData {
 public:
  Request() {
    request_ = nullptr;
    req_num_ = 0;
    topn_ = 0;
    ranker_ = nullptr;
  }

  virtual ~Request() {
    if (ranker_) {
      delete ranker_;
      ranker_ = nullptr;
    }
  }

  virtual int Serialize(char **out, int *out_len);

  virtual void Deserialize(const char *data, int len);

  int ReqNum();

  void SetReqNum(int req_num);

  int TopN();

  void SetTopN(int topn);

  int BruteForceSearch();

  void SetBruteForceSearch(int brute_force_search);

  void AddVectorQuery(struct VectorQuery &vec_fields);

  void AddField(const std::string &field);

  void AddRangeFilter(struct RangeFilter &range_filter);

  void AddTermFilter(struct TermFilter &term_filter);

  std::vector<struct VectorQuery> &VecFields();

  std::vector<std::string> &Fields();

  std::vector<struct RangeFilter> &RangeFilters();

  std::vector<struct TermFilter> &TermFilters();

  const std::string &IndexParams();

  void SetIndexParams(const std::string &index_params);

  int MultiVectorRank();

  void SetMultiVectorRank(int multi_vector_rank);

  bool L2Sqrt();

  void SetL2Sqrt(bool l2_sqrt);

  vearch::Ranker *Ranker();

  int SetRanker(std::string params, int weight_num);

 private:
  gamma_api::Request *request_;

  int req_num_;
  int topn_;
  int brute_force_search_;  // 1 : brute force search; 0 : normal search

  std::vector<struct VectorQuery> vec_fields_;

  std::vector<std::string> fields_;

  std::vector<struct RangeFilter> range_filters_;
  std::vector<struct TermFilter> term_filters_;

  std::string index_params_;
  int multi_vector_rank_;
  bool l2_sqrt_;
  vearch::Ranker *ranker_;
};

}  // namespace vearch
