//===--- GenericSignature.cpp - Generic Signature AST ---------------------===//
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
//
// This file implements the GenericSignature class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericSignature.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"

using namespace swift;

GenericSignature::GenericSignature(ArrayRef<GenericTypeParamType *> params,
                                   ArrayRef<Requirement> requirements,
                                   bool isKnownCanonical)
  : NumGenericParams(params.size()), NumRequirements(requirements.size()),
    CanonicalSignatureOrASTContext()
{
  auto paramsBuffer = getGenericParamsBuffer();
  for (unsigned i = 0; i < NumGenericParams; ++i) {
    paramsBuffer[i] = params[i];
  }

  auto reqtsBuffer = getRequirementsBuffer();
  for (unsigned i = 0; i < NumRequirements; ++i) {
    reqtsBuffer[i] = requirements[i];
  }

  if (isKnownCanonical)
    CanonicalSignatureOrASTContext = &getASTContext(params, requirements);
}

ArrayRef<GenericTypeParamType *> 
GenericSignature::getInnermostGenericParams() const {
  auto params = getGenericParams();

  // Find the point at which the depth changes.
  unsigned depth = params.back()->getDepth();
  for (unsigned n = params.size(); n > 0; --n) {
    if (params[n-1]->getDepth() != depth) {
      return params.slice(n);
    }
  }

  // All parameters are at the same depth.
  return params;
}

ASTContext &GenericSignature::getASTContext(
                                ArrayRef<swift::GenericTypeParamType *> params,
                                ArrayRef<swift::Requirement> requirements) {
  // The params and requirements cannot both be empty.
  if (!params.empty())
    return params.front()->getASTContext();
  else
    return requirements.front().getFirstType()->getASTContext();
}

ArchetypeBuilder *GenericSignature::getArchetypeBuilder(ModuleDecl &mod) {
  // The archetype builder is associated with the canonical signature.
  if (!isCanonical())
    return getCanonicalSignature()->getArchetypeBuilder(mod);

  // Archetype builders are stored on the ASTContext.
  return getASTContext().getOrCreateArchetypeBuilder(CanGenericSignature(this),
                                                     &mod).first;
}

bool GenericSignature::isCanonical() const {
  if (CanonicalSignatureOrASTContext.is<ASTContext*>()) return true;

  return getCanonicalSignature() == this;
}

CanGenericSignature GenericSignature::getCanonical(
                                        ArrayRef<GenericTypeParamType *> params,
                                        ArrayRef<Requirement> requirements) {
  // Canonicalize the parameters and requirements.
  SmallVector<GenericTypeParamType*, 8> canonicalParams;
  canonicalParams.reserve(params.size());
  for (auto param : params) {
    canonicalParams.push_back(cast<GenericTypeParamType>(param->getCanonicalType()));
  }

  SmallVector<Requirement, 8> canonicalRequirements;
  canonicalRequirements.reserve(requirements.size());
  for (auto &reqt : requirements) {
    canonicalRequirements.push_back(Requirement(reqt.getKind(),
                              reqt.getFirstType()->getCanonicalType(),
                              reqt.getSecondType().getCanonicalTypeOrNull()));
  }
  auto canSig = get(canonicalParams, canonicalRequirements,
                    /*isKnownCanonical=*/true);
  return CanGenericSignature(canSig);
}

CanGenericSignature
GenericSignature::getCanonicalSignature() const {
  // If we haven't computed the canonical signature yet, do so now.
  if (CanonicalSignatureOrASTContext.isNull()) {
    // Compute the canonical signature.
    CanGenericSignature canSig = getCanonical(getGenericParams(),
                                              getRequirements());

    // Record either the canonical signature or an indication that
    // this is the canonical signature.
    if (canSig != this)
      CanonicalSignatureOrASTContext = canSig;
    else
      CanonicalSignatureOrASTContext = &getGenericParams()[0]->getASTContext();

    // Return the canonical signature.
    return canSig;
  }

  // A stored ASTContext indicates that this is the canonical
  // signature.
  if (CanonicalSignatureOrASTContext.is<ASTContext*>())
    // TODO: CanGenericSignature should be const-correct.
    return CanGenericSignature(const_cast<GenericSignature*>(this));
  
  // Otherwise, return the stored canonical signature.
  return CanGenericSignature(
           CanonicalSignatureOrASTContext.get<GenericSignature*>());
}

ASTContext &GenericSignature::getASTContext() const {
  // Canonical signatures store the ASTContext directly.
  if (auto ctx = CanonicalSignatureOrASTContext.dyn_cast<ASTContext *>())
    return *ctx;

  // For everything else, just get it from the generic parameter.
  return getASTContext(getGenericParams(), getRequirements());
}

SubstitutionMap
GenericSignature::getSubstitutionMap(ArrayRef<Substitution> subs) const {
  SubstitutionMap result;
  getSubstitutionMap(subs, result);
  return result;
}

bool GenericSignature::enumeratePairedRequirements(
               llvm::function_ref<bool(Type, ArrayRef<Requirement>)> fn) const {
  // We'll be walking through the list of requirements.
  ArrayRef<Requirement> reqs = getRequirements();
  unsigned curReqIdx = 0, numReqs = reqs.size();

  // ... and walking through the list of generic parameters.
  ArrayRef<GenericTypeParamType *> genericParams = getGenericParams();
  unsigned curGenericParamIdx = 0, numGenericParams = genericParams.size();

  /// Local function to 'catch up' to the next dependent type we're going to
  /// visit, calling the function for each of the generic parameters in the
  /// generic parameter list prior to this parameter.
  auto enumerateGenericParamsUpToDependentType = [&](CanType depTy) -> bool {
    // Figure out where we should stop when enumerating generic parameters.
    unsigned stopDepth, stopIndex;
    if (auto gp = dyn_cast_or_null<GenericTypeParamType>(depTy)) {
      stopDepth = gp->getDepth();
      stopIndex = gp->getIndex();
    } else {
      stopDepth = genericParams.back()->getDepth() + 1;
      stopIndex = 0;
    }

    // Enumerate generic parameters up to the stopping point, calling the
    // callback function for each one
    while (curGenericParamIdx != numGenericParams) {
      auto curGenericParam = genericParams[curGenericParamIdx];

      // If the current generic parameter is before our stopping point, call
      // the function.
      if (curGenericParam->getDepth() < stopDepth ||
          (curGenericParam->getDepth() == stopDepth &&
           curGenericParam->getIndex() < stopIndex)) {
        if (fn(curGenericParam, { })) return true;
        ++curGenericParamIdx;
        continue;
      }

      // If the current generic parameter is at our stopping point, we're
      // done.
      if (curGenericParam->getDepth() == stopDepth &&
          curGenericParam->getIndex() == stopIndex) {
        ++curGenericParamIdx;
        return false;
      }

      // Otherwise, there's nothing to do.
      break;
    }

    return false;
  };

  // Walk over all of the requirements.
  while (curReqIdx != numReqs) {
    // "Catch up" by enumerating generic parameters up to this dependent type.
    CanType depTy = reqs[curReqIdx].getFirstType()->getCanonicalType();
    if (enumerateGenericParamsUpToDependentType(depTy)) return true;

    // Utility to skip over non-conformance constraints that apply to this
    // type.
    bool sawSameTypeConstraint = false;
    auto skipNonConformanceConstraints = [&] {
      while (curReqIdx != numReqs &&
             reqs[curReqIdx].getKind() != RequirementKind::Conformance &&
             reqs[curReqIdx].getFirstType()->getCanonicalType() == depTy) {
        // Record whether we saw a same-type constraint mentioning this type.
        if (reqs[curReqIdx].getKind() == RequirementKind::SameType)
          sawSameTypeConstraint = true;

        ++curReqIdx;
      }
    };

    // First, skip past any non-conformance constraints on this type.
    skipNonConformanceConstraints();

    // Collect all of the conformance constraints for this dependent type.
    unsigned startIdx = curReqIdx;
    unsigned endIdx = curReqIdx;
    while (curReqIdx != numReqs &&
           reqs[curReqIdx].getKind() == RequirementKind::Conformance &&
           reqs[curReqIdx].getFirstType()->getCanonicalType() == depTy) {
      ++curReqIdx;
      endIdx = curReqIdx;
    }

    // Skip any trailing non-conformance constraints.
    skipNonConformanceConstraints();

    // If there were any conformance constraints, or we have a generic
    // parameter we can't skip, invoke the callback.
    if ((startIdx != endIdx ||
         (isa<GenericTypeParamType>(depTy) && !sawSameTypeConstraint)) &&
        fn(depTy, reqs.slice(startIdx, endIdx-startIdx)))
      return true;
  }

  // Catch up on any remaining generic parameters.
  return enumerateGenericParamsUpToDependentType(CanType());
}

void
GenericSignature::getSubstitutionMap(ArrayRef<Substitution> subs,
                                     SubstitutionMap &result) const {
  // An empty parameter list gives an empty map.
  if (subs.empty())
    assert(getGenericParams().empty());

  for (auto depTy : getAllDependentTypes()) {
    auto sub = subs.front();
    subs = subs.slice(1);

    auto canTy = depTy->getCanonicalType();
    if (isa<GenericTypeParamType>(canTy))
      result.addSubstitution(canTy, sub.getReplacement());
    result.addConformances(canTy, sub.getConformances());
  }

  // TODO: same-type constraints

  assert(subs.empty() && "did not use all substitutions?!");
}

SmallVector<Type, 4> GenericSignature::getAllDependentTypes() const {
  SmallVector<Type, 4> result;
  enumeratePairedRequirements([&](Type type, ArrayRef<Requirement>) {
    result.push_back(type);
    return false;
  });

  return result;
}

void GenericSignature::
getSubstitutions(ModuleDecl &mod,
                 const TypeSubstitutionMap &subs,
                 GenericSignature::LookupConformanceFn lookupConformance,
                 SmallVectorImpl<Substitution> &result) const {
  // Enumerate all of the requirements that require substitution.
  enumeratePairedRequirements([&](Type depTy, ArrayRef<Requirement> reqs) {
    auto &ctx = getASTContext();

    // Compute the replacement type.
    Type currentReplacement = depTy.subst(&mod, subs);
    if (!currentReplacement)
      currentReplacement = ErrorType::get(depTy);

    // Collect the conformances.
    SmallVector<ProtocolConformanceRef, 4> currentConformances;
    for (auto req: reqs) {
      assert(req.getKind() == RequirementKind::Conformance);
      auto protoType = req.getSecondType()->castTo<ProtocolType>();
      currentConformances.push_back(
        lookupConformance(depTy->getCanonicalType(), currentReplacement,
                          protoType));
    }

    // Add it to the final substitution list.
    result.push_back({
      currentReplacement,
      ctx.AllocateCopy(currentConformances)
    });

    return false;
  });
}

void GenericSignature::
getSubstitutions(ModuleDecl &mod,
                 const SubstitutionMap &subMap,
                 SmallVectorImpl<Substitution> &result) const {
  auto lookupConformanceFn =
      [&](CanType original, Type replacement, ProtocolType *protoType)
          -> ProtocolConformanceRef {
    return *subMap.lookupConformance(original, protoType->getDecl());
  };

  getSubstitutions(mod, subMap.getMap(), lookupConformanceFn, result);
}

bool GenericSignature::requiresClass(Type type, ModuleDecl &mod) {
  if (!type->isTypeParameter()) return false;

  auto &builder = *getArchetypeBuilder(mod);
  auto pa = builder.resolveArchetype(type);
  if (!pa) return false;

  pa = pa->getRepresentative();

  // If this type was mapped to a concrete type, then there is no
  // requirement.
  if (pa->isConcreteType()) return false;

  // If there is a superclass bound, then obviously it must be a class.
  if (pa->getSuperclass()) return true;

  // If any of the protocols are class-bound, then it must be a class.
  for (auto proto : pa->getConformsTo()) {
    if (proto.first->requiresClass()) return true;
  }

  return false;
}

/// Determine the superclass bound on the given dependent type.
Type GenericSignature::getSuperclassBound(Type type, ModuleDecl &mod) {
  if (!type->isTypeParameter()) return nullptr;

  auto &builder = *getArchetypeBuilder(mod);
  auto pa = builder.resolveArchetype(type);
  if (!pa) return nullptr;

  pa = pa->getRepresentative();

  // If this type was mapped to a concrete type, then there is no
  // requirement.
  if (pa->isConcreteType()) return nullptr;

  // Retrieve the superclass bound.
  return pa->getSuperclass();
}

/// Determine the set of protocols to which the given dependent type
/// must conform.
SmallVector<ProtocolDecl *, 2> GenericSignature::getConformsTo(Type type,
                                                               ModuleDecl &mod) {
  if (!type->isTypeParameter()) return { };

  auto &builder = *getArchetypeBuilder(mod);
  auto pa = builder.resolveArchetype(type);
  if (!pa) return { };

  pa = pa->getRepresentative();

  // If this type was mapped to a concrete type, then there are no
  // requirements.
  if (pa->isConcreteType()) return { };

  // Retrieve the protocols to which this type conforms.
  SmallVector<ProtocolDecl *, 2> result;
  for (auto proto : pa->getConformsTo())
    result.push_back(proto.first);

  // Canonicalize the resulting set of protocols.
  ProtocolType::canonicalizeProtocols(result);

  return result;
}

/// Determine whether the given dependent type is equal to a concrete type.
bool GenericSignature::isConcreteType(Type type, ModuleDecl &mod) {
  return bool(getConcreteType(type, mod));
}

/// Return the concrete type that the given dependent type is constrained to,
/// or the null Type if it is not the subject of a concrete same-type
/// constraint.
Type GenericSignature::getConcreteType(Type type, ModuleDecl &mod) {
  if (!type->isTypeParameter()) return Type();

  auto &builder = *getArchetypeBuilder(mod);
  auto pa = builder.resolveArchetype(type);
  if (!pa) return Type();

  pa = pa->getRepresentative();
  if (!pa->isConcreteType()) return Type();

  return pa->getConcreteType();
}

Type GenericSignature::getRepresentative(Type type, ModuleDecl &mod) {
  assert(type->isTypeParameter());
  auto &builder = *getArchetypeBuilder(mod);
  auto pa = builder.resolveArchetype(type);
  assert(pa && "not a valid dependent type of this signature?");
  auto rep = pa->getRepresentative();
  if (rep->isConcreteType()) return rep->getConcreteType();
  if (pa == rep) {
    assert(rep->getDependentType(getGenericParams(), /*allowUnresolved*/ false)
              ->getCanonicalType() == type->getCanonicalType());
    return type;
  }
  return rep->getDependentType(getGenericParams(), /*allowUnresolved*/ false);
}

bool GenericSignature::areSameTypeParameterInContext(Type type1, Type type2,
                                                     ModuleDecl &mod) {
  assert(type1->isTypeParameter());
  assert(type2->isTypeParameter());

  if (type1.getPointer() == type2.getPointer())
    return true;

  auto &builder = *getArchetypeBuilder(mod);
  auto pa1 = builder.resolveArchetype(type1);
  assert(pa1 && "not a valid dependent type of this signature?");
  pa1 = pa1->getRepresentative();
  assert(!pa1->isConcreteType());

  auto pa2 = builder.resolveArchetype(type2);
  assert(pa2 && "not a valid dependent type of this signature?");
  pa2 = pa2->getRepresentative();
  assert(!pa2->isConcreteType());

  return pa1 == pa2;
}

bool GenericSignature::isCanonicalTypeInContext(Type type, ModuleDecl &mod) {
  // If the type isn't independently canonical, it's certainly not canonical
  // in this context.
  if (!type->isCanonical())
    return false;

  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return true;

  auto &builder = *getArchetypeBuilder(mod);

  // Look for non-canonical type parameters.
  return !type.findIf([&](Type component) -> bool {
    if (!component->isTypeParameter()) return false;

    auto pa = builder.resolveArchetype(component);
    if (!pa) return false;

    auto rep = pa->getArchetypeAnchor();
    return (rep->isConcreteType() || pa != rep);
  });
}

CanType GenericSignature::getCanonicalTypeInContext(Type type, ModuleDecl &mod) {
  type = type->getCanonicalType();

  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return CanType(type);

  auto &builder = *getArchetypeBuilder(mod);

  // Replace non-canonical type parameters.
  type = type.transform([&](Type component) -> Type {
    if (!component->isTypeParameter()) return component;

    // Resolve the potential archetype.  This can be null in nested generic
    // types, which we can't immediately canonicalize.
    auto pa = builder.resolveArchetype(component);
    if (!pa) return component;

    auto rep = pa->getArchetypeAnchor();
    if (rep->isConcreteType()) {
      return getCanonicalTypeInContext(rep->getConcreteType(), mod);
    } else {
      return rep->getDependentType(getGenericParams(),
                                   /*allowUnresolved*/ false);
    }
  });

  auto result = type->getCanonicalType();
  assert(isCanonicalTypeInContext(result, mod));
  return result;
}

GenericEnvironment *CanGenericSignature::getGenericEnvironment(
                                                     ModuleDecl &module) const {
  // Archetype builders are stored on the ASTContext.
  return module.getASTContext().getOrCreateArchetypeBuilder(*this, &module)
           .second;
}

unsigned GenericParamKey::findIndexIn(
                  llvm::ArrayRef<GenericTypeParamType *> genericParams) const {
  // For depth 0, we have random access. We perform the extra checking so that
  // we can return
  if (Depth == 0 && Index < genericParams.size() &&
      genericParams[Index] == *this)
    return Index;

  // At other depths, perform a binary search.
  unsigned result =
      std::lower_bound(genericParams.begin(), genericParams.end(), *this,
                       Ordering())
        - genericParams.begin();
  if (result < genericParams.size() && genericParams[result] == *this)
    return result;

  // We didn't find the parameter we were looking for.
  return genericParams.size();
}
