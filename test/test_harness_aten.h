/**
 * Copyright (c) 2017-present, Facebook, Inc.
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
#pragma once

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <ATen/ATen.h>

#include <cuda_runtime_api.h>
#include "tc/aten/aten_compiler.h"
#include "tc/core/cuda.h"
#include "tc/core/flags.h"

struct PrecisionException : public std::runtime_error {
  PrecisionException(const std::string& s) : std::runtime_error(s) {}
};

// Given the difference of output vs expected tensor, check whether the
// difference is within a relative tolerance range.
// By default we use IEEE float precision , in the future we should pull it
// from the type of the at::Tensor.
// Also allow a factor to specify the total number of reductions involved
// in each result so we can properly compute the expected precision.
bool checkRtol(
    const at::Tensor& diff,
    const std::vector<at::Tensor> inputs,
    double nOperations = 1.0,
    double machinePrecision = std::numeric_limits<float>::epsilon()) {
  double maxValue = 0.0;
  for (auto& tensor : inputs) {
    maxValue = fmax(tensor.abs().max().toFloat(), maxValue);
  }
  auto maxDiff = diff.abs().max().toFloat();
  if (maxDiff >= nOperations * machinePrecision * maxValue) {
    std::stringstream ss;
    ss << "Error at relative precision: " << machinePrecision
       << ", #operations: " << nOperations << ", maxValue: " << maxValue
       << ", maxDiff: " << maxDiff << ", random seed: " << tc::randomSeed();
    throw PrecisionException(ss.str());
  }
  return true;
}

at::Tensor subtensor(at::Tensor& tensor, int dim, int groups, int g) {
  if (!tensor.defined()) {
    return at::Tensor();
  }
  int64_t n = tensor.sizes()[dim] / groups;
  return tensor.narrow(dim, n * g, n).contiguous();
}

void setAtenSeed(uint64_t seed, at::Backend backend) {
  at::Generator& gen = at::globalContext().defaultGenerator(backend);
  gen.manualSeed(seed);
}

uint64_t getAtenSeed(at::Backend backend) {
  at::Generator& gen = at::globalContext().defaultGenerator(backend);
  return gen.seed();
}

void benchmarkKernelOptions(
    const std::string& tc,
    const std::string& name,
    const std::vector<at::Tensor>& inputs,
    const tc::MappingOptions mappingOptions) {
  tc::ATenCompilationUnit atCompl;
  atCompl.define(tc);
  auto handle = atCompl.compile(name, inputs, mappingOptions);
  std::vector<at::Tensor> outputs;
  atCompl.run(name, inputs, outputs, handle);
  for (int i = 1; i < tc::FLAGS_benchmark_warmup; ++i) {
    atCompl.run(name, inputs, outputs, handle);
  }
  std::vector<tc::Duration> kernelTimes;
  kernelTimes.reserve(tc::FLAGS_benchmark_iterations);
  std::vector<tc::Duration> totalTimes;
  totalTimes.reserve(tc::FLAGS_benchmark_iterations);
  for (int i = 0; i < tc::FLAGS_benchmark_iterations; ++i) {
    kernelTimes.push_back(atCompl.run(name, inputs, outputs, handle, true));
    TC_CUDA_RUNTIMEAPI_ENFORCE(cudaDeviceSynchronize());
    auto time(std::chrono::system_clock::now());
    atCompl.uncheckedRun(inputs, outputs, handle);
    TC_CUDA_RUNTIMEAPI_ENFORCE(cudaDeviceSynchronize());
    totalTimes.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now() - time));
  }

  auto p50idx = static_cast<int>(std::ceil(0.5 * kernelTimes.size()));
  auto p90idx = static_cast<int>(std::ceil(0.9 * kernelTimes.size()));
  auto p99idx = static_cast<int>(std::ceil(0.99 * kernelTimes.size()));

  std::sort(kernelTimes.begin(), kernelTimes.end());
#define GET_US(X) \
  (std::chrono::duration_cast<std::chrono::microseconds>((X)).count())

  std::cout << "\n---------------------------------------------------------";
  std::cout << "\n--------------------- KERNEL STATS ----------------------";
  std::cout << "\n------------------    " << tc::FLAGS_benchmark_iterations
            << " ITERATIONS    ----------------";
  std::cout << "\n---------------------------------------------------------";
  std::cout << "\n";
  std::cout
      << "Min: " << GET_US(kernelTimes.front()) << "us, "
      << "p50: "
      << GET_US(kernelTimes.at(std::min(p50idx, (int)kernelTimes.size() - 1)))
      << "us, "
      << "p90: "
      << GET_US(kernelTimes.at(std::min(p90idx, (int)kernelTimes.size() - 1)))
      << "us, "
      << "p99: "
      << GET_US(kernelTimes.at(std::min(p99idx, (int)kernelTimes.size() - 1)))
      << "us, "
      << "Max: " << GET_US(kernelTimes.back()) << "us";
  std::cout << "\n---------------------------------------------------------";
  std::cout << "\n\n";

#undef GET_US

  std::sort(totalTimes.begin(), totalTimes.end());
#define GET_US(X) \
  (std::chrono::duration_cast<std::chrono::microseconds>((X)).count())

  std::cout << "\n---------------------------------------------------------";
  std::cout << "\n-----------------------  TOTAL STATS --------------------";
  std::cout << "\n------------------    " << tc::FLAGS_benchmark_iterations
            << " ITERATIONS    ----------------";
  std::cout << "\n---------------------------------------------------------";
  std::cout << "\n";
  std::cout
      << "Min: " << GET_US(totalTimes.front()) << "us, "
      << "p50: "
      << GET_US(totalTimes.at(std::min(p50idx, (int)totalTimes.size() - 1)))
      << "us, "
      << "p90: "
      << GET_US(totalTimes.at(std::min(p90idx, (int)totalTimes.size() - 1)))
      << "us, "
      << "p99: "
      << GET_US(totalTimes.at(std::min(p99idx, (int)totalTimes.size() - 1)))
      << "us, "
      << "Max: " << GET_US(totalTimes.back()) << "us";
  std::cout << "\n---------------------------------------------------------";
  std::cout << "\n\n";

#undef GET_US
}
