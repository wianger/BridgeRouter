#ifndef ALIAS_ANALYZER_H
#define ALIAS_ANALYZER_H

#include "GlobalCtx.h"
#include "Common.h"

using namespace llvm;

class PointerAnalysisPass : public IterativeModulePass {

private:
    void detectAliasPointers(Function* F, AAResults &AAR, PointerAnalysisMap &aliasPtrs);
    void* getSourcePointer(Value* P);

public:

    PointerAnalysisPass(GlobalContext *Ctx_)
        : IterativeModulePass(Ctx_, "PointerAnalysis") {}
    virtual bool doInitialization(Module*);
    virtual bool doFinalization(Module*);
    virtual bool doModulePass(Module*);

};

#endif
