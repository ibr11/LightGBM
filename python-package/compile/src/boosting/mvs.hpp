#ifndef LIGHTGBM_BOOSTING_MVS_H_
#define LIGHTGBM_BOOSTING_MVS_H_

#include <LightGBM/utils/array_args.h>
#include <LightGBM/utils/log.h>
#include <LightGBM/utils/openmp_wrapper.h>
#include <LightGBM/boosting.h>

#include "score_updater.hpp"
#include "gbdt.h"

#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <functional>
#include <iostream>

namespace LightGBM {

#ifdef TIMETAG
std::chrono::duration<double, std::milli> subset_time;
std::chrono::duration<double, std::milli> re_init_tree_time;
#endif

class MVS: public GBDT {
public:
  /*!
  * \brief Constructor
  */
  MVS() : GBDT() {

  }

  ~MVS() {
    #ifdef TIMETAG
    Log::Info("MVS::subset costs %f", subset_time * 1e-3);
    Log::Info("MVS::re_init_tree costs %f", re_init_tree_time * 1e-3);
    #endif
  }

  void Init(const Config* config, const Dataset* train_data, const ObjectiveFunction* objective_function,
            const std::vector<const Metric*>& training_metrics) override {
    GBDT::Init(config, train_data, objective_function, training_metrics);
    ResetMVS();
  }

  void ResetTrainingData(const Dataset* train_data, const ObjectiveFunction* objective_function,
                         const std::vector<const Metric*>& training_metrics) override {
    GBDT::ResetTrainingData(train_data, objective_function, training_metrics);
    ResetMVS();
  }

  void ResetConfig(const Config* config) override {
    GBDT::ResetConfig(config);
    ResetMVS();
  }

  void ResetMVS() {
//    CHECK(config_->top_rate + config_->other_rate <= 1.0f);
 //   CHECK(config_->top_rate + config_->other_rate > 0.0f);
  //  if (config_->bagging_freq > 0 && config_->bagging_fraction != 1.0f) {
   //   Log::Fatal("Cannot use bagging in MVS");
    //}
    Log::Info("Using MVS");

    bag_data_indices_.resize(num_data_);
    tmp_indices_.resize(num_data_);
    tmp_indice_right_.resize(num_data_);
    offsets_buf_.resize(num_threads_);
    left_cnts_buf_.resize(num_threads_);
    right_cnts_buf_.resize(num_threads_);
    left_write_pos_buf_.resize(num_threads_);
    right_write_pos_buf_.resize(num_threads_);

    is_use_subset_ = false;
    if (config_->bagging_fraction <= 0.5) {
      auto bag_data_cnt = static_cast<data_size_t>((config_->bagging_fraction) * num_data_);
      bag_data_cnt = std::max(1, bag_data_cnt);
      tmp_subset_.reset(new Dataset(bag_data_cnt));
      tmp_subset_->CopyFeatureMapperFrom(train_data_);
      is_use_subset_ = true;
    }
    // flag to not bagging first
    bag_data_cnt_ = num_data_;
  }

  score_t CalculateThreshold(std::vector<score_t>::iterator begin,
  	                         std::vector<score_t>::iterator end,
  	                         score_t sum_low, size_t n_high, double sample_rate) {
  	score_t threshold = *begin;
  	auto middle_begin = std::partition(begin, end, [threshold](const score_t& x) {return x < threshold;});
  	auto middle_end = std::partition(middle_begin, end, [threshold](const score_t& x) {return x <= threshold;});
  	
  	score_t sum_left = std::accumulate(begin, middle_begin, 0.0);
  	size_t n_right = end - middle_end;
  	size_t n_middle = middle_end - middle_begin;
  	score_t sum_middle = n_middle * threshold;

  	double estimated_sample_rate = (sum_low + sum_left) / threshold + n_high + n_right + n_middle;
  	if (estimated_sample_rate > sample_rate) {
  		if (middle_end != end) {
  			sum_low += sum_middle + sum_left;
  			return CalculateThreshold(middle_end, end, sum_low, n_high, sample_rate);
  		} else {
  			return (sum_low + sum_left + sum_middle) / (sample_rate - n_high);
  		}
  	} else {
  		if (middle_begin != begin) {
  			n_high += n_right + n_middle;
  			return CalculateThreshold(begin, middle_begin, sum_low, n_high, sample_rate);
  		} else {
  			return sum_low / (sample_rate - n_high - n_middle - n_right + 1e-30);
  		}
  	}
  }

  data_size_t BaggingHelper(Random& cur_rand, data_size_t start, data_size_t cnt, data_size_t* buffer, data_size_t* buffer_right) {
    if (cnt <= 0) { 
      return 0;
    }
    std::vector<score_t> tmp_gradients(cnt, 0.0f);
    double lambda = config_->var_weight;
    for (data_size_t i = 0; i < cnt; ++i) {
      for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
        size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
        tmp_gradients[i] += std::fabs(gradients_[idx] * hessians_[idx]);
      }
      tmp_gradients[i] = std::sqrt(tmp_gradients[i] * tmp_gradients[i] + lambda);
    }
    score_t threshold = 0;
    double sample_rate = config_->bagging_fraction * cnt;
    threshold = CalculateThreshold(tmp_gradients.begin(), tmp_gradients.end(), 0.0, 0, sample_rate);
      //std::sort(tmp_gradients.begin(), tmp_gradients.end());
      //score_t cum_sum = 0;
      //double sample_rate = (config_->top_rate + config_->other_rate) * cnt;
      //double cur_sample_rate = 0;
      //for (int i = 1; i < cnt; ++i) {
      //  cum_sum += tmp_gradients[i - 1];
      //  threshold = tmp_gradients[i];
      //  cur_sample_rate = cum_sum / threshold + cnt - i;
      //  if (sample_rate >= cur_sample_rate) {
      //    break;
      //  }
      //}
      //if (sample_rate < cur_sample_rate) {
      //    threshold *= cur_sample_rate / sample_rate;
      //} 
    

    data_size_t cur_left_cnt = 0;
    data_size_t cur_right_cnt = 0;
    data_size_t big_weight_cnt = 0;
    for (data_size_t i = 0; i < cnt; ++i) {
      score_t grad = 0.0f;
      for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
        size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
        grad += std::fabs(gradients_[idx] * hessians_[idx]);
      }
      grad = std::sqrt(grad * grad + lambda);
      if (grad >= threshold) {
      	double prob = 1;
      	score_t multiply = 1.0 / prob;
      	if (cur_rand.NextFloat() < prob) {
          buffer[cur_left_cnt++] = start + i;
          ++big_weight_cnt;
          for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
            size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
            gradients_[idx] *= multiply;
            hessians_[idx] *= multiply;
      	  }
        } else {
      	  buffer_right[cur_right_cnt++] = start + i;
      	}
      } else {
      	//double prior = static_cast<double>(threshold) * 0.01;
        //double prob = (grad + prior) / (static_cast<double>(threshold) + prior);
        double prob = grad / static_cast<double>(threshold);
        score_t multiply = 1.0 / (prob + 1e-35);
        if (cur_rand.NextFloat() < prob) {
          buffer[cur_left_cnt++] = start + i;
          for (int cur_tree_id = 0; cur_tree_id < num_tree_per_iteration_; ++cur_tree_id) {
            size_t idx = static_cast<size_t>(cur_tree_id) * num_data_ + start + i;
            gradients_[idx] *= multiply;
            hessians_[idx] *= multiply;
          }
        } else {
          buffer_right[cur_right_cnt++] = start + i;
        }
      }
    }
    return cur_left_cnt;
  }

  void Bagging(int iter) override {
    bag_data_cnt_ = num_data_;
    // not subsample for first iterations
    //if (iter < static_cast<int>(1.0f / config_->learning_rate)) { return; }

    const data_size_t min_inner_size = 100;
    data_size_t inner_size = (num_data_ + num_threads_ - 1) / num_threads_;
    if (inner_size < min_inner_size) { inner_size = min_inner_size; }
    OMP_INIT_EX();
    #pragma omp parallel for schedule(static, 1)
    for (int i = 0; i < num_threads_; ++i) {
      OMP_LOOP_EX_BEGIN();
      left_cnts_buf_[i] = 0;
      right_cnts_buf_[i] = 0;
      data_size_t cur_start = i * inner_size;
      if (cur_start > num_data_) { continue; }
      data_size_t cur_cnt = inner_size;
      if (cur_start + cur_cnt > num_data_) { cur_cnt = num_data_ - cur_start; }
      Random cur_rand(config_->bagging_seed + iter * num_threads_ + i);
      data_size_t cur_left_count = BaggingHelper(cur_rand, cur_start, cur_cnt,
                                                 tmp_indices_.data() + cur_start, tmp_indice_right_.data() + cur_start);
      offsets_buf_[i] = cur_start;
      left_cnts_buf_[i] = cur_left_count;
      right_cnts_buf_[i] = cur_cnt - cur_left_count;
      OMP_LOOP_EX_END();
    }
    OMP_THROW_EX();
    data_size_t left_cnt = 0;
    left_write_pos_buf_[0] = 0;
    right_write_pos_buf_[0] = 0;
    for (int i = 1; i < num_threads_; ++i) {
      left_write_pos_buf_[i] = left_write_pos_buf_[i - 1] + left_cnts_buf_[i - 1];
      right_write_pos_buf_[i] = right_write_pos_buf_[i - 1] + right_cnts_buf_[i - 1];
    }
    left_cnt = left_write_pos_buf_[num_threads_ - 1] + left_cnts_buf_[num_threads_ - 1];

    #pragma omp parallel for schedule(static, 1)
    for (int i = 0; i < num_threads_; ++i) {
      OMP_LOOP_EX_BEGIN();
      if (left_cnts_buf_[i] > 0) {
        std::memcpy(bag_data_indices_.data() + left_write_pos_buf_[i],
                    tmp_indices_.data() + offsets_buf_[i], left_cnts_buf_[i] * sizeof(data_size_t));
      }
      if (right_cnts_buf_[i] > 0) {
        std::memcpy(bag_data_indices_.data() + left_cnt + right_write_pos_buf_[i],
                    tmp_indice_right_.data() + offsets_buf_[i], right_cnts_buf_[i] * sizeof(data_size_t));
      }
      OMP_LOOP_EX_END();
    }
    OMP_THROW_EX();
    bag_data_cnt_ = left_cnt;
    Log::Info("Sample rate %f", float(bag_data_cnt_) / num_data_);
    //std::cout << "Sample rate set to " << float(bag_data_cnt_) / num_data_ << '\n';
    // set bagging data to tree learner
    if (!is_use_subset_) {
      tree_learner_->SetBaggingData(bag_data_indices_.data(), bag_data_cnt_);
    } else {
      // get subset
      #ifdef TIMETAG
      auto start_time = std::chrono::steady_clock::now();
      #endif
      tmp_subset_->ReSize(bag_data_cnt_);
      tmp_subset_->CopySubset(train_data_, bag_data_indices_.data(), bag_data_cnt_, false);
      #ifdef TIMETAG
      subset_time += std::chrono::steady_clock::now() - start_time;
      #endif
      #ifdef TIMETAG
      start_time = std::chrono::steady_clock::now();
      #endif
      tree_learner_->ResetTrainingData(tmp_subset_.get());
      #ifdef TIMETAG
      re_init_tree_time += std::chrono::steady_clock::now() - start_time;
      #endif
    }
  }

private:
  std::vector<data_size_t> tmp_indice_right_;
};

}  // namespace LightGBM
#endif   // LIGHTGBM_BOOSTING_MVS_H_
