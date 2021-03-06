//===- FuzzerInternal.h - Internal header for the Fuzzer --------*- C++ -* ===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Define the main class fuzzer::Fuzzer and most functions.
//===----------------------------------------------------------------------===//
#include <cassert>
#include <climits>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>

#include "FuzzerInterface.h"

namespace fuzzer {
typedef std::vector<uint8_t> Unit;
using namespace std::chrono;

std::string FileToString(const std::string &Path);
Unit FileToVector(const std::string &Path);
void ReadDirToVectorOfUnits(const char *Path, std::vector<Unit> *V);
void WriteToFile(const Unit &U, const std::string &Path);
void CopyFileToErr(const std::string &Path);
// Returns "Dir/FileName" or equivalent for the current OS.
std::string DirPlusFile(const std::string &DirPath,
                        const std::string &FileName);

void Mutate(Unit *U, size_t MaxLen);

void CrossOver(const Unit &A, const Unit &B, Unit *U, size_t MaxLen);

void Print(const Unit &U, const char *PrintAfter = "");
void PrintASCII(const Unit &U, const char *PrintAfter = "");
std::string Hash(const Unit &U);
void SetTimer(int Seconds);
void PrintFileAsBase64(const std::string &Path);

class Fuzzer {
 public:
  struct FuzzingOptions {
    int Verbosity = 1;
    int MaxLen = 0;
    bool DoCrossOver = true;
    int  MutateDepth = 5;
    bool ExitOnFirst = false;
    bool UseCounters = false;
    bool UseFullCoverageSet  = false;
    bool UseCoveragePairs = false;
    bool UseDFSan = false;
    int PreferSmallDuringInitialShuffle = -1;
    size_t MaxNumberOfRuns = ULONG_MAX;
    std::string OutputCorpus;
    std::vector<std::string> Tokens;
  };
  Fuzzer(UserCallback Callback, FuzzingOptions Options);
  void AddToCorpus(const Unit &U) { Corpus.push_back(U); }
  void Loop(size_t NumIterations);
  void ShuffleAndMinimize();
  void InitializeDFSan();
  size_t CorpusSize() const { return Corpus.size(); }
  void ReadDir(const std::string &Path) {
    ReadDirToVectorOfUnits(Path.c_str(), &Corpus);
  }
  // Save the current corpus to OutputCorpus.
  void SaveCorpus();

  size_t secondsSinceProcessStartUp() {
    return duration_cast<seconds>(system_clock::now() - ProcessStartTime)
        .count();
  }

  size_t getTotalNumberOfRuns() { return TotalNumberOfRuns; }

  static void StaticAlarmCallback();

  Unit SubstituteTokens(const Unit &U) const;

 private:
  void AlarmCallback();
  void ExecuteCallback(const Unit &U);
  void MutateAndTestOne(Unit *U);
  void ReportNewCoverage(size_t NewCoverage, const Unit &U);
  size_t RunOne(const Unit &U);
  void RunOneAndUpdateCorpus(const Unit &U);
  size_t RunOneMaximizeTotalCoverage(const Unit &U);
  size_t RunOneMaximizeFullCoverageSet(const Unit &U);
  size_t RunOneMaximizeCoveragePairs(const Unit &U);
  void WriteToOutputCorpus(const Unit &U);
  void WriteToCrash(const Unit &U, const char *Prefix);
  void PrintStats(const char *Where, size_t Cov, const char *End = "\n");
  void PrintUnitInASCIIOrTokens(const Unit &U, const char *PrintAfter = "");

  // Trace-based fuzzing: we run a unit with some kind of tracing
  // enabled and record potentially useful mutations. Then
  // We apply these mutations one by one to the unit and run it again.

  // Start tracing; forget all previously proposed mutations.
  void StartTraceRecording();
  // Stop tracing and return the number of proposed mutations.
  size_t StopTraceRecording();
  // Apply Idx-th trace-based mutation to U.
  void ApplyTraceBasedMutation(size_t Idx, Unit *U);

  void SetDeathCallback();
  static void StaticDeathCallback();
  void DeathCallback();
  Unit CurrentUnit;

  size_t TotalNumberOfRuns = 0;

  std::vector<Unit> Corpus;
  std::unordered_set<uintptr_t> FullCoverageSets;
  std::unordered_set<uint64_t>  CoveragePairs;

  // For UseCounters
  std::vector<uint8_t> CounterBitmap;
  size_t TotalBits() {  // Slow. Call it only for printing stats.
    size_t Res = 0;
    for (auto x : CounterBitmap) Res += __builtin_popcount(x);
    return Res;
  }

  UserCallback Callback;
  FuzzingOptions Options;
  system_clock::time_point ProcessStartTime = system_clock::now();
  system_clock::time_point UnitStartTime;
  long TimeOfLongestUnitInSeconds = 0;
};

};  // namespace fuzzer
