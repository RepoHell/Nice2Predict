/*
   Copyright 2014 Software Reliability Lab, ETH Zurich

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include <string>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include "base.h"
#include "gflags/gflags.h"
#include "glog/logging.h"

#include "jsoncpp/json/json.h"
#include "graph_inference.h"

#include "stringprintf.h"
#include "stringset.h"
#include "readerutil.h"

DEFINE_string(input, "testdata", "Input file with JSON objects regarding training data");
DEFINE_string(out_model, "model", "File prefix for output models");
DEFINE_bool(hogwild, true, "Whether to use Hogwild parallel training");
DEFINE_int32(num_threads, 8, "Number of threads to use");
DEFINE_int32(num_training_passes, 24, "Number of passes in training");

DEFINE_double(start_learning_rate, 0.1, "Initial learning rate");
DEFINE_double(stop_learning_rate, 0.0001, "Stop learning if learning rate falls below the value");
DEFINE_double(regularization_const, 2.0, "Regularization constant. The higher, the more regularization.");
DEFINE_double(svm_margin, 0.1, "SVM Margin = Penalty for keeping equal labels as in the training data during training.");

DEFINE_int32(cross_validation_folds, 0, "If more than 1, cross-validation is performed with the specified number of folds");
DEFINE_bool(evaluate, false, "Whether to perform evaluation instead of training. In this case, the --input parameter contains JSON values with evaluation data.");
DEFINE_string(output_errors, "", "If set, will output the label errors done by the system.");

DEFINE_bool(train_multiclass_classifier, false, "Perform training by base multiclass classifier.");

typedef std::function<void(const Json::Value&, const Json::Value&)> InputProcessor;
void ForeachInput(RecordInput* input, InputProcessor proc) {
  std::unique_ptr<InputRecordReader> reader(input->CreateReader());
  Json::Reader jsonreader;
  while (!reader->ReachedEnd()) {
    std::string line;
    reader->Read(&line);
    if (line.empty()) continue;
    Json::Value v;
    if (!jsonreader.parse(line, v, false)) {
      LOG(ERROR) << "Could not parse input: " << jsonreader.getFormatedErrorMessages();
    } else {
      proc(v["query"], v["assign"]);
    }
  }
}

void ProcessLinesParallel(InputRecordReader* reader, InputProcessor proc) {
  std::string line;
  Json::Reader jsonreader;
  while (!reader->ReachedEnd()) {
    std::string line;
    reader->Read(&line);
    if (line.empty()) continue;
    Json::Value v;
    if (!jsonreader.parse(line, v, false)) {
      LOG(ERROR) << "Could not parse input: " << jsonreader.getFormatedErrorMessages() << "\n" << line;
    } else {
      proc(v["query"], v["assign"]);
    }
  }
}

typedef std::function<void(const std::string&)> RawInputProcessor;
void ProcessRawLinesParallel(InputRecordReader* reader, RawInputProcessor proc) {
  std::string line;
  while (!reader->ReachedEnd()) {
    std::string line;
    reader->Read(&line);
    if (line.empty()) continue;
    proc(line);
  }
}

void ParallelForeachRawInput(RecordInput* input, RawInputProcessor proc) {
  // Do parallel ForEach
  std::unique_ptr<InputRecordReader> reader(input->CreateReader());
  std::vector<std::thread> threads;
  for (int i = 0; i < FLAGS_num_threads; ++i) {
    threads.push_back(std::thread(std::bind(&ProcessRawLinesParallel, reader.get(), proc)));
  }
  for (auto& thread : threads){
    thread.join();
  }
}

void ParallelForeachInput(RecordInput* input, InputProcessor proc) {
  if (!FLAGS_hogwild) {
    ForeachInput(input, proc);
    return;
  }

  // Do parallel ForEach
  std::unique_ptr<InputRecordReader> reader(input->CreateReader());
  std::vector<std::thread> threads;
  for (int i = 0; i < FLAGS_num_threads; ++i) {
    threads.push_back(std::thread(std::bind(&ProcessLinesParallel, reader.get(), proc)));
  }
  for (auto& thread : threads){
    thread.join();
  }
}

void InitTrain(RecordInput* input, GraphInference* inference) {
  int count = 0;
  std::mutex mutex;
  ParallelForeachInput(input, [&inference,&count,&mutex](const Json::Value& query, const Json::Value& assign) {
    std::lock_guard<std::mutex> lock(mutex);
    inference->AddQueryToModel(query, assign);
    ++count;
  });
  LOG(INFO) << "Loaded " << count << " training data samples.";
  inference->PrepareForInference();
}




void TestInference(RecordInput* input, GraphInference* inference) {
  int64 start_time = GetCurrentTimeMicros();
  double score_gain = 0;
  // int count = 0;
  ForeachInput(input, [&](const Json::Value& query, const Json::Value& assign) {
    // if (count >= 10) return;
    // count++;
    Nice2Query* q = inference->CreateQuery();
    q->FromJSON(query);
    Nice2Assignment* a = inference->CreateAssignment(q);
    a->FromJSON(assign);
    double start_score = inference->GetAssignmentScore(a);
    inference->MapInference(q, a);
    score_gain += inference->GetAssignmentScore(a) - start_score;
    delete a;
    delete q;
  });
  int64 end_time = GetCurrentTimeMicros();
  LOG(INFO) << "Inference took " << (end_time - start_time) / 1000 << "ms for gain of " << score_gain << ".";

}

void Train(RecordInput* input, GraphInference* inference) {
  inference->SSVMInit(FLAGS_regularization_const, FLAGS_svm_margin);
  double learning_rate = FLAGS_start_learning_rate;
  LOG(INFO) << "Starting training with --start_learning_rate=" << std::fixed << FLAGS_start_learning_rate
      << ", --regularization_const=" << std::fixed << FLAGS_regularization_const
      << " and --svm_margin=" << std::fixed << FLAGS_svm_margin;
  double last_error_rate = 1.0;
  for (int pass = 0; pass < FLAGS_num_training_passes; ++pass) {
    double error_rate = 0.0;

    GraphInference backup_inference(*inference);

    int64 start_time = GetCurrentTimeMicros();
    PrecisionStats stats;
    ParallelForeachInput(input, [&inference,&stats,learning_rate](const Json::Value& query, const Json::Value& assign) {
      std::unique_ptr<Nice2Query> q(inference->CreateQuery());
      q->FromJSON(query);
      std::unique_ptr<Nice2Assignment> a(inference->CreateAssignment(q.get()));
      a->FromJSON(assign);
      inference->SSVMLearn(q.get(), a.get(), learning_rate, &stats);
    });
    int64 end_time = GetCurrentTimeMicros();
    LOG(INFO) << "Training pass took " << (end_time - start_time) / 1000 << "ms.";

    LOG(INFO) << "Correct " << stats.correct_labels << " vs " << stats.incorrect_labels << " incorrect labels.";
    error_rate = stats.incorrect_labels / (static_cast<double>(stats.incorrect_labels + stats.correct_labels));
    LOG(INFO) << "Pass " << pass << " with learning rate " << learning_rate << " has error rate of " << std::fixed << error_rate;
    if (error_rate > last_error_rate) {
      LOG(INFO) << "Reverting last pass.";
      learning_rate *= 0.5;  // Halve the learning rate.
      *inference = backup_inference;
      if (learning_rate < FLAGS_stop_learning_rate) break;  // Stop learning in this case.
    } else {
      last_error_rate = error_rate;
    }
    inference->PrepareForInference();
  }
}

SingleLabelErrorStats* CreateLabelErrorStats() {
  if (FLAGS_output_errors.empty()) return nullptr;
  if (FLAGS_output_errors == "-")
    LOG(INFO) << "Will perform label error evaluation that will LOG the top errors.";
  else
    LOG(INFO) << "Will perform evaluation that will output to " << FLAGS_output_errors;
  return new SingleLabelErrorStats();
}

void PrintLabelErrorStatsSummary(const SingleLabelErrorStats* stats) {
  if (stats == nullptr) return;
  LOG(INFO) << "Counting classification errors...";
  std::vector<std::pair<int, std::string> > best_stats;
  for (auto it = stats->errors_and_counts.begin(); it != stats->errors_and_counts.end(); ++it) {
    best_stats.push_back(std::pair<int, std::string>(it->second, it->first));
  }
  std::sort(best_stats.begin(), best_stats.end(), std::greater<std::pair<int, std::string> >());

  std::string summary = "Top classification errors done by label (expected -> predicted):";
  for (size_t i = 0; i < 32 && i < best_stats.size(); ++i) {
    StringAppendF(&summary, "\n%8d : %s", best_stats[i].first, best_stats[i].second.c_str());
  }
  LOG(INFO) << summary;
}

void OutputLabelErrorStats(const SingleLabelErrorStats* stats) {
  if (stats == nullptr) return;
  if (FLAGS_output_errors == "-") return;
  LOG(INFO) << "Outputting error stats to " << FLAGS_output_errors << "...";

  std::vector<std::pair<int, std::string> > best_stats;
  for (auto it = stats->errors_and_counts.begin(); it != stats->errors_and_counts.end(); ++it) {
    best_stats.push_back(std::pair<int, std::string>(it->second, it->first));
  }
  std::sort(best_stats.begin(), best_stats.end(), std::greater<std::pair<int, std::string> >());
  FILE* f = fopen(FLAGS_output_errors.c_str(), "wt");
  for (size_t i = 0; i < best_stats.size(); ++i) {
    fprintf(f, "%8d : %s\n", best_stats[i].first, best_stats[i].second.c_str());
  }
  fclose(f);
  LOG(INFO) << "Error stats written.";
}

void Evaluate(RecordInput* evaluation_data, GraphInference* inference,
    PrecisionStats* total_stats, SingleLabelErrorStats* error_stats) {
  int64 start_time = GetCurrentTimeMicros();
  PrecisionStats stats;
  ParallelForeachInput(evaluation_data, [&inference,&stats,error_stats](const Json::Value& query, const Json::Value& assign) {
    std::unique_ptr<Nice2Query> q(inference->CreateQuery());
    q->FromJSON(query);
    std::unique_ptr<Nice2Assignment> a(inference->CreateAssignment(q.get()));
    a->FromJSON(assign);
    std::unique_ptr<Nice2Assignment> refa(inference->CreateAssignment(q.get()));
    refa->FromJSON(assign);

    a->ClearInferredAssignment();
    inference->MapInference(q.get(), a.get());
    a->CompareAssignments(refa.get(), &stats);
    if (error_stats != nullptr)
      a->CompareAssignmentErrors(refa.get(), error_stats);
  });
  int64 end_time = GetCurrentTimeMicros();
  LOG(INFO) << "Evaluation pass took " << (end_time - start_time) / 1000 << "ms.";


  LOG(INFO) << "Correct " << stats.correct_labels << " vs " << stats.incorrect_labels << " incorrect labels";
  double error_rate = stats.incorrect_labels / (static_cast<double>(stats.incorrect_labels + stats.correct_labels));
  LOG(INFO) << "Error rate of " << std::fixed << error_rate;
  PrintLabelErrorStatsSummary(error_stats);

  total_stats->AddStats(stats);
}


int main(int argc, char** argv) {
  google::InstallFailureSignalHandler();
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  if (FLAGS_cross_validation_folds > 1) {
    PrecisionStats total_stats;
    std::unique_ptr<SingleLabelErrorStats> error_stats(CreateLabelErrorStats());
    for (int fold_id = 0; fold_id < FLAGS_cross_validation_folds; ++fold_id) {
      GraphInference inference;
      std::unique_ptr<RecordInput> training_data(
          new ShuffledCacheInput(new CrossValidationInput(new FileRecordInput(FLAGS_input),
              fold_id, FLAGS_cross_validation_folds, true)));
      std::unique_ptr<RecordInput> validation_data(
          new ShuffledCacheInput(new CrossValidationInput(new FileRecordInput(FLAGS_input),
              fold_id, FLAGS_cross_validation_folds, true)));
      LOG(INFO) << "Training fold " << fold_id;
      InitTrain(training_data.get(), &inference);
      Train(training_data.get(), &inference);
      LOG(INFO) << "Evaluating fold " << fold_id;
      Evaluate(validation_data.get(), &inference, &total_stats, error_stats.get());
    }
    // Output results of cross-validation (no model is saved in this mode).
    LOG(INFO) << "========================================";
    LOG(INFO) << "Cross-validation done";
    LOG(INFO) << "Correct " << total_stats.correct_labels << " vs " << total_stats.incorrect_labels << " incorrect labels for the whole dataset";
    double error_rate = total_stats.incorrect_labels / (static_cast<double>(total_stats.incorrect_labels + total_stats.correct_labels));
    LOG(INFO) << "Error rate of " << std::fixed << error_rate;
    OutputLabelErrorStats(error_stats.get());
  } else if (FLAGS_evaluate) {
    std::unique_ptr<SingleLabelErrorStats> error_stats(CreateLabelErrorStats());
    GraphInference inference;
    std::unique_ptr<RecordInput> input(new FileRecordInput(FLAGS_input));
    inference.LoadModel(FLAGS_out_model);
    PrecisionStats total_stats;
    Evaluate(input.get(), &inference, &total_stats, error_stats.get());
    OutputLabelErrorStats(error_stats.get());
    // No need to print total_stats. Evaluate() already prints info.
  } else {
    // Structured training.
    GraphInference inference;
    std::unique_ptr<RecordInput> input(new ShuffledCacheInput(new FileRecordInput(FLAGS_input)));
    InitTrain(input.get(), &inference);
    Train(input.get(), &inference);
    // Save the model in the regular training.
    inference.SaveModel(FLAGS_out_model);
  }

  return 0;
}
