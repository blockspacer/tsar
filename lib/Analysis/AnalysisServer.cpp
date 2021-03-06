//===--- AnalysisServer.cpp ---- Analysis Server ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements base representation of analysis server and a server pass
// which could be used to send response.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/AnalysisServer.h"

using namespace llvm;

template<> char AnalysisClientServerMatcherWrapper::ID = 0;
INITIALIZE_PASS(AnalysisClientServerMatcherWrapper, "analysis-cs-matcher-iw",
  "Analysis Client Server Matcher (Wrapper)", true, true)

ImmutablePass * llvm::createAnalysisClientServerMatcherWrapper(
    ValueToValueMapTy &OriginToClone) {
  initializeAnalysisClientServerMatcherWrapperPass(
    *PassRegistry::getPassRegistry());
  auto P = new AnalysisClientServerMatcherWrapper;
  P->set(OriginToClone);
  return P;
}

