#include "jitprof.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include "codegen_shared.h"

cl::opt<bool> ForceProfileAllocations("julia-force-profile-allocations", cl::init(false), cl::Hidden);
cl::opt<bool> ForceProfileCalls("julia-force-profile-calls", cl::init(false), cl::Hidden);
cl::opt<bool> ForceProfileBranches("julia-force-profile-branches", cl::init(false), cl::Hidden);
using namespace llvm;

static auto extractProfNum(MDTuple *MD, unsigned Idx) {
    auto *MDV = cast<ConstantAsMetadata>(MD->getOperand(Idx))->getValue();
    return cast<ConstantInt>(MDV);
}

static ProfilingFlags fromFunction(Function &F) {

    ProfilingFlags Flags = {false, false, false, false};
    if (auto ProfMD = F.getMetadata("julia.prof")) {
        return ProfilingFlags::fromMDNode(ProfMD);
    }
    Flags.ProfileAllocations |= ForceProfileAllocations;
    Flags.ProfileBranches |= ForceProfileBranches;
    Flags.ProfileCalls |= ForceProfileCalls;
    return Flags;
}

ProfilingFlags ProfilingFlags::fromMDNode(MDNode *MDN) {
    auto *MD = cast<MDTuple>(MDN);
    assert(MD->getNumOperands() == 4);
    return ProfilingFlags{
        !!extractProfNum(MD, 0)->getZExtValue(),
        !!extractProfNum(MD, 1)->getZExtValue(),
        !!extractProfNum(MD, 2)->getZExtValue(),
        !!extractProfNum(MD, 3)->getZExtValue(),
    };
}

MDNode *ProfilingFlags::toMDNode(LLVMContext &Ctx) {
    return MDTuple::get(Ctx, {
        ConstantAsMetadata::get(ConstantInt::getBool(Ctx, ProfileAllocations)),
        ConstantAsMetadata::get(ConstantInt::getBool(Ctx, ProfileBranches)),
        ConstantAsMetadata::get(ConstantInt::getBool(Ctx, ProfileCalls)),
        ConstantAsMetadata::get(ConstantInt::getBool(Ctx, ApplyPGO)),
    });
}

static void addCallInstrumentation(FunctionProfile &Prof, Function &F) {
    IRBuilder<> Builder(&*F.getEntryBlock().getFirstInsertionPt());
    auto CallPtr = ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt64Ty(F.getContext()), (uint64_t)&Prof.CallCount), Type::getInt64PtrTy(F.getContext()));
    //Increment call count by one
    auto CallCount = Builder.CreateAtomicRMW(AtomicRMWInst::Add, CallPtr, ConstantInt::get(Type::getInt64Ty(F.getContext()), 1), Align(alignof(decltype(Prof.CallCount))), AtomicOrdering::Monotonic);

    //We might also care about reporting when we hit a certain threshold of calls, so we add an additional piece of metadata
    //for threshold, function, and arguments
    auto ReporterMDRaw = F.getMetadata("julia.prof.reporter");
    if (ReporterMDRaw) {
        auto ReporterMD = cast<MDTuple>(ReporterMDRaw);
        assert(ReporterMD->getNumOperands() >= 2);
        auto Limit = extractProfNum(ReporterMD, 0);
        auto Report = Builder.CreateICmpEQ(CallCount, Limit);
        Builder.SetInsertPoint(SplitBlockAndInsertIfThen(Report, &*Builder.GetInsertPoint(), false, MDBuilder(F.getContext()).createBranchWeights(1, Limit->getSExtValue() - Prof.CallCount.load(std::memory_order::memory_order_relaxed))));
        SmallVector<Value *, 8> Args;
        SmallVector<Type *, 8> Types;
        //Collect all the arguments and types
        for (unsigned i = 2; i < ReporterMD->getNumOperands(); i++) {
            auto *Arg = cast<ConstantAsMetadata>(ReporterMD->getOperand(i))->getValue();
            Args.push_back(Arg);
            Types.push_back(Arg->getType());
        }
        auto ReporterFT = FunctionType::get(Type::getVoidTy(F.getContext()), Types, false);
        auto ReporterF = ConstantExpr::getIntToPtr(extractProfNum(ReporterMD, 1), ReporterFT->getPointerTo());
        Builder.CreateCall(ReporterFT, ReporterF, Args);
    }
}

static void addBranchInstrumentation(FunctionProfile &Prof, Function &F) {
    std::lock_guard<std::mutex> Lock(Prof.BranchesMutex);
    if (Prof.BranchProfiles.empty()) {
        Prof.BranchProfiles.resize(F.size());
    }
    assert(Prof.BranchProfiles.size() == F.size());
    size_t BBidx = 0;
    for (auto &BB : F) {
        if (auto Br = dyn_cast<BranchInst>(BB.getTerminator())) {
            if (Br->isUnconditional() || isa<Constant>(Br->getCondition()))
                continue;
            auto &BI = Prof.BranchProfiles[BBidx];
            if (!BI) {
                BI = std::make_unique<FunctionProfile::BranchInfo>();
                BI->Taken = 0;
                BI->Total = 0;
            }
            auto TakenPtr = ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt64Ty(F.getContext()), (uint64_t)&BI->Taken), Type::getInt64PtrTy(F.getContext()));
            auto TotalPtr = ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt64Ty(F.getContext()), (uint64_t)&BI->Total), Type::getInt64PtrTy(F.getContext()));
            IRBuilder<> Builder(Br);
            Builder.CreateAtomicRMW(AtomicRMWInst::Add, TotalPtr, ConstantInt::get(Type::getInt64Ty(F.getContext()), 1), Align(alignof(decltype(BI->Total))), AtomicOrdering::Monotonic);
            Builder.CreateAtomicRMW(AtomicRMWInst::Add, TakenPtr, Builder.CreateZExt(Br->getCondition(), Type::getInt64Ty(F.getContext())), Align(alignof(decltype(BI->Taken))), AtomicOrdering::Monotonic);
        }
        BBidx++;
    }
}

static void applyPGOInstrumentation(FunctionProfile &Prof, Function &F) {
    auto Calls = Prof.CallCount.load(std::memory_order::memory_order_relaxed);
    auto MDB = MDBuilder(F.getContext());
    F.setMetadata(LLVMContext::MD_prof, MDB.createFunctionEntryCount(Calls, false, nullptr));
    std::lock_guard<std::mutex> (Prof.BranchesMutex);
    uint64_t BBidx = 0;
    for (auto &BB : F) {
        auto &Branch = Prof.BranchProfiles[BBidx];
        auto Taken = Branch->Taken.load(std::memory_order::memory_order_relaxed);
        auto Total = Branch->Total.load(std::memory_order::memory_order_relaxed);
        if (Total > 0) {
            auto MD = MDB.createBranchWeights(Taken, Total - Taken);
            BB.getTerminator()->setMetadata(LLVMContext::MD_prof, MD);
        }
        BBidx++;
    }
}

static void addAllocInstrumentation(FunctionProfile::AllocInfo &Prof, Function &F, bool Preopt) {
    auto Size = ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt64Ty(F.getContext()), (uint64_t)&Prof.Size), Type::getInt64PtrTy(F.getContext()));
    auto Count = ConstantExpr::getIntToPtr(ConstantInt::get(Type::getInt64Ty(F.getContext()), (uint64_t)&Prof.Count), Type::getInt64PtrTy(F.getContext()));
    for (auto &BB : F) {
        for (auto &I : BB) {
            if (auto CI = dyn_cast<CallInst>(&I)) {
                if (CI->getCalledFunction()) {
                    bool Match = false;
                    if (Preopt) {
                        Match = CI->getCalledFunction()->getName() == "julia.gc_alloc_obj";
                    }
                    else {
                        auto Name = CI->getCalledFunction()->getName();
                        Match = Name.contains("jl_gc_pool_alloc") || Name.contains("jl_gc_big_alloc");
                    }
                    if (Match) {
                        IRBuilder<> Builder(CI);
                        //Increment size by the size of the allocation
                        Builder.CreateAtomicRMW(AtomicRMWInst::Add, Size, Builder.CreateIntCast(CI->getArgOperand(1), Type::getInt64Ty(F.getContext()), false), Align(alignof(decltype(Prof.Size))), AtomicOrdering::Monotonic);
                        //Increment count by one
                        Builder.CreateAtomicRMW(AtomicRMWInst::Add, Count, ConstantInt::get(Type::getInt64Ty(F.getContext()), 1), Align(alignof(decltype(Prof.Count))), AtomicOrdering::Monotonic);
                    }
                }
            }
        }
    }
}

PreservedAnalyses JITPostoptimizationProfiler::run(Function &F, FunctionAnalysisManager &FAM) {
    static_assert(sizeof(void*) == sizeof(uint64_t), "This code assumes 64-bit pointers");
    if (!JITProf)
        return PreservedAnalyses::all();

    auto Flags = fromFunction(F);

    if (!Flags.ProfileAllocations && !Flags.ProfileCalls)
        return PreservedAnalyses::all();
    
    auto Prof = JITProf->getProfile(F.getName());
    if (!Prof)
        return PreservedAnalyses::all();

    if (Flags.ProfileCalls) {
        addCallInstrumentation(*Prof, F);
    }

    if (Flags.ProfileAllocations) {
        addAllocInstrumentation(Prof->Postopt, F, false);
    }

    //Call instrumentation could add a branch, invalidating the CFG
    return PreservedAnalyses();
}

PreservedAnalyses JITPreoptimizationProfiler::run(Function &F, FunctionAnalysisManager &FAM) {
    static_assert(sizeof(void*) == sizeof(uint64_t), "This code assumes 64-bit pointers");
    if (!JITProf)
        return PreservedAnalyses::all();

    auto Flags = fromFunction(F);

    if (!Flags.ProfileBranches && !Flags.ProfileAllocations && !Flags.ApplyPGO)
        return PreservedAnalyses::all();
    
    auto &Prof = JITProf->getOrCreateProfile(F.getName(), [&]() {
        auto FP = std::make_unique<FunctionProfile>();
        FP->Loops = FAM.getResult<LoopAnalysis>(F).getLoopsInPreorder().size();
        FP->BBs = F.size();
        FP->Insts = std::accumulate(F.begin(), F.end(), 0u, [](unsigned Acc, const BasicBlock &BB) {
            return Acc + BB.size();
        });
        return FP;
    });

    if (Flags.ProfileBranches) {
        addBranchInstrumentation(Prof, F);
    }

    if (Flags.ProfileAllocations) {
        addAllocInstrumentation(Prof.Preopt, F, true);
    }

    if (Flags.ApplyPGO) {
        applyPGOInstrumentation(Prof, F);
    }

    //If we apply PGO we must clobber almost everything due to branch weights updating
    return PreservedAnalyses::none();
}

void JITFunctionProfiler::dump(raw_ostream &OS) const {
    std::lock_guard<std::mutex> Lock(FunctionProfilesMutex);
    OS << '[';
    size_t Count = FunctionProfiles.size();
    for (const auto &KV : FunctionProfiles) {
        OS <<
        '{' <<
            "\"Name\":\"" << KV.first() << "\",";
        if (KV.second->CallCount) {
            OS << "\"Calls\":" << KV.second->CallCount << ",";
        }
        if (KV.second->Preopt.Count || KV.second->Postopt.Count) {
            OS <<
            "\"Allocs\":["
                "{\"Size\":" << KV.second->Preopt.Size << ",\"Count\":" << KV.second->Preopt.Count << "},"
                "{\"Size\":" << KV.second->Postopt.Size << ",\"Count\":" << KV.second->Postopt.Count << "}"
            << "],";
        }
        {
            std::lock_guard<std::mutex> Lock(KV.second->BranchesMutex);
            if (!KV.second->BranchProfiles.empty()) {
                size_t Count = KV.second->BranchProfiles.size();
                bool Comma = false;
                for (size_t Idx = 0; Idx < Count; Idx++) {
                    const auto &Branch = KV.second->BranchProfiles[Idx];
                    if (!Branch)
                        continue;
                    if (Comma) {
                        OS << ',';
                    } else {
                        OS << "\"Branches\":[";
                    }
                    OS << "{\"Idx\":" << Idx << ",\"Taken\":" << Branch->Taken << ",\"Total\":" << Branch->Total << "}";
                    Comma = true;
                }
                if (Comma) {
                    OS << "],";
                }
            }
        }
        if (KV.second->Loops)
            OS << "\"Loops\":" << KV.second->Loops << ",";
        OS <<
            "\"BBs\":" << KV.second->BBs << "," <<
            "\"Insts\":" << KV.second->Insts << '}';
        if (--Count) {
            OS << ",";
        }
    }
    OS << ']' << '\n';
}

FunctionProfile *JITFunctionProfiler::getProfile(StringRef Name) {
    std::lock_guard<std::mutex> Lock(FunctionProfilesMutex);
    auto It = FunctionProfiles.find(Name);
    if (It == FunctionProfiles.end())
        return nullptr;
    return It->second.get();
}

FunctionProfile &JITFunctionProfiler::getOrCreateProfile(StringRef Name, unique_function<std::unique_ptr<FunctionProfile>()> Create) {
    std::lock_guard<std::mutex> Lock(FunctionProfilesMutex);
    auto It = FunctionProfiles.find(Name);
    if (It == FunctionProfiles.end()) {
        auto Prof = Create();
        It = FunctionProfiles.insert(std::make_pair(Name, std::move(Prof))).first;
    }
    return *It->second;
}