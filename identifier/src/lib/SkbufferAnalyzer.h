/*
 * Identify skb-like heap buffers embedded in kernel structures.
 */

#ifndef SKBUFFER_ANALYZER_H_
#define SKBUFFER_ANALYZER_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "GlobalCtx.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

class SkbufferAnalyzerPass : public IterativeModulePass {
 private:
  void runOnFunction(llvm::Function *F);
  bool isCall2UserAccess(std::string calleeName);
  bool isUserReadCall(std::string calleeName);
  bool isUserWriteCall(std::string calleeName);
  bool isPriviledged(llvm::Function *F);

  struct BufferAccessSummary {
    unsigned bufferArgNo = 0;
    unsigned lenArgNo = 0;
    bool userRead = false;
    bool userWrite = false;
  };

  struct StructAccessSummary {
    llvm::StructType *stType = nullptr;
    unsigned objectArgNo = 0;
    unsigned fieldOffset = 0;
    int offsetArgNo = -1;
    int lenArgNo = -1;
    bool userRead = false;
    bool userWrite = false;
  };

  typedef std::map<std::string, std::vector<BufferAccessSummary> >
      BufferSummaryMap;
  typedef std::map<std::string, std::vector<StructAccessSummary> >
      StructSummaryMap;

  BufferSummaryMap bufferSummaryMap;
  StructSummaryMap structSummaryMap;

  llvm::Value *getAccessBase(llvm::Value *V, int64_t *byteOffset);
  llvm::StructType *getStructFieldAccess(llvm::Value *V, llvm::Module *M,
                                         unsigned *fieldOffset,
                                         int64_t *byteOffset,
                                         llvm::Value **objectValue = nullptr);
  bool isZeroOffsetAccess(llvm::Value *V);
  bool sizeCoversAccess(llvm::Value *allocSize, llvm::Value *accessLen);
  bool valuesMayMatch(llvm::Value *A, llvm::Value *B);
  bool valueDependsOn(llvm::Value *V, llvm::Value *Needle,
                      std::set<llvm::Value *> &seen);
  llvm::Value *findStoredValue(llvm::Value *V);
  llvm::Value *resolveAllocSize(llvm::Value *allocSize,
                                llvm::Value *objectValue);
  llvm::Optional<uint64_t> getConstantIntValue(llvm::Value *V);
  llvm::Optional<uint64_t> getConstantValueOrStoredValue(llvm::Value *V);
  llvm::StructType *getStructPointerType(llvm::Value *V);
  int getArgumentIndex(llvm::Value *V);
  int getDependentArgumentIndex(llvm::Function *F, llvm::Value *V);
  bool getUserAccessInfo(llvm::CallInst *callInst, std::string calleeName,
                         llvm::Value **buffer, llvm::Value **len,
                         bool *userRead, bool *userWrite);
  bool getStructFieldAccessPattern(llvm::Value *V, llvm::StructType **stType,
                                   unsigned *fieldOffset, int *objectArgNo,
                                   int *offsetArgNo);
  bool addBufferSummary(std::string funcName, BufferAccessSummary summary);
  bool addStructSummary(std::string funcName, StructAccessSummary summary);
  bool collectFunctionSummaries(llvm::Function *F);
  bool collectStructSummaryFromAccess(llvm::Function *F, llvm::Value *buffer,
                                      llvm::Value *len, bool userRead,
                                      bool userWrite);
  bool collectStructSummaryFromCall(llvm::Function *F,
                                    llvm::CallInst *callInst,
                                    StructAccessSummary calleeSummary);
  void applyStructSummary(llvm::CallInst *callInst,
                          StructAccessSummary summary);
  bool recordSkbufferInfo(llvm::CallInst *callInst, llvm::StructType *stType,
                          unsigned fieldOffset, llvm::Value *objectValue,
                          llvm::Value *len, bool userRead, bool userWrite,
                          bool allowSeparatedPaths = false);
  void analyzeAccess(llvm::CallInst *callInst, std::string calleeName);

  FuncSet getSyscalls(llvm::Function *F);
  FuncSet reachableSyscall(llvm::Function *F);

  std::vector<std::string> rootSyscall = {
      "sys_reboot",        "sys_kexec_load",
      "sys_swapon",        "sys_swapoff",
      "sys_umount",        "sys_oldumount",
      "sys_quotactl",      "sys_mount",
      "sys_pivot_root",    "sys_lookup_dcookie",
      "sys_bdflush",       "sys_seccomp",
      "sys_finit_module",  "sys_init_module",
      "sys_delete_module", "sys_open_by_handle_at",
      "sys_fchown",        "sys_fchown16",
      "sys_fchownat",      "sys_lchown",
      "sys_lchown16",      "sys_chown",
      "sys_chown16",       "sys_fchmodat",
      "sys_chmod",         "sys_fchmod",
      "sys_acct",          "sys_settimeofday",
      "sys_stime",         "sys_adjtimex",
      "sys_chroot",        "sys_syslog"};

  typedef llvm::DenseMap<llvm::Function *, FuncSet> ReachableSyscallCache;
  ReachableSyscallCache reachableSyscallCache;

 public:
  SkbufferAnalyzerPass(GlobalContext *Ctx_)
      : IterativeModulePass(Ctx_, "SkbufferAnalyzer") {}
  virtual bool doInitialization(llvm::Module *M);
  virtual bool doFinalization(llvm::Module *M);
  virtual bool doModulePass(llvm::Module *M);
  void dumpSkbuffers(bool dumpSkbufferOnly = false);
};

#endif
