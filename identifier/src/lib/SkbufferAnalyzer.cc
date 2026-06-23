/*
 * Identify skb-like heap buffers embedded in kernel structures.
 */

#include "SkbufferAnalyzer.h"

#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

#include "Annotation.h"
#include "Common.h"
#include "StructAnalyzer.h"

using namespace llvm;
using namespace std;

extern cl::opt<bool> IgnoreReachable;

bool SkbufferAnalyzerPass::doInitialization(Module *M) { return false; }

bool SkbufferAnalyzerPass::doModulePass(Module *M) {
  bool changed = false;
  for (Function &F : *M) changed |= collectFunctionSummaries(&F);
  for (Function &F : *M) runOnFunction(&F);
  return changed;
}

bool SkbufferAnalyzerPass::doFinalization(Module *M) { return false; }

bool SkbufferAnalyzerPass::isCall2UserAccess(std::string calleeName) {
  if (calleeName == "copy_from_user" || calleeName == "_copy_from_user")
    return true;
  if (calleeName == "copy_to_user" || calleeName == "_copy_to_user")
    return true;
  return false;
}

bool SkbufferAnalyzerPass::isUserReadCall(std::string calleeName) {
  return calleeName == "copy_to_user" || calleeName == "_copy_to_user" ||
         calleeName == "copy_to_iter" || calleeName == "_copy_to_iter" ||
         calleeName == "simple_copy_to_iter" ||
         calleeName == "hash_and_copy_to_iter" ||
         calleeName == "csum_and_copy_to_iter";
}

bool SkbufferAnalyzerPass::isUserWriteCall(std::string calleeName) {
  return calleeName == "copy_from_user" || calleeName == "_copy_from_user" ||
         calleeName == "copy_from_iter" || calleeName == "_copy_from_iter";
}

bool SkbufferAnalyzerPass::isPriviledged(Function *F) {
  SmallVector<Function *, 4> workList;
  workList.push_back(F);

  FuncSet seen;
  while (!workList.empty()) {
    Function *Cur = workList.pop_back_val();
    if (Ctx->devDenyList.find(Cur) != Ctx->devDenyList.end()) return true;
    if (!seen.insert(Cur).second) continue;

    CallerMap::iterator it = Ctx->Callers.find(Cur);
    if (it == Ctx->Callers.end()) continue;
    for (auto callerInst : it->second) {
      workList.push_back(callerInst->getParent()->getParent());
    }
  }
  return false;
}

void SkbufferAnalyzerPass::runOnFunction(Function *F) {
  if (!IgnoreReachable) {
    FuncSet Syscalls = reachableSyscall(F);
    if (Syscalls.size() == 0) return;
  }

  if (F->hasSection() && F->getSection().str() == ".init.text") return;
  if (isPriviledged(F)) return;

  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; i++) {
    Instruction *I = &*i;
    if (CallInst *callInst = dyn_cast<CallInst>(I)) {
      const Function *callee = callInst->getCalledFunction();
      if (!callee)
        callee = dyn_cast<Function>(
            callInst->getCalledOperand()->stripPointerCasts());
      if (!callee) continue;

      std::string calleeName = callee->getName().str();
      if (isCall2UserAccess(calleeName)) analyzeAccess(callInst, calleeName);

      auto summaryIt = structSummaryMap.find(calleeName);
      if (summaryIt == structSummaryMap.end()) continue;
      for (auto const &summary : summaryIt->second)
        applyStructSummary(callInst, summary);
    }
  }
}

Value *SkbufferAnalyzerPass::getAccessBase(Value *V, int64_t *byteOffset) {
  if (!V) return nullptr;
  V = V->stripPointerCasts();
  if (byteOffset) *byteOffset = 0;

  while (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
    APInt offset(64, 0, true);
    const DataLayout &DL = GEP->getModule()->getDataLayout();
    if (!GEP->accumulateConstantOffset(DL, offset)) return nullptr;
    if (byteOffset) *byteOffset += offset.getSExtValue();
    V = GEP->getPointerOperand()->stripPointerCasts();
  }
  return V;
}

StructType *SkbufferAnalyzerPass::getStructFieldAccess(Value *V, Module *M,
                                                       unsigned *fieldOffset,
                                                       int64_t *byteOffset,
                                                       Value **objectValue) {
  if (!V) return nullptr;

  int64_t accessByteOffset = 0;
  Value *base = getAccessBase(V, &accessByteOffset);
  if (!base || accessByteOffset != 0) return nullptr;

  base = base->stripPointerCasts();
  if (auto *LI = dyn_cast<LoadInst>(base)) {
    V = LI->getPointerOperand()->stripPointerCasts();
  } else {
    return nullptr;
  }

  int64_t totalByteOffset = 0;

  while (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
    APInt offset(64, 0, true);
    const DataLayout &DL = GEP->getModule()->getDataLayout();
    if (!GEP->accumulateConstantOffset(DL, offset)) return nullptr;
    totalByteOffset += offset.getSExtValue();

    if (GEP->getNumIndices() >= 2) {
      auto *ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
      if (!ptrType) return nullptr;
      auto *stType = dyn_cast<StructType>(ptrType->getPointerElementType());
      if (stType) {
        auto *CI = dyn_cast<ConstantInt>(GEP->getOperand(2));
        if (!CI) return nullptr;
        if (fieldOffset) *fieldOffset = CI->getZExtValue();
        if (byteOffset) *byteOffset = totalByteOffset;
        if (objectValue) *objectValue = GEP->getPointerOperand();
        return stType;
      }
    }

    V = GEP->getPointerOperand()->stripPointerCasts();
  }
  return nullptr;
}

bool SkbufferAnalyzerPass::isZeroOffsetAccess(Value *V) {
  int64_t byteOffset = 0;
  Value *base = getAccessBase(V, &byteOffset);
  return base && byteOffset == 0;
}

Optional<uint64_t> SkbufferAnalyzerPass::getConstantIntValue(Value *V) {
  if (!V) return None;
  V = V->stripPointerCasts();
  if (auto *CI = dyn_cast<ConstantInt>(V)) return CI->getZExtValue();
  return None;
}

Optional<uint64_t> SkbufferAnalyzerPass::getConstantValueOrStoredValue(
    Value *V) {
  Optional<uint64_t> constValue = getConstantIntValue(V);
  if (constValue) return constValue;

  Value *storedValue = findStoredValue(V);
  if (!storedValue || storedValue == V) return None;
  return getConstantIntValue(storedValue);
}

bool SkbufferAnalyzerPass::valueDependsOn(Value *V, Value *Needle,
                                          std::set<Value *> &seen) {
  if (!V || !Needle) return false;
  V = V->stripPointerCasts();
  Needle = Needle->stripPointerCasts();
  if (V == Needle) return true;
  if (!seen.insert(V).second) return false;

  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *storedValue = findStoredValue(LI);
    if (storedValue && storedValue != V)
      return valueDependsOn(storedValue, Needle, seen);
  }

  if (auto *I = dyn_cast<Instruction>(V)) {
    for (unsigned i = 0, e = I->getNumOperands(); i != e; i++) {
      if (valueDependsOn(I->getOperand(i), Needle, seen)) return true;
    }
  }
  return false;
}

bool SkbufferAnalyzerPass::valuesMayMatch(Value *A, Value *B) {
  if (!A || !B) return false;
  A = A->stripPointerCasts();
  B = B->stripPointerCasts();
  if (A == B) return true;

  Value *storedA = findStoredValue(A);
  Value *storedB = findStoredValue(B);
  if (storedA) storedA = storedA->stripPointerCasts();
  if (storedB) storedB = storedB->stripPointerCasts();
  return storedA && storedB && storedA == storedB;
}

Value *SkbufferAnalyzerPass::findStoredValue(Value *V) {
  if (!V) return nullptr;
  V = V->stripPointerCasts();

  auto *LI = dyn_cast<LoadInst>(V);
  if (!LI) return V;

  Value *pointer = LI->getPointerOperand()->stripPointerCasts();
  for (auto *user : pointer->users()) {
    auto *SI = dyn_cast<StoreInst>(user);
    if (!SI || SI->getPointerOperand()->stripPointerCasts() != pointer)
      continue;
    return SI->getValueOperand()->stripPointerCasts();
  }
  return V;
}

Value *SkbufferAnalyzerPass::resolveAllocSize(Value *allocSize,
                                              Value *objectValue) {
  if (!allocSize) return nullptr;
  Value *resolvedSize = findStoredValue(allocSize);
  if (!resolvedSize) resolvedSize = allocSize;

  auto *A = dyn_cast<Argument>(resolvedSize->stripPointerCasts());
  if (!A) return allocSize;

  Value *objectSource = findStoredValue(objectValue);
  auto *CI = dyn_cast_or_null<CallInst>(objectSource);
  if (!CI) return allocSize;

  Function *callee = CI->getCalledFunction();
  if (!callee)
    callee = dyn_cast<Function>(CI->getCalledOperand()->stripPointerCasts());
  if (!callee || A->getParent() != callee) return allocSize;

  unsigned argNo = A->getArgNo();
  if (argNo >= CI->arg_size()) return allocSize;
  return CI->getArgOperand(argNo);
}

bool SkbufferAnalyzerPass::sizeCoversAccess(Value *allocSize,
                                            Value *accessLen) {
  if (!accessLen) return false;
  if (!allocSize) return false;

  Optional<uint64_t> allocConst = getConstantValueOrStoredValue(allocSize);
  Optional<uint64_t> accessConst = getConstantValueOrStoredValue(accessLen);
  if (allocConst && accessConst) return *allocConst >= *accessConst;

  return valuesMayMatch(allocSize, accessLen);
}

StructType *SkbufferAnalyzerPass::getStructPointerType(Value *V) {
  if (!V) return nullptr;
  V = V->stripPointerCasts();
  auto *ptrType = dyn_cast<PointerType>(V->getType());
  if (!ptrType) return nullptr;
  return dyn_cast<StructType>(ptrType->getPointerElementType());
}

int SkbufferAnalyzerPass::getArgumentIndex(Value *V) {
  if (!V) return -1;
  V = V->stripPointerCasts();
  if (auto *A = dyn_cast<Argument>(V)) return A->getArgNo();
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *pointer = LI->getPointerOperand()->stripPointerCasts();
    for (auto *user : pointer->users()) {
      auto *SI = dyn_cast<StoreInst>(user);
      if (!SI || SI->getPointerOperand()->stripPointerCasts() != pointer)
        continue;
      return getArgumentIndex(SI->getValueOperand());
    }
  }
  return -1;
}

int SkbufferAnalyzerPass::getDependentArgumentIndex(Function *F, Value *V) {
  if (!F || !V) return -1;
  for (Argument &A : F->args()) {
    std::set<Value *> seen;
    if (valueDependsOn(V, &A, seen)) return A.getArgNo();
  }
  return -1;
}

bool SkbufferAnalyzerPass::getUserAccessInfo(CallInst *callInst,
                                             std::string calleeName,
                                             Value **buffer, Value **len,
                                             bool *userRead,
                                             bool *userWrite) {
  if (!callInst || !buffer || !len || !userRead || !userWrite) return false;

  *buffer = nullptr;
  *len = nullptr;
  *userRead = false;
  *userWrite = false;

  if (calleeName == "copy_from_user" || calleeName == "_copy_from_user") {
    if (callInst->arg_size() < 3) return false;
    *buffer = callInst->getArgOperand(0);
    *len = callInst->getArgOperand(2);
    *userWrite = true;
    return true;
  }

  if (calleeName == "copy_to_user" || calleeName == "_copy_to_user") {
    if (callInst->arg_size() < 3) return false;
    *buffer = callInst->getArgOperand(1);
    *len = callInst->getArgOperand(2);
    *userRead = true;
    return true;
  }

  if (calleeName == "copy_from_iter" || calleeName == "_copy_from_iter") {
    if (callInst->arg_size() < 3) return false;
    *buffer = callInst->getArgOperand(0);
    *len = callInst->getArgOperand(1);
    *userWrite = true;
    return true;
  }

  if (calleeName == "copy_to_iter" || calleeName == "_copy_to_iter" ||
      calleeName == "simple_copy_to_iter" ||
      calleeName == "hash_and_copy_to_iter" ||
      calleeName == "csum_and_copy_to_iter") {
    if (callInst->arg_size() < 2) return false;
    *buffer = callInst->getArgOperand(0);
    *len = callInst->getArgOperand(1);
    *userRead = true;
    return true;
  }

  return false;
}

bool SkbufferAnalyzerPass::getStructFieldAccessPattern(Value *V,
                                                       StructType **stType,
                                                       unsigned *fieldOffset,
                                                       int *objectArgNo,
                                                       int *offsetArgNo) {
  if (!V || !stType || !fieldOffset || !objectArgNo || !offsetArgNo)
    return false;

  *stType = nullptr;
  *fieldOffset = 0;
  *objectArgNo = -1;
  *offsetArgNo = -1;

  V = V->stripPointerCasts();
  if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
    if (GEP->getNumIndices() == 1) {
      *offsetArgNo = getDependentArgumentIndex(GEP->getFunction(),
                                               GEP->getOperand(1));
      V = GEP->getPointerOperand()->stripPointerCasts();
    }
  }

  if (auto *LI = dyn_cast<LoadInst>(V))
    V = LI->getPointerOperand()->stripPointerCasts();
  else
    return false;

  while (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
    if (GEP->getNumIndices() >= 2) {
      auto *ptrType = dyn_cast<PointerType>(GEP->getPointerOperandType());
      if (!ptrType) return false;
      auto *fieldStType =
          dyn_cast<StructType>(ptrType->getPointerElementType());
      auto *CI = dyn_cast<ConstantInt>(GEP->getOperand(2));
      if (fieldStType && CI) {
        int objArg = getArgumentIndex(GEP->getPointerOperand());
        if (objArg < 0) return false;
        *stType = fieldStType;
        *fieldOffset = CI->getZExtValue();
        *objectArgNo = objArg;
        return true;
      }
    }
    V = GEP->getPointerOperand()->stripPointerCasts();
  }

  return false;
}

bool SkbufferAnalyzerPass::recordSkbufferInfo(CallInst *callInst,
                                              StructType *stType,
                                              unsigned fieldOffset,
                                              Value *objectValue, Value *len,
                                              bool userRead, bool userWrite,
                                              bool allowSeparatedPaths) {
  StructInfo *stInfo =
      Ctx->structAnalyzer.getStructInfo(stType, callInst->getModule());
  if (!stInfo) {
    KA_LOGS(2, "[SKB] skip no struct info\n");
    return false;
  }

  KeyStructMap::iterator keyIt = Ctx->keyStructMap.find(stInfo->name);
  if (keyIt != Ctx->keyStructMap.end() && keyIt->second)
    stInfo = keyIt->second;

  unsigned allocFieldOffset = fieldOffset;
  if (stInfo->fieldAllocGEP.count(allocFieldOffset) == 0 &&
      stInfo->fieldAllocGEP.size() == 1) {
    allocFieldOffset = *stInfo->fieldAllocGEP.begin();
  }

  if (stInfo->fieldAllocGEP.count(allocFieldOffset) == 0) {
    KA_LOGS(2, "[SKB] skip no field alloc " << stInfo->name << "."
                                            << fieldOffset << "\n");
    return false;
  }

  auto allocInfoIt = stInfo->fieldAllocInfo.find(allocFieldOffset);
  if (allocInfoIt == stInfo->fieldAllocInfo.end()) {
    KA_LOGS(2, "[SKB] skip no field alloc info " << stInfo->name << "."
                                                 << allocFieldOffset << "\n");
    return false;
  }

  bool recorded = false;
  for (auto const &allocInfo : allocInfoIt->second) {
    Value *allocSize = resolveAllocSize(allocInfo.sizeValue, objectValue);
    bool sizeCovered = sizeCoversAccess(allocSize, len);
    // Summary-derived object access often connects allocation and user access
    // through different functions. Keep the size evidence, but allow a
    // conservative match when both allocation and access length exist.
    if (!sizeCovered && allowSeparatedPaths)
      sizeCovered = allocInfo.allocInst && len;
    if (!sizeCovered) {
      KA_LOGS(2, "[SKB] skip size mismatch " << stInfo->name << "."
                                             << fieldOffset << "\n");
      continue;
    }

    KA_LOGS(1, "[SKB] " << stInfo->name << "." << fieldOffset << " in "
                        << callInst->getFunction()->getName() << "\n");
    stInfo->addSkbufferInfo(allocFieldOffset, callInst, allocInfo.allocInst, len,
                            allocSize, userRead, userWrite);
    Ctx->keyStructMap[stInfo->name] = stInfo;
    recorded = true;
  }

  return recorded;
}

bool SkbufferAnalyzerPass::addBufferSummary(std::string funcName,
                                            BufferAccessSummary summary) {
  std::vector<BufferAccessSummary> &summaries = bufferSummaryMap[funcName];
  for (auto const &existing : summaries) {
    if (existing.bufferArgNo == summary.bufferArgNo &&
        existing.lenArgNo == summary.lenArgNo &&
        existing.userRead == summary.userRead &&
        existing.userWrite == summary.userWrite)
      return false;
  }
  summaries.push_back(summary);
  return true;
}

bool SkbufferAnalyzerPass::addStructSummary(std::string funcName,
                                            StructAccessSummary summary) {
  std::vector<StructAccessSummary> &summaries = structSummaryMap[funcName];
  for (auto const &existing : summaries) {
    if (existing.objectArgNo == summary.objectArgNo &&
        existing.fieldOffset == summary.fieldOffset &&
        existing.offsetArgNo == summary.offsetArgNo &&
        existing.lenArgNo == summary.lenArgNo &&
        existing.userRead == summary.userRead &&
        existing.userWrite == summary.userWrite)
      return false;
  }
  summaries.push_back(summary);
  return true;
}

bool SkbufferAnalyzerPass::collectStructSummaryFromAccess(Function *F,
                                                         Value *buffer,
                                                         Value *len,
                                                         bool userRead,
                                                         bool userWrite) {
  if (!F || !buffer || !len) return false;

  StructType *stType = nullptr;
  unsigned fieldOffset = 0;
  int objectArgNo = -1;
  int offsetArgNo = -1;
  if (!getStructFieldAccessPattern(buffer, &stType, &fieldOffset, &objectArgNo,
                                   &offsetArgNo))
    return false;

  int lenArgNo = getDependentArgumentIndex(F, len);
  if (lenArgNo < 0) return false;

  StructAccessSummary summary;
  summary.stType = stType;
  summary.objectArgNo = objectArgNo;
  summary.fieldOffset = fieldOffset;
  summary.offsetArgNo = offsetArgNo;
  summary.lenArgNo = lenArgNo;
  summary.userRead = userRead;
  summary.userWrite = userWrite;
  return addStructSummary(F->getName().str(), summary);
}

bool SkbufferAnalyzerPass::collectStructSummaryFromCall(
    Function *F, CallInst *callInst, StructAccessSummary calleeSummary) {
  if (!F || !callInst) return false;
  if (calleeSummary.objectArgNo >= callInst->arg_size()) return false;
  if (calleeSummary.lenArgNo < 0 ||
      static_cast<unsigned>(calleeSummary.lenArgNo) >= callInst->arg_size())
    return false;

  Value *object = callInst->getArgOperand(calleeSummary.objectArgNo);
  int objectArgNo = getArgumentIndex(object);
  if (objectArgNo < 0) return false;

  int offsetArgNo = -1;
  if (calleeSummary.offsetArgNo >= 0 &&
      static_cast<unsigned>(calleeSummary.offsetArgNo) < callInst->arg_size())
    offsetArgNo =
        getDependentArgumentIndex(F, callInst->getArgOperand(calleeSummary.offsetArgNo));

  int lenArgNo =
      getDependentArgumentIndex(F, callInst->getArgOperand(calleeSummary.lenArgNo));
  if (lenArgNo < 0) return false;

  StructAccessSummary summary = calleeSummary;
  summary.objectArgNo = objectArgNo;
  summary.offsetArgNo = offsetArgNo;
  summary.lenArgNo = lenArgNo;
  return addStructSummary(F->getName().str(), summary);
}

bool SkbufferAnalyzerPass::collectFunctionSummaries(Function *F) {
  if (!F || F->empty()) return false;
  bool changed = false;

  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; i++) {
    auto *callInst = dyn_cast<CallInst>(&*i);
    if (!callInst) continue;

    const Function *callee = callInst->getCalledFunction();
    if (!callee)
      callee = dyn_cast<Function>(
          callInst->getCalledOperand()->stripPointerCasts());
    if (!callee) continue;

    std::string calleeName = callee->getName().str();
    Value *buffer = nullptr;
    Value *len = nullptr;
    bool userRead = false;
    bool userWrite = false;
    if (getUserAccessInfo(callInst, calleeName, &buffer, &len, &userRead,
                          &userWrite)) {
      int bufferArgNo = getArgumentIndex(buffer);
      int lenArgNo = getDependentArgumentIndex(F, len);
      if (bufferArgNo >= 0 && lenArgNo >= 0) {
        BufferAccessSummary bufferSummary;
        bufferSummary.bufferArgNo = bufferArgNo;
        bufferSummary.lenArgNo = lenArgNo;
        bufferSummary.userRead = userRead;
        bufferSummary.userWrite = userWrite;
        changed |= addBufferSummary(F->getName().str(), bufferSummary);
      }
      changed |= collectStructSummaryFromAccess(F, buffer, len, userRead,
                                                userWrite);
    }

    auto bufferIt = bufferSummaryMap.find(calleeName);
    if (bufferIt != bufferSummaryMap.end()) {
      for (auto const &summary : bufferIt->second) {
        if (summary.bufferArgNo >= callInst->arg_size() ||
            summary.lenArgNo >= callInst->arg_size())
          continue;
        changed |= collectStructSummaryFromAccess(
            F, callInst->getArgOperand(summary.bufferArgNo),
            callInst->getArgOperand(summary.lenArgNo), summary.userRead,
            summary.userWrite);
      }
    }

    auto structIt = structSummaryMap.find(calleeName);
    if (structIt != structSummaryMap.end()) {
      for (auto const &summary : structIt->second)
        changed |= collectStructSummaryFromCall(F, callInst, summary);
    }
  }

  return changed;
}

void SkbufferAnalyzerPass::applyStructSummary(CallInst *callInst,
                                              StructAccessSummary summary) {
  if (!callInst) return;
  if (summary.objectArgNo >= callInst->arg_size()) return;
  if (summary.lenArgNo < 0 ||
      static_cast<unsigned>(summary.lenArgNo) >= callInst->arg_size())
    return;

  if (summary.offsetArgNo >= 0 &&
      static_cast<unsigned>(summary.offsetArgNo) < callInst->arg_size()) {
    Optional<uint64_t> offset =
        getConstantValueOrStoredValue(callInst->getArgOperand(summary.offsetArgNo));
    if (offset && *offset != 0) return;
  }

  Value *objectValue = callInst->getArgOperand(summary.objectArgNo);
  Value *len = callInst->getArgOperand(summary.lenArgNo);
  recordSkbufferInfo(callInst, summary.stType, summary.fieldOffset, objectValue,
                     len, summary.userRead, summary.userWrite, true);
}

void SkbufferAnalyzerPass::analyzeAccess(CallInst *callInst,
                                         std::string calleeName) {
  Value *buffer = nullptr;
  Value *len = nullptr;
  bool userRead = false;
  bool userWrite = false;
  if (!getUserAccessInfo(callInst, calleeName, &buffer, &len, &userRead,
                         &userWrite))
    return;
  if (!len) return;

  if (!isZeroOffsetAccess(buffer)) {
    KA_LOGS(2, "[SKB] skip non-zero access\n");
    return;
  }

    unsigned fieldOffset = 0;
    int64_t byteOffset = 0;
    Value *objectValue = nullptr;
    StructType *stType =
        getStructFieldAccess(buffer, callInst->getModule(),
                             &fieldOffset, &byteOffset, &objectValue);
    if (!stType) {
      KA_LOGS(2, "[SKB] skip no struct field\n");
      return;
    }

    StructInfo *stInfo =
        Ctx->structAnalyzer.getStructInfo(stType, callInst->getModule());
    if (!stInfo) {
      KA_LOGS(2, "[SKB] skip no struct info\n");
      return;
    }
    if (stInfo->fieldAllocGEP.count(fieldOffset) == 0) {
      KA_LOGS(2, "[SKB] skip no field alloc " << stInfo->name << "."
                                              << fieldOffset << "\n");
      return;
    }

    auto allocInfoIt = stInfo->fieldAllocInfo.find(fieldOffset);
    if (allocInfoIt == stInfo->fieldAllocInfo.end()) {
      KA_LOGS(2, "[SKB] skip no field alloc info " << stInfo->name << "."
                                                   << fieldOffset << "\n");
      return;
    }

    for (auto const &allocInfo : allocInfoIt->second) {
      Value *allocSize = resolveAllocSize(allocInfo.sizeValue, objectValue);
      if (!sizeCoversAccess(allocSize, len)) {
        KA_LOGS(2, "[SKB] skip size mismatch " << stInfo->name << "."
                                               << fieldOffset << "\n");
        continue;
      }
      KA_LOGS(1, "[SKB] " << stInfo->name << "." << fieldOffset << " in "
                          << callInst->getFunction()->getName() << "\n");
      stInfo->addSkbufferInfo(fieldOffset, callInst, allocInfo.allocInst, len,
                              allocSize, userRead, userWrite);
      Ctx->keyStructMap[stInfo->name] = stInfo;
    }
}

FuncSet SkbufferAnalyzerPass::getSyscalls(Function *F) {
  ReachableSyscallCache::iterator it = reachableSyscallCache.find(F);
  if (it != reachableSyscallCache.end()) return it->second;
  FuncSet null;
  return null;
}

FuncSet SkbufferAnalyzerPass::reachableSyscall(Function *F) {
  ReachableSyscallCache::iterator it = reachableSyscallCache.find(F);
  if (it != reachableSyscallCache.end()) return it->second;

  FuncSet reachableFuncs;
  FuncSet reachableSyscalls;
  SmallVector<Function *, 4> workList;
  workList.push_back(F);

  while (!workList.empty()) {
    Function *Cur = workList.pop_back_val();
    if (!reachableFuncs.insert(Cur).second) continue;

    if (reachableSyscallCache.find(Cur) != reachableSyscallCache.end()) {
      FuncSet RS = getSyscalls(Cur);
      if (!RS.empty()) {
        for (auto *RF : RS) reachableFuncs.insert(RF);
        continue;
      }
    }

    CallerMap::iterator callerIt = Ctx->Callers.find(Cur);
    if (callerIt != Ctx->Callers.end()) {
      for (auto callerInst : callerIt->second) {
        workList.push_back(callerInst->getFunction());
      }
    } else {
      FuncSet &FS = Ctx->Name2Func[Cur->getName().str()];
      for (auto RF : FS) {
        callerIt = Ctx->Callers.find(RF);
        if (callerIt == Ctx->Callers.end()) continue;
        for (auto callerInst : callerIt->second) {
          workList.push_back(callerInst->getFunction());
        }
      }
    }
  }

  for (auto F : reachableFuncs) {
    StringRef funcNameRef = F->getName();
    std::string funcName = "";
    if (funcNameRef.startswith("__sys_")) {
      funcName = "sys_" + funcNameRef.str().substr(6);
    } else if (funcNameRef.startswith("__x64_sys_")) {
      funcName = "sys_" + funcNameRef.str().substr(9);
    } else if (funcNameRef.startswith("__ia32_sys")) {
      funcName = "sys_" + funcNameRef.str().substr(10);
    } else if (funcNameRef.startswith("__se_sys")) {
      funcName = "sys_" + funcNameRef.str().substr(8);
    }

    if (funcName != "") {
      if (std::find(rootSyscall.begin(), rootSyscall.end(), funcName) ==
          rootSyscall.end()) {
        reachableSyscalls.insert(F);
      }
    }
  }

  reachableSyscallCache.insert(std::make_pair(F, reachableSyscalls));
  return reachableSyscalls;
}

void SkbufferAnalyzerPass::dumpSkbuffers(bool dumpSkbufferOnly) {
  for (KeyStructMap::iterator it = Ctx->keyStructMap.begin(),
                              e = Ctx->keyStructMap.end();
       it != e; it++) {
    StructInfo *st = it->second;
    if (!st || !st->hasSkbufferInfo()) continue;
    st->dumpStructInfo(false, dumpSkbufferOnly);
  }
}
