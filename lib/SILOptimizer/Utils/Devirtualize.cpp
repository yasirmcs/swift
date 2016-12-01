//===--- Devirtualize.cpp - Helper for devirtualizing apply ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-devirtualize-utility"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SILOptimizer/Utils/Local.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Casting.h"
using namespace swift;

STATISTIC(NumClassDevirt, "Number of class_method applies devirtualized");
STATISTIC(NumWitnessDevirt, "Number of witness_method applies devirtualized");

//===----------------------------------------------------------------------===//
//                         Class Method Optimization
//===----------------------------------------------------------------------===//

/// Compute all subclasses of a given class.
///
/// \p CHA class hierarchy analysis
/// \p CD class declaration
/// \p ClassType type of the instance
/// \p M SILModule
/// \p Subs a container to be used for storing the set of subclasses
static void getAllSubclasses(ClassHierarchyAnalysis *CHA,
                             ClassDecl *CD,
                             SILType ClassType,
                             SILModule &M,
                             ClassHierarchyAnalysis::ClassList &Subs) {
  // Collect the direct and indirect subclasses for the class.
  // Sort these subclasses in the order they should be tested by the
  // speculative devirtualization. Different strategies could be used,
  // E.g. breadth-first, depth-first, etc.
  // Currently, let's use the breadth-first strategy.
  // The exact static type of the instance should be tested first.
  auto &DirectSubs = CHA->getDirectSubClasses(CD);
  auto &IndirectSubs = CHA->getIndirectSubClasses(CD);

  Subs.append(DirectSubs.begin(), DirectSubs.end());
  //SmallVector<ClassDecl *, 8> Subs(DirectSubs);
  Subs.append(IndirectSubs.begin(), IndirectSubs.end());

  if (isa<BoundGenericClassType>(ClassType.getSwiftRValueType())) {
    // Filter out any subclasses that do not inherit from this
    // specific bound class.
    auto RemovedIt = std::remove_if(Subs.begin(), Subs.end(),
        [&ClassType, &M](ClassDecl *Sub){
          auto SubCanTy = Sub->getDeclaredType()->getCanonicalType();
          // Unbound generic type can override a method from
          // a bound generic class, but this unbound generic
          // class is not considered to be a subclass of a
          // bound generic class in a general case.
          if (isa<UnboundGenericType>(SubCanTy))
            return false;
          // Handle the usual case here: the class in question
          // should be a real subclass of a bound generic class.
          return !ClassType.isBindableToSuperclassOf(
              SILType::getPrimitiveObjectType(SubCanTy));
        });
    Subs.erase(RemovedIt, Subs.end());
  }
}

/// \brief Returns true, if a method implementation corresponding to
/// the class_method applied to an instance of the class CD is
/// effectively final, i.e. it is statically known to be not overridden
/// by any subclasses of the class CD.
///
/// \p AI  invocation instruction
/// \p ClassType type of the instance
/// \p CD  static class of the instance whose method is being invoked
/// \p CHA class hierarchy analysis
bool isEffectivelyFinalMethod(FullApplySite AI,
                              SILType ClassType,
                              ClassDecl *CD,
                              ClassHierarchyAnalysis *CHA) {
  if (CD && CD->isFinal())
    return true;

  const DeclContext *DC = AI.getModule().getAssociatedContext();

  // Without an associated context we cannot perform any
  // access-based optimizations.
  if (!DC)
    return false;

  auto *CMI = cast<MethodInst>(AI.getCallee());

  if (!calleesAreStaticallyKnowable(AI.getModule(), CMI->getMember()))
    return false;

  auto *Method = CMI->getMember().getAbstractFunctionDecl();
  assert(Method && "Expected abstract function decl!");
  assert(!Method->isFinal() && "Unexpected indirect call to final method!");

  // If this method is not overridden in the module,
  // there is no other implementation.
  if (!Method->isOverridden())
    return true;

  // Class declaration may be nullptr, e.g. for cases like:
  // func foo<C:Base>(c: C) {}, where C is a class, but
  // it does not have a class decl.
  if (!CD)
    return false;

  if (!CHA)
    return false;

  // This is a private or a module internal class.
  //
  // We can analyze the class hierarchy rooted at it and
  // eventually devirtualize a method call more efficiently.

  ClassHierarchyAnalysis::ClassList Subs;
  getAllSubclasses(CHA, CD, ClassType, AI.getModule(), Subs);

  // This is the implementation of the method to be used
  // if the exact class of the instance would be CD.
  auto *ImplMethod = CD->findImplementingMethod(Method);

  // First, analyze all direct subclasses.
  for (auto S : Subs) {
    // Check if the subclass overrides a method and provides
    // a different implementation.
    auto *ImplFD = S->findImplementingMethod(Method);
    if (ImplFD != ImplMethod)
      return false;
  }

  return true;
}

/// Check if a given class is final in terms of a current
/// compilation, i.e.:
/// - it is really final
/// - or it is private and has not sub-classes
/// - or it is an internal class without sub-classes and
///   it is a whole-module compilation.
static bool isKnownFinalClass(ClassDecl *CD, SILModule &M,
                              ClassHierarchyAnalysis *CHA) {
  const DeclContext *DC = M.getAssociatedContext();

  if (CD->isFinal())
    return true;

  // Without an associated context we cannot perform any
  // access-based optimizations.
  if (!DC)
    return false;

  // Only handle classes defined within the SILModule's associated context.
  if (!CD->isChildContextOf(DC))
    return false;

  if (!CD->hasAccessibility())
    return false;

  // Only consider 'private' members, unless we are in whole-module compilation.
  switch (CD->getEffectiveAccess()) {
  case Accessibility::Open:
    return false;
  case Accessibility::Public:
  case Accessibility::Internal:
    if (!M.isWholeModule())
      return false;
    break;
  case Accessibility::FilePrivate:
  case Accessibility::Private:
    break;
  }

  // Take the ClassHierarchyAnalysis into account.
  // If a given class has no subclasses and
  // - private
  // - or internal and it is a WMO compilation
  // then this class can be considered final for the purpose
  // of devirtualization.
  if (CHA) {
    if (!CHA->hasKnownDirectSubclasses(CD)) {
      switch (CD->getEffectiveAccess()) {
      case Accessibility::Open:
        return false;
      case Accessibility::Public:
      case Accessibility::Internal:
        if (!M.isWholeModule())
          return false;
        break;
      case Accessibility::FilePrivate:
      case Accessibility::Private:
        break;
      }

      return true;
    }
  }

  return false;
}


// Attempt to get the instance for S, whose static type is the same as
// its exact dynamic type, returning a null SILValue() if we cannot find it.
// The information that a static type is the same as the exact dynamic,
// can be derived e.g.:
// - from a constructor or
// - from a successful outcome of a checked_cast_br [exact] instruction.
SILValue swift::getInstanceWithExactDynamicType(SILValue S, SILModule &M,
                                                ClassHierarchyAnalysis *CHA) {

  while (S) {
    S = stripCasts(S);
    if (isa<AllocRefInst>(S) || isa<MetatypeInst>(S))
      return S;

    auto *Arg = dyn_cast<SILArgument>(S);
    if (!Arg)
      break;

    auto *SinglePred = Arg->getParent()->getSinglePredecessorBlock();
    if (!SinglePred) {
      if (!Arg->isFunctionArg())
        break;
      auto *CD = Arg->getType().getClassOrBoundGenericClass();
      // Check if this class is effectively final.
      if (!CD || !isKnownFinalClass(CD, M, CHA))
        break;
      return Arg;
    }

    // Traverse the chain of predecessors.
    if (isa<BranchInst>(SinglePred->getTerminator()) ||
        isa<CondBranchInst>(SinglePred->getTerminator())) {
      S = Arg->getIncomingValue(SinglePred);
      continue;
    }

    // If it is a BB argument received on a success branch
    // of a checked_cast_br, then we know its exact type.
    auto *CCBI = dyn_cast<CheckedCastBranchInst>(SinglePred->getTerminator());
    if (!CCBI)
      break;
    if (!CCBI->isExact() || CCBI->getSuccessBB() != Arg->getParent())
      break;
    return S;
  }

  return SILValue();
}

/// Try to determine the exact dynamic type of an object.
/// returns the exact dynamic type of the object, or an empty type if the exact
/// type could not be determined.
SILType swift::getExactDynamicType(SILValue S, SILModule &M,
                                   ClassHierarchyAnalysis *CHA,
                                   bool ForUnderlyingObject) {
  // Set of values to be checked for their exact types.
  SmallVector<SILValue, 8> WorkList;
  // The detected type of the underlying object.
  SILType ResultType;
  // Set of processed values.
  llvm::SmallSet<SILValue, 8> Processed;
  WorkList.push_back(S);

  while (!WorkList.empty()) {
    auto V = WorkList.pop_back_val();
    if (!V)
      return SILType();
    if (Processed.count(V))
      continue;
    Processed.insert(V);
    // For underlying object strip casts and projections.
    // For the object itself, simply strip casts.
    V = ForUnderlyingObject ? getUnderlyingObject(V) : stripCasts(V);

    if (isa<AllocRefInst>(V) || isa<MetatypeInst>(V)) {
      if (ResultType && ResultType != V->getType())
        return SILType();
      ResultType = V->getType();
      continue;
    }

    if (isa<LiteralInst>(V)) {
      if (ResultType && ResultType != V->getType())
        return SILType();
      ResultType = V->getType();
      continue;
    }

    if (isa<StructInst>(V) || isa<TupleInst>(V) || isa<EnumInst>(V)) {
      if (ResultType && ResultType != V->getType())
        return SILType();
      ResultType = V->getType();
      continue;
    }

    if (ForUnderlyingObject) {
      if (isa<AllocationInst>(V)) {
        if (ResultType && ResultType != V->getType())
          return SILType();
        ResultType = V->getType();
        continue;
      }
      // Look through strong_pin instructions.
      if (isa<StrongPinInst>(V)) {
        WorkList.push_back(cast<SILInstruction>(V)->getOperand(0));
        continue;
      }
    }

    auto Arg = dyn_cast<SILArgument>(V);
    if (!Arg) {
      // We don't know what it is.
      return SILType();
    }

    if (Arg->isFunctionArg()) {
      // Bail on metatypes for now.
      if (Arg->getType().getSwiftRValueType()->is<AnyMetatypeType>()) {
        return SILType();
      }
      auto *CD = Arg->getType().getClassOrBoundGenericClass();
      // If it is not class and it is a trivial type, then it
      // should be the exact type.
      if (!CD && Arg->getType().isTrivial(M)) {
        if (ResultType && ResultType != Arg->getType())
          return SILType();
        ResultType = Arg->getType();
        continue;
      }

      if (!CD) {
        // It is not a class or a trivial type, so we don't know what it is.
        return SILType();
      }

      // Check if this class is effectively final.
      if (!isKnownFinalClass(CD, M, CHA)) {
        return SILType();
      }

      if (ResultType && ResultType != Arg->getType())
        return SILType();
      ResultType = Arg->getType();
      continue;
    }

    auto *SinglePred = Arg->getParent()->getSinglePredecessorBlock();
    if (SinglePred) {
      // If it is a BB argument received on a success branch
      // of a checked_cast_br, then we know its exact type.
      auto *CCBI = dyn_cast<CheckedCastBranchInst>(SinglePred->getTerminator());
      if (CCBI && CCBI->isExact() && CCBI->getSuccessBB() == Arg->getParent()) {
        if (ResultType && ResultType != Arg->getType())
          return SILType();
        ResultType = Arg->getType();
        continue;
      }
    }

    // It is a BB argument, look through incoming values. If they all have the
    // same exact type, then we consider it to be the type of the BB argument.
    SmallVector<SILValue, 4> IncomingValues;

    if (Arg->getIncomingValues(IncomingValues)) {
      for (auto InValue : IncomingValues) {
        WorkList.push_back(InValue);
      }
      continue;
    }

    // The exact type is unknown.
    return SILType();
  }

  return ResultType;
}


/// Try to determine the exact dynamic type of the underlying object.
/// returns the exact dynamic type of a value, or an empty type if the exact
/// type could not be determined.
SILType
swift::getExactDynamicTypeOfUnderlyingObject(SILValue S, SILModule &M,
                                             ClassHierarchyAnalysis *CHA) {
  return getExactDynamicType(S, M, CHA, /* ForUnderlyingObject */ true);
}

// Start with the substitutions from the apply.
// Try to propagate them to find out the real substitutions required
// to invoke the method.
static void
getSubstitutionsForCallee(SILModule &M,
                          CanSILFunctionType baseCalleeType,
                          CanType derivedSelfType,
                          FullApplySite AI,
                          SmallVectorImpl<Substitution> &newSubs) {

  // If the base method is not polymorphic, no substitutions are required,
  // even if we originally had substitutions for calling the derived method.
  if (!baseCalleeType->isPolymorphic())
    return;

  auto derivedClass = derivedSelfType;
  if (auto metatypeType = dyn_cast<MetatypeType>(derivedClass))
    derivedClass = CanType(metatypeType->getInstanceType());

  SubstitutionMap subMap;

  if (auto origCalleeSig = AI.getOrigCalleeType()->getGenericSignature()) {
    auto calleeSelfType = AI.getSubstCalleeType()->getSelfParameter().getType();
    if (auto metatypeType = dyn_cast<MetatypeType>(calleeSelfType))
      calleeSelfType = CanType(metatypeType->getInstanceType());
    auto *calleeClassDecl = calleeSelfType->getClassOrBoundGenericClass();
    assert(calleeClassDecl && "self is not a class type");

    auto origSubs = AI.getSubstitutions();

    // Add generic parameters from the method itself, ignoring any generic
    // parameters from the derived class.
    unsigned minDepth = 0;
    if (auto derivedClassSig = calleeClassDecl->getGenericSignatureOfContext())
      minDepth = derivedClassSig->getGenericParams().back()->getDepth() + 1;

    for (auto depTy : origCalleeSig->getAllDependentTypes()) {
      // Grab the next substitution.
      auto sub = origSubs.front();
      origSubs = origSubs.slice(1);

      // If the dependent type doesn't contain any generic parameter with
      // a depth of at least the minimum, skip this type.
      auto canTy = depTy->getCanonicalType();
      auto hasInnerGenericParameter = [minDepth](Type type) -> bool {
        if (auto gp = type->getAs<GenericTypeParamType>()) {
          return gp->getDepth() >= minDepth;
        }
        return false;
      };

      if (!Type(canTy.getPointer()).findIf(hasInnerGenericParameter))
        continue;

      // Otherwise, record the replacement and conformances for the mapped
      // type.
      subMap.addSubstitution(canTy, sub.getReplacement());
      subMap.addConformances(canTy, sub.getConformances());
    }
    assert(origSubs.empty());
  }

  // Add any generic substitutions for the base class.
  auto baseSelfType = baseCalleeType->getSelfParameter().getType();
  if (auto metatypeType = dyn_cast<MetatypeType>(baseSelfType))
    baseSelfType = CanType(metatypeType->getInstanceType());

  auto *baseClassDecl = baseSelfType.getClassOrBoundGenericClass();
  assert(baseClassDecl && "not a class method");

  if (auto baseClassSig = baseClassDecl->getGenericSignatureOfContext()) {
    // Compute the type of the base class, starting from the
    // derived class type and the type of the method's self
    // parameter.
    auto baseClass = derivedClass->getSuperclassForDecl(baseClassDecl, nullptr)
        ->getCanonicalType();
    auto baseClassSubs = baseClass->gatherAllSubstitutions(
        M.getSwiftModule(), nullptr);

    // Decompose the base class substitutions, adding them to the same
    // substitution maps as above.
    baseClassSig->getSubstitutionMap(baseClassSubs, subMap);
  }

  // Build the new substitutions using the base method signature.
  auto baseCalleeSig = baseCalleeType->getGenericSignature();
  baseCalleeSig->getSubstitutions(*M.getSwiftModule(), subMap, newSubs);
}

SILFunction *swift::getTargetClassMethod(SILModule &M,
                                         SILType ClassOrMetatypeType,
                                         MethodInst *MI) {
  assert((isa<ClassMethodInst>(MI) || isa<WitnessMethodInst>(MI) ||
          isa<SuperMethodInst>(MI)) &&
         "Only class_method and witness_method instructions are supported");

  SILDeclRef Member = MI->getMember();
  if (ClassOrMetatypeType.is<MetatypeType>())
    ClassOrMetatypeType = ClassOrMetatypeType.getMetatypeInstanceType(M);

  auto *CD = ClassOrMetatypeType.getClassOrBoundGenericClass();
  return M.lookUpFunctionInVTable(CD, Member);
}

/// \brief Check if it is possible to devirtualize an Apply instruction
/// and a class member obtained using the class_method instruction into
/// a direct call to a specific member of a specific class.
///
/// \p AI is the apply to devirtualize.
/// \p ClassOrMetatypeType is the class type or metatype type we are
///    devirtualizing for.
/// return true if it is possible to devirtualize, false - otherwise.
bool swift::canDevirtualizeClassMethod(FullApplySite AI,
                                       SILType ClassOrMetatypeType) {

  DEBUG(llvm::dbgs() << "    Trying to devirtualize : " << *AI.getInstruction());

  SILModule &Mod = AI.getModule();

  // First attempt to lookup the origin for our class method. The origin should
  // either be a metatype or an alloc_ref.
  DEBUG(llvm::dbgs() << "        Origin Type: " << ClassOrMetatypeType);

  auto *MI = cast<MethodInst>(AI.getCallee());

  // Find the implementation of the member which should be invoked.
  auto *F = getTargetClassMethod(Mod, ClassOrMetatypeType, MI);

  // If we do not find any such function, we have no function to devirtualize
  // to... so bail.
  if (!F) {
    DEBUG(llvm::dbgs() << "        FAIL: Could not find matching VTable or "
                          "vtable method for this class.\n");
    return false;
  }

  if (!F->shouldOptimize()) {
    // Do not consider functions that should not be optimized.
    DEBUG(llvm::dbgs() << "        FAIL: Could not optimize function "
                       << " because it is marked no-opt: " << F->getName()
                       << "\n");
    return false;
  }

  if (AI.getFunction()->isFragile()) {
    // function_ref inside fragile function cannot reference a private or
    // hidden symbol.
    if (!F->hasValidLinkageForFragileRef())
      return false;
  }

  // Type of the actual function to be called.
  CanSILFunctionType GenCalleeType = F->getLoweredFunctionType();

  // Type of the actual function to be called with substitutions applied.
  CanSILFunctionType SubstCalleeType = GenCalleeType;

  // For polymorphic functions, bail if the number of substitutions is
  // not the same as the number of expected generic parameters.
  if (GenCalleeType->isPolymorphic()) {
    // First, find proper list of substitutions for the concrete
    // method to be called.
    SmallVector<Substitution, 4> Subs;
    getSubstitutionsForCallee(Mod, GenCalleeType,
                              ClassOrMetatypeType.getSwiftRValueType(),
                              AI, Subs);
    SubstCalleeType =
        GenCalleeType->substGenericArgs(Mod, Mod.getSwiftModule(), Subs);
  }

  // Check if the optimizer knows how to cast the return type.
  SILType ReturnType = SubstCalleeType->getSILResult();

  if (!canCastValueToABICompatibleType(Mod, ReturnType, AI.getType()))
      return false;

  return true;
}

/// \brief Devirtualize an apply of a class method.
///
/// \p AI is the apply to devirtualize.
/// \p ClassOrMetatype is a class value or metatype value that is the
///    self argument of the apply we will devirtualize.
/// return the result value of the new ApplyInst if created one or null.
DevirtualizationResult swift::devirtualizeClassMethod(FullApplySite AI,
                                                     SILValue ClassOrMetatype) {
  DEBUG(llvm::dbgs() << "    Trying to devirtualize : " << *AI.getInstruction());

  SILModule &Mod = AI.getModule();
  auto *MI = cast<MethodInst>(AI.getCallee());
  auto ClassOrMetatypeType = ClassOrMetatype->getType();
  auto *F = getTargetClassMethod(Mod, ClassOrMetatypeType, MI);

  CanSILFunctionType GenCalleeType = F->getLoweredFunctionType();

  SmallVector<Substitution, 4> Subs;
  getSubstitutionsForCallee(Mod, GenCalleeType,
                            ClassOrMetatypeType.getSwiftRValueType(),
                            AI, Subs);
  CanSILFunctionType SubstCalleeType = GenCalleeType;
  if (GenCalleeType->isPolymorphic())
    SubstCalleeType = GenCalleeType->substGenericArgs(Mod, Mod.getSwiftModule(), Subs);

  SILBuilderWithScope B(AI.getInstruction());
  FunctionRefInst *FRI = B.createFunctionRef(AI.getLoc(), F);

  // Create the argument list for the new apply, casting when needed
  // in order to handle covariant indirect return types and
  // contravariant argument types.
  llvm::SmallVector<SILValue, 8> NewArgs;

  auto IndirectResultArgs = AI.getIndirectResults();
  auto IndirectResultInfos = SubstCalleeType->getIndirectResults();
  for (unsigned i : indices(IndirectResultArgs))
    NewArgs.push_back(castValueToABICompatibleType(&B, AI.getLoc(),
                              IndirectResultArgs[i],
                              IndirectResultArgs[i]->getType(),
                              IndirectResultInfos[i].getSILType()).getValue());

  auto Args = AI.getArgumentsWithoutIndirectResults();
  auto ParamTypes = SubstCalleeType->getParameterSILTypes();
  for (unsigned i = 0, e = Args.size() - 1; i != e; ++i)
    NewArgs.push_back(castValueToABICompatibleType(&B, AI.getLoc(), Args[i],
                                                   Args[i]->getType(),
                                                   ParamTypes[i]).getValue());

  // Add the self argument, upcasting if required because we're
  // calling a base class's method.
  auto SelfParamTy = SubstCalleeType->getSelfParameter().getSILType();
  NewArgs.push_back(castValueToABICompatibleType(&B, AI.getLoc(),
                                                 ClassOrMetatype,
                                                 ClassOrMetatypeType,
                                                 SelfParamTy).getValue());

  SILType ResultTy = SubstCalleeType->getSILResult();

  SILType SubstCalleeSILType =
    SILType::getPrimitiveObjectType(SubstCalleeType);
  FullApplySite NewAI;

  SILBasicBlock *ResultBB = nullptr;
  SILBasicBlock *NormalBB = nullptr;
  SILValue ResultValue;
  bool ResultCastRequired = false;
  SmallVector<Operand *, 4> OriginalResultUses;

  if (!isa<TryApplyInst>(AI)) {
    NewAI = B.createApply(AI.getLoc(), FRI, SubstCalleeSILType, ResultTy,
                          Subs, NewArgs, cast<ApplyInst>(AI)->isNonThrowing());
    ResultValue = NewAI.getInstruction();
  } else {
    auto *TAI = cast<TryApplyInst>(AI);
    // Create new normal and error BBs only if:
    // - re-using a BB would create a critical edge
    // - or, the result of the new apply would be of different
    //   type than the argument of the original normal BB.
    if (TAI->getNormalBB()->getSinglePredecessorBlock())
      ResultBB = TAI->getNormalBB();
    else {
      ResultBB = B.getFunction().createBasicBlock();
      ResultBB->createArgument(ResultTy);
    }

    NormalBB = TAI->getNormalBB();

    SILBasicBlock *ErrorBB = nullptr;
    if (TAI->getErrorBB()->getSinglePredecessorBlock())
      ErrorBB = TAI->getErrorBB();
    else {
      ErrorBB = B.getFunction().createBasicBlock();
      ErrorBB->createArgument(TAI->getErrorBB()->getArgument(0)->getType());
    }

    NewAI = B.createTryApply(AI.getLoc(), FRI, SubstCalleeSILType,
                             Subs, NewArgs,
                             ResultBB, ErrorBB);
    if (ErrorBB != TAI->getErrorBB()) {
      B.setInsertionPoint(ErrorBB);
      B.createBranch(TAI->getLoc(), TAI->getErrorBB(),
                     {ErrorBB->getArgument(0)});
    }

    // Does the result value need to be casted?
    ResultCastRequired = ResultTy != NormalBB->getArgument(0)->getType();

    if (ResultBB != NormalBB)
      B.setInsertionPoint(ResultBB);
    else if (ResultCastRequired) {
      B.setInsertionPoint(NormalBB->begin());
      // Collect all uses, before casting.
      for (auto *Use : NormalBB->getArgument(0)->getUses()) {
        OriginalResultUses.push_back(Use);
      }
      NormalBB->getArgument(0)->replaceAllUsesWith(
          SILUndef::get(AI.getType(), Mod));
      NormalBB->replaceArgument(0, ResultTy, nullptr);
    }

    // The result value is passed as a parameter to the normal block.
    ResultValue = ResultBB->getArgument(0);
  }

  // Check if any casting is required for the return value.
  ResultValue = castValueToABICompatibleType(&B, NewAI.getLoc(), ResultValue,
                                             ResultTy, AI.getType()).getValue();

  DEBUG(llvm::dbgs() << "        SUCCESS: " << F->getName() << "\n");
  NumClassDevirt++;

  if (NormalBB) {
    if (NormalBB != ResultBB) {
      // If artificial normal BB was introduced, branch
      // to the original normal BB.
      B.createBranch(NewAI.getLoc(), NormalBB, { ResultValue });
    } else if (ResultCastRequired) {
      // Update all original uses by the new value.
      for (auto *Use: OriginalResultUses) {
        Use->set(ResultValue);
      }
    }
    return std::make_pair(NewAI.getInstruction(), NewAI);
  }

  // We need to return a pair of values here:
  // - the first one is the actual result of the devirtualized call, possibly
  //   casted into an appropriate type. This SILValue may be a BB arg, if it
  //   was a cast between optional types.
  // - the second one is the new apply site.
  return std::make_pair(ResultValue, NewAI);
}

DevirtualizationResult swift::tryDevirtualizeClassMethod(FullApplySite AI,
                                                   SILValue ClassInstance) {
  if (!canDevirtualizeClassMethod(AI, ClassInstance->getType()))
    return std::make_pair(nullptr, FullApplySite());
  return devirtualizeClassMethod(AI, ClassInstance);
}


//===----------------------------------------------------------------------===//
//                        Witness Method Optimization
//===----------------------------------------------------------------------===//

static ArrayRef<Substitution>
getSubstitutionsForProtocolConformance(ProtocolConformanceRef CRef) {
  auto C = CRef.getConcrete();

  // Walk down to the base NormalProtocolConformance.
  ArrayRef<Substitution> Subs;
  const ProtocolConformance *ParentC = C;
  while (!isa<NormalProtocolConformance>(ParentC)) {
    switch (ParentC->getKind()) {
    case ProtocolConformanceKind::Normal:
      llvm_unreachable("should have exited the loop?!");
    case ProtocolConformanceKind::Inherited:
      ParentC = cast<InheritedProtocolConformance>(ParentC)
        ->getInheritedConformance();
      break;
    case ProtocolConformanceKind::Specialized: {
      auto SC = cast<SpecializedProtocolConformance>(ParentC);
      ParentC = SC->getGenericConformance();
      assert(Subs.empty() && "multiple conformance specializations?!");
      Subs = SC->getGenericSubstitutions();
      break;
    }
    }
  }
  const NormalProtocolConformance *NormalC
    = cast<NormalProtocolConformance>(ParentC);

  // If the normal conformance is for a generic type, and we didn't hit a
  // specialized conformance, collect the substitutions from the generic type.
  // FIXME: The AST should do this for us.
  if (NormalC->getType()->isSpecialized() && Subs.empty()) {
    Subs = NormalC->getType()
      ->gatherAllSubstitutions(NormalC->getDeclContext()->getParentModule(),
                               nullptr);
  }
  
  return Subs;
}

/// Compute substitutions for making a direct call to a SIL function with
/// @convention(witness_method) convention.
///
/// Such functions have a substituted generic signature where the
/// abstract `Self` parameter from the original type of the protocol
/// requirement is replaced by a concrete type.
///
/// Thus, the original substitutions of the apply instruction that
/// are written in terms of the requirement's generic signature need
/// to be remapped to substitutions suitable for the witness signature.
///
/// \param conformanceRef The (possibly-specialized) conformance
/// \param requirementSig The generic signature of the requirement
/// \param witnessThunkSig The generic signature of the witness method
/// \param origSubs The substitutions from the call instruction
/// \param newSubs New substitutions are stored here
static void getWitnessMethodSubstitutions(
    SILModule &M,
    ProtocolConformanceRef conformanceRef,
    GenericSignature *requirementSig,
    GenericSignature *witnessThunkSig,
    ArrayRef<Substitution> origSubs,
    bool isDefaultWitness,
    SmallVectorImpl<Substitution> &newSubs) {

  if (witnessThunkSig == nullptr)
    return;

  assert(!conformanceRef.isAbstract());

  auto conformance = conformanceRef.getConcrete();

  // Otherwise, we need to build new caller-side substitutions
  // written in terms of the witness thunk's generic signature,
  // mapping to the archetypes of the caller.
  SubstitutionMap subMap;

  // Take apart substitutions from the conforming type.
  ArrayRef<Substitution> witnessSubs;
  auto *rootConformance = conformance->getRootNormalConformance();
  auto *witnessSig = rootConformance->getGenericSignature();
  unsigned depth = 0;
  if (isDefaultWitness) {
    // For default witnesses, we substitute all of Self.
    auto gp = witnessThunkSig->getGenericParams().front()->getCanonicalType();
    subMap.addSubstitution(gp, origSubs.front().getReplacement());
    subMap.addConformances(gp, origSubs.front().getConformances());

    // For default witnesses, innermost generic parameters are always at
    // depth 1.
    depth = 1;
  } else {
    // If `Self` maps to a bound generic type, this gives us the
    // substitutions for the concrete type's generic parameters.
    witnessSubs = getSubstitutionsForProtocolConformance(conformanceRef);

    if (!witnessSubs.empty()) {
      witnessSig->getSubstitutionMap(witnessSubs, subMap);
      depth = witnessSig->getGenericParams().back()->getDepth() + 1;
    }
  }

  // Next, take apart caller-side substitutions.
  //
  // Note that the Self-derived dependent types appearing on the left
  // hand side of the map are dropped.
  // FIXME: This won't be correct if the requirement itself adds 'Self'
  // requirements. We should be working from the substitutions in the witness.
  //
  // Also note that we rebuild the generic parameters in the requirement
  // to provide them with the required depth for the thunk itself.
  if (requirementSig->getGenericParams().back()->getDepth() > 0) {
    // Local function to replace generic parameters within the requirement
    // signature with the generic parameter we want to use in the substitution
    // map:
    //   - If the generic parameter is 'Self', return a null type so we don't
    //     add any substitution.
    //   - Otherwise, reset the generic parameter's depth one level deeper than
    //     the deepest generic parameter in the conformance.
    //
    // This local function is meant to be used with Type::transform();
    auto replaceGenericParameter = [&](Type type) -> Type {
      if (auto gp = type->getAs<GenericTypeParamType>()) {
        if (gp->getDepth() == 0) return Type();
        return GenericTypeParamType::get(depth, gp->getIndex(),
                                         M.getASTContext());
      }

      return type;
    };

    // Walk through the substitutions and dependent types.
    ArrayRef<Substitution> subs = origSubs;
    for (auto origDepTy : requirementSig->getAllDependentTypes()) {
      // Grab the next substitution.
      auto sub = subs.front();
      subs = subs.slice(1);

      // Map the generic parameters in the dependent type into the witness
      // thunk's depth.
      auto mappedDepTy = origDepTy.transform(replaceGenericParameter);

      // If the dependent type was rooted in 'Self', it will come out null;
      // skip it.
      if (!mappedDepTy) continue;

      // Otherwise, record the replacement and conformances for the mapped
      // type.
      auto canTy = mappedDepTy->getCanonicalType();
      subMap.addSubstitution(canTy, sub.getReplacement());
      subMap.addConformances(canTy, sub.getConformances());
    }
    assert(subs.empty() && "Did not consume all substitutions");
  }

  // Now, apply both sets of substitutions computed above to the
  // forwarding substitutions of the witness thunk.
  witnessThunkSig->getSubstitutions(*M.getSwiftModule(),
                                    subMap, newSubs);
}

static void getWitnessMethodSubstitutions(ApplySite AI, SILFunction *F,
                                          ProtocolConformanceRef CRef,
                                          SmallVectorImpl<Substitution> &NewSubs) {
  auto &Module = AI.getModule();

  auto requirementSig = AI.getOrigCalleeType()->getGenericSignature();
  auto witnessThunkSig = F->getLoweredFunctionType()->getGenericSignature();

  ArrayRef<Substitution> origSubs = AI.getSubstitutions();

  bool isDefaultWitness =
    F->getLoweredFunctionType()->getRepresentation()
      == SILFunctionTypeRepresentation::WitnessMethod &&
    F->getLoweredFunctionType()->getDefaultWitnessMethodProtocol(
                                                     *Module.getSwiftModule())
      == CRef.getRequirement();

  getWitnessMethodSubstitutions(Module, CRef, requirementSig, witnessThunkSig,
                                origSubs, isDefaultWitness, NewSubs);
}

/// Check if an upcast is legal.
/// The logic in this function is heavily based on the checks in
/// the SILVerifier.
bool swift::isLegalUpcast(SILType FromTy, SILType ToTy) {
  if (ToTy.is<MetatypeType>()) {
    CanType InstTy(ToTy.castTo<MetatypeType>()->getInstanceType());
    if (!FromTy.is<MetatypeType>())
      return false;
    CanType OpInstTy(FromTy.castTo<MetatypeType>()->getInstanceType());
    auto InstClass = InstTy->getClassOrBoundGenericClass();
    if (!InstClass)
      return false;

    bool CanBeUpcasted =
        InstClass->usesObjCGenericsModel()
            ? InstClass->getDeclaredTypeInContext()->isBindableToSuperclassOf(
                  OpInstTy, nullptr)
            : InstTy->isExactSuperclassOf(OpInstTy, nullptr);

    return CanBeUpcasted;
  }

  // Upcast from Optional<B> to Optional<A> is legal as long as B is a
  // subclass of A.
  if (ToTy.getSwiftRValueType().getAnyOptionalObjectType() &&
      FromTy.getSwiftRValueType().getAnyOptionalObjectType()) {
    ToTy = SILType::getPrimitiveObjectType(
        ToTy.getSwiftRValueType().getAnyOptionalObjectType());
    FromTy = SILType::getPrimitiveObjectType(
        FromTy.getSwiftRValueType().getAnyOptionalObjectType());
  }

  auto ToClass = ToTy.getClassOrBoundGenericClass();
  if (!ToClass)
    return false;
  bool CanBeUpcasted =
      ToClass->usesObjCGenericsModel()
          ? ToClass->getDeclaredTypeInContext()->isBindableToSuperclassOf(
                FromTy.getSwiftRValueType(), nullptr)
          : ToTy.isExactSuperclassOf(FromTy);

  return CanBeUpcasted;
}

/// Check if we can pass/convert all arguments of the original apply
/// as required by the found devirtualized method.
static bool
canPassOrConvertAllArguments(ApplySite AI,
                             CanSILFunctionType SubstCalleeCanType) {
  for (unsigned ArgN = 0, ArgE = AI.getNumArguments(); ArgN != ArgE; ++ArgN) {
    SILValue A = AI.getArgument(ArgN);
    auto ParamType = SubstCalleeCanType->getSILArgumentType(
      SubstCalleeCanType->getNumSILArguments() - AI.getNumArguments() + ArgN);
    // Check if we can cast the provided argument into the required
    // parameter type.
    auto FromTy = A->getType();
    auto ToTy = ParamType;
    // If types are the same, no conversion will be required.
    if (FromTy == ToTy)
      continue;
    // Otherwise, it should be possible to upcast the arguments.
    if (!isLegalUpcast(FromTy, ToTy))
      return false;
  }
  return true;
}

/// Generate a new apply of a function_ref to replace an apply of a
/// witness_method when we've determined the actual function we'll end
/// up calling.
static ApplySite devirtualizeWitnessMethod(ApplySite AI, SILFunction *F,
                                           ProtocolConformanceRef C) {
  // We know the witness thunk and the corresponding set of substitutions
  // required to invoke the protocol method at this point.
  auto &Module = AI.getModule();

  // Collect all the required substitutions.
  //
  // The complete set of substitutions may be different, e.g. because the found
  // witness thunk F may have been created by a specialization pass and have
  // additional generic parameters.
  SmallVector<Substitution, 4> NewSubs;

  getWitnessMethodSubstitutions(AI, F, C, NewSubs);

  // Figure out the exact bound type of the function to be called by
  // applying all substitutions.
  auto CalleeCanType = F->getLoweredFunctionType();
  auto SubstCalleeCanType = CalleeCanType->substGenericArgs(
    Module, Module.getSwiftModule(), NewSubs);

  // Bail if some of the arguments cannot be converted into
  // types required by the found devirtualized method.
  if (!canPassOrConvertAllArguments(AI, SubstCalleeCanType))
    return ApplySite();

  // Collect arguments from the apply instruction.
  auto Arguments = SmallVector<SILValue, 4>();

  // Iterate over the non self arguments and add them to the
  // new argument list, upcasting when required.
  SILBuilderWithScope B(AI.getInstruction());
  for (unsigned ArgN = 0, ArgE = AI.getNumArguments(); ArgN != ArgE; ++ArgN) {
    SILValue A = AI.getArgument(ArgN);
    auto ParamType = SubstCalleeCanType->getSILArgumentType(
      SubstCalleeCanType->getNumSILArguments() - AI.getNumArguments() + ArgN);
    if (A->getType() != ParamType)
      A = B.createUpcast(AI.getLoc(), A, ParamType);

    Arguments.push_back(A);
  }

  // Replace old apply instruction by a new apply instruction that invokes
  // the witness thunk.
  SILBuilderWithScope Builder(AI.getInstruction());
  SILLocation Loc = AI.getLoc();
  FunctionRefInst *FRI = Builder.createFunctionRef(Loc, F);

  auto SubstCalleeSILType = SILType::getPrimitiveObjectType(SubstCalleeCanType);
  auto ResultSILType = SubstCalleeCanType->getSILResult();
  ApplySite SAI;

  if (auto *A = dyn_cast<ApplyInst>(AI))
    SAI = Builder.createApply(Loc, FRI, SubstCalleeSILType,
                              ResultSILType, NewSubs, Arguments,
                              A->isNonThrowing());
  if (auto *TAI = dyn_cast<TryApplyInst>(AI))
    SAI = Builder.createTryApply(Loc, FRI, SubstCalleeSILType,
                                 NewSubs, Arguments,
                                 TAI->getNormalBB(), TAI->getErrorBB());
  if (auto *PAI = dyn_cast<PartialApplyInst>(AI))
    SAI = Builder.createPartialApply(Loc, FRI, SubstCalleeSILType,
                                     NewSubs, Arguments, PAI->getType());

  NumWitnessDevirt++;
  return SAI;
}

/// In the cases where we can statically determine the function that
/// we'll call to, replace an apply of a witness_method with an apply
/// of a function_ref, returning the new apply.
DevirtualizationResult swift::tryDevirtualizeWitnessMethod(ApplySite AI) {
  SILFunction *F;
  SILWitnessTable *WT;

  auto *WMI = cast<WitnessMethodInst>(AI.getCallee());

  std::tie(F, WT) =
    AI.getModule().lookUpFunctionInWitnessTable(WMI->getConformance(),
                                                WMI->getMember());

  if (!F)
    return std::make_pair(nullptr, FullApplySite());

  if (AI.getFunction()->isFragile()) {
    // function_ref inside fragile function cannot reference a private or
    // hidden symbol.
    if (!F->hasValidLinkageForFragileRef())
      return std::make_pair(nullptr, FullApplySite());
  }

  auto Result = devirtualizeWitnessMethod(AI, F, WMI->getConformance());
  return std::make_pair(Result.getInstruction(), Result);
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

/// Attempt to devirtualize the given apply if possible, and return a
/// new instruction in that case, or nullptr otherwise.
DevirtualizationResult
swift::tryDevirtualizeApply(FullApplySite AI, ClassHierarchyAnalysis *CHA) {
  DEBUG(llvm::dbgs() << "    Trying to devirtualize: " << *AI.getInstruction());

  // Devirtualize apply instructions that call witness_method instructions:
  //
  //   %8 = witness_method $Optional<UInt16>, #LogicValue.boolValue!getter.1
  //   %9 = apply %8<Self = CodeUnit?>(%6#1) : ...
  //
  if (isa<WitnessMethodInst>(AI.getCallee()))
    return tryDevirtualizeWitnessMethod(AI);

  /// Optimize a class_method and alloc_ref pair into a direct function
  /// reference:
  ///
  /// \code
  /// %XX = alloc_ref $Foo
  /// %YY = class_method %XX : $Foo, #Foo.get!1 : $@convention(method)...
  /// \endcode
  ///
  ///  or
  ///
  /// %XX = metatype $...
  /// %YY = class_method %XX : ...
  ///
  ///  into
  ///
  /// %YY = function_ref @...
  if (auto *CMI = dyn_cast<ClassMethodInst>(AI.getCallee())) {
    auto &M = AI.getModule();
    auto Instance = stripUpCasts(CMI->getOperand());
    auto ClassType = Instance->getType();
    if (ClassType.is<MetatypeType>())
      ClassType = ClassType.getMetatypeInstanceType(M);

    auto *CD = ClassType.getClassOrBoundGenericClass();

    if (isEffectivelyFinalMethod(AI, ClassType, CD, CHA))
      return tryDevirtualizeClassMethod(AI, Instance);

    // Try to check if the exact dynamic type of the instance is statically
    // known.
    if (auto Instance = getInstanceWithExactDynamicType(CMI->getOperand(),
                                                        CMI->getModule(),
                                                        CHA))
      return tryDevirtualizeClassMethod(AI, Instance);

    if (auto ExactTy = getExactDynamicType(CMI->getOperand(), CMI->getModule(),
                                           CHA)) {
      if (ExactTy == CMI->getOperand()->getType())
        return tryDevirtualizeClassMethod(AI, CMI->getOperand());
    }
  }

  if (isa<SuperMethodInst>(AI.getCallee())) {
    if (AI.hasSelfArgument()) {
      return tryDevirtualizeClassMethod(AI, AI.getSelfArgument());
    }

    // It is an invocation of a class method.
    // Last operand is the metatype that should be used for dispatching.
    return tryDevirtualizeClassMethod(AI, AI.getArguments().back());
  }

  return std::make_pair(nullptr, FullApplySite());
}
