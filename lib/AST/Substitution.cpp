//===--- Substitution.cpp - Type substitutions ----------------------------===//
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
//  This file implements the Substitution class and operations on it.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Substitution.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/DenseMap.h"

using namespace swift;

bool Substitution::operator==(const Substitution &other) const {
  // The archetypes may be missing, but we can compare them directly
  // because archetypes are always canonical.
  return
    Replacement->getCanonicalType() == other.Replacement->getCanonicalType() &&
    Conformance.equals(other.Conformance);
}

Substitution::Substitution(Type Replacement,
                           ArrayRef<ProtocolConformanceRef> Conformance)
  : Replacement(Replacement), Conformance(Conformance)
{
  // The replacement type must be materializable.
  assert(Replacement->isMaterializable()
         && "cannot substitute with a non-materializable type");
}

Substitution Substitution::subst(Module *module,
                                 const SubstitutionMap &subMap) const {
  // Substitute the replacement.
  Type substReplacement = Replacement.subst(subMap, None);
  assert(substReplacement && "substitution replacement failed");

  if (substReplacement->isEqual(Replacement))
    return *this;

  if (Conformance.empty()) {
    return {substReplacement, Conformance};
  }

  bool conformancesChanged = false;
  SmallVector<ProtocolConformanceRef, 4> substConformances;
  substConformances.reserve(Conformance.size());

  for (auto c : Conformance) {
    // If we have a concrete conformance, we need to substitute the
    // conformance to apply to the new type.
    if (c.isConcrete()) {
      auto substC = c.getConcrete()->subst(module, substReplacement, subMap);
      substConformances.push_back(ProtocolConformanceRef(substC));
      if (c != substConformances.back())
        conformancesChanged = true;
      continue;
    }

    // Otherwise, we may need to fill in the conformance.
    ProtocolDecl *proto = c.getAbstract();
    Optional<ProtocolConformanceRef> conformance;

    // If the original type was an archetype, check the conformance map.
    if (auto replacementArch = Replacement->getAs<ArchetypeType>()) {
      conformance = subMap.lookupConformance(CanType(replacementArch), proto);
    }

    // If that didn't find anything, we can still synthesize AnyObject
    // conformances from thin air.  FIXME: gross.
    if (!conformance &&
        proto->isSpecificProtocol(KnownProtocolKind::AnyObject)) {
      auto classDecl
        = substReplacement->getClassOrBoundGenericClass();
      assert(classDecl);
      SmallVector<ProtocolConformance *, 1> lookupResults;
      classDecl->lookupConformance(classDecl->getParentModule(),
                                   proto, lookupResults);
      conformance = ProtocolConformanceRef(lookupResults.front());
    }

    if (conformance) {
      if (conformance->isConcrete())
        conformancesChanged = true;
      substConformances.push_back(*conformance);
    } else {
      assert(substReplacement->hasDependentProtocolConformances() &&
             "couldn't find concrete conformance for concrete type?");
      substConformances.push_back(ProtocolConformanceRef(proto));
    }
  }
  assert(substConformances.size() == Conformance.size());

  ArrayRef<ProtocolConformanceRef> substConfs;
  if (conformancesChanged)
    substConfs = module->getASTContext().AllocateCopy(substConformances);
  else
    substConfs = Conformance;

  return Substitution{substReplacement, substConfs};
}
