//===--- ConstraintSystem.cpp - Constraint-based Type Checking ------------===//
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
// This file implements the constraint-based type checker, anchored by the
// \c ConstraintSystem class, which provides type checking and type
// inference for expressions.
//
//===----------------------------------------------------------------------===//
#include "ConstraintSystem.h"
#include "ConstraintGraph.h"
#include "swift/AST/GenericEnvironment.h"
#include "llvm/ADT/SmallString.h"

using namespace swift;
using namespace constraints;

ConstraintSystem::ConstraintSystem(TypeChecker &tc, DeclContext *dc,
                                   ConstraintSystemOptions options)
  : TC(tc), DC(dc), Options(options),
    Arena(tc.Context, Allocator),
    CG(*new ConstraintGraph(*this))
{
  assert(DC && "context required");
}

ConstraintSystem::~ConstraintSystem() {
  delete &CG;
}

bool ConstraintSystem::hasFreeTypeVariables() {
  // Look for any free type variables.
  for (auto tv : TypeVariables) {
    if (!tv->getImpl().hasRepresentativeOrFixed()) {
      return true;
    }
  }
  
  return false;
}

void ConstraintSystem::addTypeVariable(TypeVariableType *typeVar) {
  TypeVariables.push_back(typeVar);
  
  // Notify the constraint graph.
  (void)CG[typeVar];
}

void ConstraintSystem::mergeEquivalenceClasses(TypeVariableType *typeVar1,
                                               TypeVariableType *typeVar2,
                                               bool updateWorkList) {
  assert(typeVar1 == getRepresentative(typeVar1) &&
         "typeVar1 is not the representative");
  assert(typeVar2 == getRepresentative(typeVar2) &&
         "typeVar2 is not the representative");
  assert(typeVar1 != typeVar2 && "cannot merge type with itself");
  typeVar1->getImpl().mergeEquivalenceClasses(typeVar2, getSavedBindings());

  // Merge nodes in the constraint graph.
  CG.mergeNodes(typeVar1, typeVar2);

  if (updateWorkList) {
    addTypeVariableConstraintsToWorkList(typeVar1);
  }
}

/// Determine whether the given type variables occurs in the given type.
bool ConstraintSystem::typeVarOccursInType(TypeVariableType *typeVar,
                                           Type type,
                                           bool *involvesOtherTypeVariables) {
  SmallVector<TypeVariableType *, 4> typeVars;
  type->getTypeVariables(typeVars);
  bool result = false;
  for (auto referencedTypeVar : typeVars) {
    if (referencedTypeVar == typeVar) {
      result = true;
      if (!involvesOtherTypeVariables || *involvesOtherTypeVariables)
        break;

      continue;
    }

    if (involvesOtherTypeVariables)
      *involvesOtherTypeVariables = true;
  }

  return result;
}

void ConstraintSystem::assignFixedType(TypeVariableType *typeVar, Type type,
                                       bool updateState) {
  
  typeVar->getImpl().assignFixedType(type, getSavedBindings());

  if (!updateState)
    return;

  if (!type->isTypeVariableOrMember()) {
    // If this type variable represents a literal, check whether we picked the
    // default literal type. First, find the corresponding protocol.
    ProtocolDecl *literalProtocol = nullptr;
    // If we have the constraint graph, we can check all type variables in
    // the equivalence class. This is the More Correct path.
    // FIXME: Eliminate the less-correct path.
    auto typeVarRep = getRepresentative(typeVar);
    for (auto tv : CG[typeVarRep].getEquivalenceClass()) {
      auto locator = tv->getImpl().getLocator();
      if (!locator || !locator->getPath().empty())
        continue;

      auto anchor = locator->getAnchor();
      if (!anchor)
        continue;

      literalProtocol = TC.getLiteralProtocol(anchor);
      if (literalProtocol)
        break;
    }

    // If the protocol has a default type, check it.
    if (literalProtocol) {
      if (auto defaultType = TC.getDefaultType(literalProtocol, DC)) {
        // Check whether the nominal types match. This makes sure that we
        // properly handle Array vs. Array<T>.
        if (defaultType->getAnyNominal() != type->getAnyNominal())
          increaseScore(SK_NonDefaultLiteral);
      }
    }
  }

  // Notify the constraint graph.
  CG.bindTypeVariable(typeVar, type);
  addTypeVariableConstraintsToWorkList(typeVar);
}

void ConstraintSystem::setMustBeMaterializableRecursive(Type type)
{
  assert(type->isMaterializable() &&
         "argument to setMustBeMaterializableRecursive may not be inherently "
         "non-materializable");
  type = getFixedTypeRecursive(type, /*wantRValue=*/false);
  if (auto typeVar = type->getAs<TypeVariableType>()) {
    typeVar->getImpl().setMustBeMaterializable(getSavedBindings());
  } else if (auto *tupleTy = type->getAs<TupleType>()) {
    for (auto elt : tupleTy->getElementTypes()) {
      setMustBeMaterializableRecursive(elt);
    }
  }
}

void ConstraintSystem::addTypeVariableConstraintsToWorkList(
       TypeVariableType *typeVar) {
  // Gather the constraints affected by a change to this type variable.
  SmallVector<Constraint *, 8> constraints;
  CG.gatherConstraints(typeVar, constraints);

  // Add any constraints that aren't already active to the worklist.
  for (auto constraint : constraints) {
    if (!constraint->isActive()) {
      ActiveConstraints.splice(ActiveConstraints.end(),
                               InactiveConstraints, constraint);
      constraint->setActive(true);
    }
  }
}

/// Retrieve a dynamic result signature for the given declaration.
static std::tuple<char, ObjCSelector, CanType>
getDynamicResultSignature(ValueDecl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    // Handle functions.
    auto type =
      decl->getInterfaceType()->castTo<AnyFunctionType>()->getResult();
    return std::make_tuple(func->isStatic(), func->getObjCSelector(),
                           type->getCanonicalType());
  }

  if (auto asd = dyn_cast<AbstractStorageDecl>(decl)) {
    // Handle properties and subscripts, anchored by the getter's selector.
    return std::make_tuple(asd->isStatic(), asd->getObjCGetterSelector(),
                           asd->getInterfaceType()->getCanonicalType());
  }

  llvm_unreachable("Not a valid @objc member");
}

LookupResult &ConstraintSystem::lookupMember(Type base, DeclName name) {
  base = base->getCanonicalType();

  // Check whether we've already performed this lookup.
  auto knownMember = MemberLookups.find({base, name});
  if (knownMember != MemberLookups.end())
    return *knownMember->second;

  // Lookup the member.
  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  if (isa<AbstractFunctionDecl>(DC))
    lookupOptions |= NameLookupFlags::KnownPrivate;

  MemberLookups[{base, name}] = None;
  auto lookup = TC.lookupMember(DC, base, name, lookupOptions);
  auto &result = MemberLookups[{base, name}];
  result = std::move(lookup);

  // If we aren't performing dynamic lookup, we're done.
  auto instanceTy = base->getRValueType();
  if (auto metaTy = instanceTy->getAs<AnyMetatypeType>())
    instanceTy = metaTy->getInstanceType();
  auto protoTy = instanceTy->getAs<ProtocolType>();
  if (!*result ||
      !protoTy ||
      !protoTy->getDecl()->isSpecificProtocol(
                             KnownProtocolKind::AnyObject))
    return *result;

  // We are performing dynamic lookup. Filter out redundant results early.
  llvm::DenseSet<std::tuple<char, ObjCSelector, CanType>> known;
  result->filter([&](ValueDecl *decl) -> bool {
    if (decl->isInvalid())
      return false;

    return known.insert(getDynamicResultSignature(decl)).second;
  });

  return *result;
}

ArrayRef<Type> ConstraintSystem::
getAlternativeLiteralTypes(KnownProtocolKind kind) {
  unsigned index;

  switch (kind) {
#define PROTOCOL_WITH_NAME(Id, Name) \
  case KnownProtocolKind::Id: llvm_unreachable("Not a literal protocol");
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, Name)
#include "swift/AST/KnownProtocols.def"

  case KnownProtocolKind::ExpressibleByArrayLiteral:     index = 0; break;
  case KnownProtocolKind::ExpressibleByDictionaryLiteral:index = 1; break;
  case KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral: index = 2;
    break;
  case KnownProtocolKind::ExpressibleByFloatLiteral: index = 3; break;
  case KnownProtocolKind::ExpressibleByIntegerLiteral: index = 4; break;
  case KnownProtocolKind::ExpressibleByStringInterpolation: index = 5; break;
  case KnownProtocolKind::ExpressibleByStringLiteral: index = 6; break;
  case KnownProtocolKind::ExpressibleByNilLiteral: index = 7; break;
  case KnownProtocolKind::ExpressibleByBooleanLiteral: index = 8; break;
  case KnownProtocolKind::ExpressibleByUnicodeScalarLiteral: index = 9; break;
  case KnownProtocolKind::ExpressibleByColorLiteral: index = 10; break;
  case KnownProtocolKind::ExpressibleByImageLiteral: index = 11; break;
  case KnownProtocolKind::ExpressibleByFileReferenceLiteral: index = 12; break;
  }
  static_assert(NumAlternativeLiteralTypes == 13, "Wrong # of literal types");

  // If we already looked for alternative literal types, return those results.
  if (AlternativeLiteralTypes[index])
    return *AlternativeLiteralTypes[index];

  SmallVector<Type, 4> types;

  // Some literal kinds are related.
  switch (kind) {
#define PROTOCOL_WITH_NAME(Id, Name) \
  case KnownProtocolKind::Id: llvm_unreachable("Not a literal protocol");
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, Name)
#include "swift/AST/KnownProtocols.def"

  case KnownProtocolKind::ExpressibleByArrayLiteral:
  case KnownProtocolKind::ExpressibleByDictionaryLiteral:
    break;

  case KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral:
  case KnownProtocolKind::ExpressibleByStringInterpolation:
  case KnownProtocolKind::ExpressibleByStringLiteral:
  case KnownProtocolKind::ExpressibleByUnicodeScalarLiteral:
    break;

  case KnownProtocolKind::ExpressibleByIntegerLiteral:
    // Integer literals can be treated as floating point literals.
    if (auto floatProto = TC.Context.getProtocol(
                            KnownProtocolKind::ExpressibleByFloatLiteral)) {
      if (auto defaultType = TC.getDefaultType(floatProto, DC)) {
        types.push_back(defaultType);
      }
    }
    break;

  case KnownProtocolKind::ExpressibleByFloatLiteral:
    break;

  case KnownProtocolKind::ExpressibleByNilLiteral:
  case KnownProtocolKind::ExpressibleByBooleanLiteral:
    break;
  case KnownProtocolKind::ExpressibleByColorLiteral:
  case KnownProtocolKind::ExpressibleByImageLiteral:
  case KnownProtocolKind::ExpressibleByFileReferenceLiteral:
    break;
  }

  AlternativeLiteralTypes[index] = allocateCopy(types);
  return *AlternativeLiteralTypes[index];
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
                     Expr *anchor,
                     ArrayRef<ConstraintLocator::PathElement> path,
                     unsigned summaryFlags) {
  assert(summaryFlags == ConstraintLocator::getSummaryFlagsForPath(path));

  // Check whether a locator with this anchor + path already exists.
  llvm::FoldingSetNodeID id;
  ConstraintLocator::Profile(id, anchor, path);
  void *insertPos = nullptr;
  auto locator = ConstraintLocators.FindNodeOrInsertPos(id, insertPos);
  if (locator)
    return locator;

  // Allocate a new locator and add it to the set.
  locator = ConstraintLocator::create(getAllocator(), anchor, path,
                                      summaryFlags);
  ConstraintLocators.InsertNode(locator, insertPos);
  return locator;
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
                     const ConstraintLocatorBuilder &builder) {
  // If the builder has an empty path, just extract its base locator.
  if (builder.hasEmptyPath()) {
    return builder.getBaseLocator();
  }

  // We have to build a new locator. Extract the paths from the builder.
  SmallVector<LocatorPathElt, 4> path;
  Expr *anchor = builder.getLocatorParts(path);
  return getConstraintLocator(anchor, path, builder.getSummaryFlags());
}

namespace {
  /// Function object that replaces all occurrences of archetypes and
  /// dependent types with type variables.
  class ReplaceDependentTypes {
    ConstraintSystem &cs;
    ConstraintLocatorBuilder &locator;
    llvm::DenseMap<CanType, TypeVariableType *> &replacements;
    llvm::DenseMap<CanType, Type> dependentMemberReplacements;

  public:
    ReplaceDependentTypes(
        ConstraintSystem &cs,
        ConstraintLocatorBuilder &locator,
        llvm::DenseMap<CanType, TypeVariableType *> &replacements)
      : cs(cs), locator(locator), replacements(replacements) { }

    Type operator()(Type type) {
      // Swift only supports rank-1 polymorphism.
      assert(!type->is<GenericFunctionType>());

      // Preserve parens when opening types.
      if (isa<ParenType>(type.getPointer())) {
        return type;
      }

      // Replace a generic type parameter with its corresponding type variable.
      if (auto genericParam = type->getAs<GenericTypeParamType>()) {
        auto known = replacements.find(genericParam->getCanonicalType());
        
        if (known == replacements.end())
          return cs.createTypeVariable(nullptr, TVO_PrefersSubtypeBinding);
        
        return known->second;
      }

      // Replace a dependent member with a fresh type variable and make it a
      // member of its base type.
      if (auto dependentMember = type->getAs<DependentMemberType>()) {
        // Check whether we've already dealt with this dependent member.
        auto known = dependentMemberReplacements.find(
                       dependentMember->getCanonicalType());
        if (known != dependentMemberReplacements.end())
          return known->second;

        // Replace archetypes in the base type.
        // FIXME: Tracking the dependent members seems unnecessary.
        if (auto base =
            ((*this)(dependentMember->getBase()))->getAs<TypeVariableType>()) {
          auto result =
            DependentMemberType::get(base, dependentMember->getAssocType());
          dependentMemberReplacements[dependentMember->getCanonicalType()] =
            result;
          return result;
        }
      }

      // Open up unbound generic types, turning them into bound generic
      // types with type variables for each parameter.
      if (auto unbound = type->getAs<UnboundGenericType>()) {
        auto parentTy = unbound->getParent();
        if (parentTy)
          parentTy = parentTy.transform(*this);

        auto unboundDecl = unbound->getDecl();
        if (unboundDecl->isInvalid())
          return ErrorType::get(cs.getASTContext());
        
        // If the unbound decl hasn't been validated yet, we have a circular
        // dependency that isn't being diagnosed properly.
        if (!unboundDecl->getGenericSignature()) {
          cs.TC.diagnose(unboundDecl, diag::circular_reference);
          return ErrorType::get(type);
        }
        
        
        // Open up the generic type.
        cs.openGeneric(unboundDecl->getInnermostDeclContext(),
                       unboundDecl->getDeclContext(),
                       unboundDecl->getInnermostGenericParamTypes(),
                       unboundDecl->getGenericRequirements(),
                       /*skipProtocolSelfConstraint=*/false,
                       locator,
                       replacements);
        
        // Map the generic parameters to their corresponding type variables.
        llvm::SmallVector<TypeLoc, 4> arguments;
        for (auto gp : unboundDecl->getInnermostGenericParamTypes()) {
          assert(replacements.count(gp->getCanonicalType()) &&
                 "Missing generic parameter?");
          arguments.push_back(TypeLoc::withoutLoc(
                              replacements[gp->getCanonicalType()]));
        }

        // FIXME: For some reason we can end up with unbound->getDecl()
        // pointing at a generic TypeAliasDecl here. If we find a way to
        // handle generic TypeAliases elsewhere, this can just become a
        // call to BoundGenericType::get().
        return cs.TC.applyUnboundGenericArguments(unbound, unboundDecl,
                                                  SourceLoc(), cs.DC, arguments,
                                                  /*isGenericSignature*/false,
                                                  /*resolver*/nullptr);
      }
      
      return type;
    }
  };
}

Type ConstraintSystem::openType(
       Type startingType,
       ConstraintLocatorBuilder locator,
       llvm::DenseMap<CanType, TypeVariableType *> &replacements) {
  ReplaceDependentTypes replaceDependentTypes(*this, locator, replacements);
  return startingType.transform(replaceDependentTypes);
}

/// Remove argument labels from the function type.
static Type removeArgumentLabels(Type type, unsigned numArgumentLabels) {
  // If there is nothing to remove, don't.
  if (numArgumentLabels == 0) return type;
  
  auto fnType = type->getAs<FunctionType>();

  // Drop argument labels from the input type.
  Type inputType = fnType->getInput();
  if (auto tupleTy = dyn_cast<TupleType>(inputType.getPointer())) {
    SmallVector<TupleTypeElt, 4> elements;
    elements.reserve(tupleTy->getNumElements());
    for (const auto &elt : tupleTy->getElements()) {
      elements.push_back(elt.getWithoutName());
    }
    inputType = TupleType::get(elements, type->getASTContext());
  }

  return FunctionType::get(inputType,
                           removeArgumentLabels(fnType->getResult(),
                                                numArgumentLabels - 1),
                           fnType->getExtInfo());
}

Type ConstraintSystem::openFunctionType(
       AnyFunctionType *funcType,
       unsigned numArgumentLabelsToRemove,
       ConstraintLocatorBuilder locator,
       llvm::DenseMap<CanType, TypeVariableType *> &replacements,
       DeclContext *innerDC,
       DeclContext *outerDC,
       bool skipProtocolSelfConstraint) {
  Type type;

  if (auto *genericFn = funcType->getAs<GenericFunctionType>()) {
    // Open up the generic parameters and requirements.
    openGeneric(innerDC,
                outerDC,
                genericFn->getGenericSignature(),
                skipProtocolSelfConstraint,
                locator,
                replacements);

    // Transform the input and output types.
    Type inputTy = openType(genericFn->getInput(), locator, replacements);
    if (!inputTy)
      return Type();

    Type resultTy = openType(genericFn->getResult(), locator, replacements);
    if (!resultTy)
      return Type();

    // Build the resulting (non-generic) function type.
    type = FunctionType::get(inputTy, resultTy,
                             FunctionType::ExtInfo().
                               withThrows(genericFn->throws()));
  } else {
    type = openType(funcType, locator, replacements);
    if (!type) return Type();
  }

  return removeArgumentLabels(type, numArgumentLabelsToRemove);
}

bool ConstraintSystem::isArrayType(Type t) {
  t = t->getDesugaredType();
  
  // ArraySliceType<T> desugars to Array<T>.
  if (isa<ArraySliceType>(t.getPointer()))
    return true;
  if (auto boundStruct = dyn_cast<BoundGenericStructType>(t.getPointer())) {
    return boundStruct->getDecl() == TC.Context.getArrayDecl();
  }
  
  return false;
}

Optional<std::pair<Type, Type>> ConstraintSystem::isDictionaryType(Type type) {
  if (auto boundStruct = type->getAs<BoundGenericStructType>()) {
    if (boundStruct->getDecl() != TC.Context.getDictionaryDecl())
      return None;

    auto genericArgs = boundStruct->getGenericArgs();
    return std::make_pair(genericArgs[0], genericArgs[1]);
  }

  return None;
}

bool ConstraintSystem::isSetType(Type type) {
  if (auto boundStruct = type->getAs<BoundGenericStructType>()) {
    return boundStruct->getDecl() == TC.Context.getSetDecl();
  }

  return false;
}

bool ConstraintSystem::isAnyHashableType(Type type) {
  if (auto st = type->getAs<StructType>()) {
    return st->getDecl() == TC.Context.getAnyHashableDecl();
  }

  return false;
}

Type ConstraintSystem::openBindingType(Type type, 
                                       ConstraintLocatorBuilder locator) {
  Type result = openType(type, locator);
  
  if (isArrayType(type)) {
    auto boundStruct = type->getAs<BoundGenericStructType>();
    if (auto replacement = getTypeChecker().getArraySliceType(
                             SourceLoc(), boundStruct->getGenericArgs()[0])) {
      return replacement;
    }
  }

  if (auto dict = isDictionaryType(type)) {
    if (auto replacement = getTypeChecker().getDictionaryType(
                             SourceLoc(), dict->first, dict->second))
      return replacement;
  }

  return result;
}

Type ConstraintSystem::getFixedTypeRecursive(Type type,
                                             TypeMatchOptions &flags,
                                             bool wantRValue,
                                             bool retainParens) {

  if (wantRValue)
    type = type->getRValueType();

  if (retainParens) {
    if (auto parenTy = dyn_cast<ParenType>(type.getPointer())) {
      type = getFixedTypeRecursive(parenTy->getUnderlyingType(), flags,
                                   wantRValue, retainParens);
      return ParenType::get(getASTContext(), type);
    }
  }

  while (true) {
    if (auto depMemType = type->getAs<DependentMemberType>()) {
      if (!depMemType->getBase()->isTypeVariableOrMember()) return type;

      // FIXME: Perform a more limited simplification?
      Type newType = simplifyType(type);
      if (newType.getPointer() == type.getPointer()) return type;

      if (wantRValue)
        newType = newType->getRValueType();

      type = newType;

      // Once we've simplified a dependent member type, we need to generate a
      // new constraint.
      flags |= TMF_GenerateConstraints;
      continue;
    }

    if (auto typeVar = type->getAs<TypeVariableType>()) {
      if (auto fixed = getFixedType(typeVar)) {
        if (wantRValue)
          fixed = fixed->getRValueType();

        type = fixed;
        continue;
      }
      break;
    }

    break;
  }

  return type;
}

void ConstraintSystem::recordOpenedTypes(
       ConstraintLocatorBuilder locator,
       const llvm::DenseMap<CanType, TypeVariableType *> &replacements) {
  if (replacements.empty())
    return;

  // If the last path element is an archetype or associated type, ignore it.
  SmallVector<LocatorPathElt, 2> pathElts;
  Expr *anchor = locator.getLocatorParts(pathElts);
  if (!pathElts.empty() &&
      (pathElts.back().getKind() == ConstraintLocator::Archetype ||
       pathElts.back().getKind() == ConstraintLocator::AssociatedType))
    return;

  // If the locator is empty, ignore it.
  if (!anchor && pathElts.empty())
    return;

  ConstraintLocator *locatorPtr = getConstraintLocator(locator);
  assert(locatorPtr && "No locator for opened types?");
  assert(std::find_if(OpenedTypes.begin(), OpenedTypes.end(),
                      [&](const std::pair<ConstraintLocator *,
                          ArrayRef<OpenedType>> &entry) {
                        return entry.first == locatorPtr;
                      }) == OpenedTypes.end() &&
         "already registered opened types for this locator");

  OpenedType* openedTypes
    = Allocator.Allocate<OpenedType>(replacements.size());
  std::copy(replacements.begin(), replacements.end(), openedTypes);
  OpenedTypes.push_back({ locatorPtr,
    llvm::makeArrayRef(openedTypes,
                       replacements.size()) });
}

/// Determine how many levels of argument labels should be removed from the
/// function type when referencing the given declaration.
static unsigned getNumRemovedArgumentLabels(ASTContext &ctx, ValueDecl *decl,
                                            bool isCurriedInstanceReference,
                                            FunctionRefKind functionRefKind) {
  // Only applicable to functions. Nothing else should have argument labels in
  // the type.
  auto func = dyn_cast<AbstractFunctionDecl>(decl);
  if (!func) return 0;

  switch (functionRefKind) {
  case FunctionRefKind::Unapplied:
  case FunctionRefKind::Compound:
    // Always remove argument labels from unapplied references and references
    // that use a compound name.
    return func->getNumParameterLists();

  case FunctionRefKind::SingleApply:
    // If we have fewer than two parameter lists, leave the labels.
    if (func->getNumParameterLists() < 2) return 0;

    // If this is a curried reference to an instance method, where 'self' is
    // being applied, e.g., "ClassName.instanceMethod(self)", remove the
    // argument labels from the resulting function type. The 'self' parameter is
    // always unlabeled, so this operation is a no-op for the actual application.
    return isCurriedInstanceReference ? func->getNumParameterLists() : 1;

  case FunctionRefKind::DoubleApply:
    // Never remove argument labels from a double application.
    return 0;
  }

  llvm_unreachable("Unhandled FunctionRefKind in switch.");
}

std::pair<Type, Type>
ConstraintSystem::getTypeOfReference(ValueDecl *value,
                                     bool isTypeReference,
                                     bool isSpecialized,
                                     FunctionRefKind functionRefKind,
                                     ConstraintLocatorBuilder locator,
                                     const DeclRefExpr *base) {
  llvm::DenseMap<CanType, TypeVariableType *> replacements;

  if (value->getDeclContext()->isTypeContext() && isa<FuncDecl>(value)) {
    // Unqualified lookup can find operator names within nominal types.
    auto func = cast<FuncDecl>(value);
    assert(func->isOperator() && "Lookup should only find operators");

    auto openedType = openFunctionType(
            func->getInterfaceType()->castTo<AnyFunctionType>(),
            /*numRemovedArgumentLabels=*/0,
            locator, replacements,
            func->getInnermostDeclContext(),
            func->getDeclContext(),
            /*skipProtocolSelfConstraint=*/false);
    auto openedFnType = openedType->castTo<FunctionType>();
    
    // If this is a method whose result type is dynamic Self, replace
    // DynamicSelf with the actual object type.
    if (func->hasDynamicSelf()) {
      Type selfTy = openedFnType->getInput()->getRValueInstanceType();
      openedType = openedType->replaceCovariantResultType(
                     selfTy,
                     func->getNumParameterLists());
      openedFnType = openedType->castTo<FunctionType>();
    }

    // If we opened up any type variables, record the replacements.
    recordOpenedTypes(locator, replacements);

    // The reference implicitly binds 'self'.
    return { openedType, openedFnType->getResult() };
  }

  // If we have a type declaration, resolve it within the current context.
  if (auto typeDecl = dyn_cast<TypeDecl>(value)) {
    // Resolve the reference to this type declaration in our current context.
    auto type = getTypeChecker().resolveTypeInContext(typeDecl, DC,
                                                      TR_InExpression,
                                                      isSpecialized);
    if (!type)
      return { nullptr, nullptr };

    // Open the type.
    type = openType(type, locator, replacements);

    // If we opened up any type variables, record the replacements.
    recordOpenedTypes(locator, replacements);

    // If it's a type reference or it's a module type, we're done.
    if (isTypeReference || type->is<ModuleType>())
      return { type, type };

    // If it's a value reference, refer to the metatype.
    type = MetatypeType::get(type);
    return { type, type };
  }

  // Determine the type of the value, opening up that type if necessary.
  bool wantInterfaceType = true;
  if (isa<VarDecl>(value))
    wantInterfaceType = !value->getDeclContext()->isLocalContext();
  Type valueType = TC.getUnopenedTypeOfReference(value, Type(), DC, base,
                                                 wantInterfaceType);

  // If this is a let-param whose type is a type variable, this is an untyped
  // closure param that may be bound to an inout type later. References to the
  // param should have lvalue type instead. Express the relationship with a new
  // constraint.
  if (auto *param = dyn_cast<ParamDecl>(value)) {
    if (param->isLet() && valueType->is<TypeVariableType>()) {
      Type paramType = valueType;
      valueType = createTypeVariable(getConstraintLocator(locator),
                                     TVO_CanBindToLValue);
      addConstraint(ConstraintKind::BindParam, paramType, valueType,
                    getConstraintLocator(locator));
    }
  }

  // Adjust the type of the reference.
  if (auto funcType = valueType->getAs<AnyFunctionType>()) {
    valueType =
      openFunctionType(
          funcType,
          getNumRemovedArgumentLabels(TC.Context, value,
                                      /*isCurriedInstanceReference=*/false,
                                      functionRefKind),
          locator, replacements,
          value->getInnermostDeclContext(),
          value->getDeclContext(),
          /*skipProtocolSelfConstraint=*/false);
  } else {
    valueType = openType(valueType, locator, replacements);
  }

  // If we opened up any type variables, record the replacements.
  recordOpenedTypes(locator, replacements);

  return { valueType, valueType };
}

void ConstraintSystem::openGeneric(
       DeclContext *innerDC,
       DeclContext *outerDC,
       GenericSignature *signature,
       bool skipProtocolSelfConstraint,
       ConstraintLocatorBuilder locator,
       llvm::DenseMap<CanType, TypeVariableType *> &replacements) {
  openGeneric(innerDC,
              outerDC,
              signature->getGenericParams(),
              signature->getRequirements(),
              skipProtocolSelfConstraint,
              locator,
              replacements);
}

/// Bind type variables for archetypes that are determined from
/// context.
///
/// For example, if we are opening a generic function type
/// nested inside another function, we must bind the outer
/// generic parameters to context archetypes, because the
/// nested function can "capture" these outer generic parameters.
///
/// Another case where this comes up is if a generic type is
/// nested inside a function. We don't support codegen for this
/// yet, but again we need to bind any outer generic parameters
/// to context archetypes, because they're not free.
///
/// A final case we have to handle, even though it is invalid, is
/// when a type is nested inside another protocol. We bind the
/// protocol type variable for the protocol Self to its archetype
/// in protocol context. This of course makes no sense, but we
/// can't leave the type variable dangling, because then we crash
/// later.
///
/// If we ever do want to allow nominal types to be nested inside
/// protocols, the key is to set their declared type to a
/// NominalType whose parent is the 'Self' generic parameter, and
/// not the ProtocolType. Then, within a conforming type context,
/// we can 'reparent' the NominalType to that concrete type, and
/// resolve references to associated types inside that NominalType
/// relative to this concrete 'Self' type.
///
/// Also, of course IRGen would have to know to store the 'Self'
/// metadata as an extra hidden generic parameter in the metadata
/// of such a type, etc.
static void bindArchetypesFromContext(
    ConstraintSystem &cs,
    DeclContext *outerDC,
    ConstraintLocator *locatorPtr,
    const llvm::DenseMap<CanType, TypeVariableType *> &replacements) {

  auto *genericEnv = outerDC->getGenericEnvironmentOfContext();

  for (const auto *parentDC = outerDC;
       !parentDC->isModuleScopeContext();
       parentDC = parentDC->getParent()) {
    if (parentDC->isTypeContext() &&
        (parentDC == outerDC ||
         !parentDC->getAsProtocolOrProtocolExtensionContext()))
      continue;

    auto *genericSig = parentDC->getGenericSignatureOfContext();
    if (!genericSig)
      break;

    for (auto *paramTy : genericSig->getGenericParams()) {
      auto found = replacements.find(paramTy->getCanonicalType());

      // We might not have a type variable for this generic parameter
      // because either we're opening up an UnboundGenericType,
      // in which case we only want to infer the innermost generic
      // parameters, or because this generic parameter was constrained
      // away into a concrete type.
      if (found != replacements.end()) {
        auto typeVar = found->second;
        auto contextTy = genericEnv->mapTypeIntoContext(paramTy);
        cs.addConstraint(ConstraintKind::Bind, typeVar, contextTy,
                         locatorPtr);
      }
    }

    break;
  }
}

void ConstraintSystem::openGeneric(
       DeclContext *innerDC,
       DeclContext *outerDC,
       ArrayRef<GenericTypeParamType *> params,
       ArrayRef<Requirement> requirements,
       bool skipProtocolSelfConstraint,
       ConstraintLocatorBuilder locator,
       llvm::DenseMap<CanType, TypeVariableType *> &replacements) {
  auto locatorPtr = getConstraintLocator(locator);
  auto *genericEnv = innerDC->getGenericEnvironmentOfContext();

  // Create the type variables for the generic parameters.
  for (auto gp : params) {
    auto contextTy = genericEnv->mapTypeIntoContext(gp);
    if (auto *archetype = contextTy->getAs<ArchetypeType>())
      locatorPtr = getConstraintLocator(
          locator.withPathElement(LocatorPathElt(archetype)));

    auto typeVar = createTypeVariable(locatorPtr,
                                      TVO_PrefersSubtypeBinding |
                                      TVO_MustBeMaterializable);
    replacements[gp->getCanonicalType()] = typeVar;
  }

  ReplaceDependentTypes replaceDependentTypes(*this, locator, replacements);

  // Remember that any new constraints generated by opening this generic are
  // due to the opening.
  locatorPtr = getConstraintLocator(
                     locator.withPathElement(ConstraintLocator::OpenedGeneric));

  bindArchetypesFromContext(*this, outerDC, locatorPtr, replacements);

  // Add the requirements as constraints.
  for (auto req : requirements) {
  switch (req.getKind()) {
    case RequirementKind::Conformance: {
      auto subjectTy = req.getFirstType().transform(replaceDependentTypes);
      auto proto = req.getSecondType()->castTo<ProtocolType>();
      auto protoDecl = proto->getDecl();

      // Determine whether this is the protocol 'Self' constraint we should
      // skip.
      if (skipProtocolSelfConstraint &&
          protoDecl == outerDC &&
          (protoDecl->getSelfInterfaceType()->getCanonicalType() ==
           req.getFirstType()->getCanonicalType())) {
        break;
      }

      addConstraint(ConstraintKind::ConformsTo, subjectTy, proto,
                    locatorPtr);
      break;
    }

    case RequirementKind::Superclass: {
      auto subjectTy = req.getFirstType().transform(replaceDependentTypes);
      auto boundTy = req.getSecondType().transform(replaceDependentTypes);
      addConstraint(ConstraintKind::Subtype, subjectTy, boundTy, locatorPtr);
      break;
    }

    case RequirementKind::SameType: {
      auto firstTy = req.getFirstType().transform(replaceDependentTypes);
      auto secondTy = req.getSecondType().transform(replaceDependentTypes);
      addConstraint(ConstraintKind::Bind, firstTy, secondTy, locatorPtr);
      break;
    }
  }
  }
}

/// Add the constraint on the type used for the 'Self' type for a member
/// reference.
///
/// \param cs The constraint system.
///
/// \param objectTy The type of the object that we're using to access the
/// member.
///
/// \param selfTy The instance type of the context in which the member is
/// declared.
static void addSelfConstraint(ConstraintSystem &cs, Type objectTy, Type selfTy,
                              ConstraintLocatorBuilder locator){
  assert(!selfTy->is<ProtocolType>());

  // Otherwise, use a subtype constraint for classes to cope with inheritance.
  if (selfTy->getClassOrBoundGenericClass()) {
    cs.addConstraint(ConstraintKind::Subtype, objectTy, selfTy,
                     cs.getConstraintLocator(locator));
    return;
  }

  // Otherwise, the types must be equivalent.
  cs.addConstraint(ConstraintKind::Equal, objectTy, selfTy,
                   cs.getConstraintLocator(locator));
}

/// Determine whether the given locator is for a witness or requirement.
static bool isRequirementOrWitness(const ConstraintLocatorBuilder &locator) {
  if (auto last = locator.last()) {
    return last->getKind() == ConstraintLocator::Requirement ||
    last->getKind() == ConstraintLocator::Witness;
  }

  return false;
}

std::pair<Type, Type>
ConstraintSystem::getTypeOfMemberReference(
    Type baseTy, ValueDecl *value,
    bool isTypeReference,
    bool isDynamicResult,
    FunctionRefKind functionRefKind,
    ConstraintLocatorBuilder locator,
    const DeclRefExpr *base,
    llvm::DenseMap<CanType, TypeVariableType *> *replacementsPtr) {
  // Figure out the instance type used for the base.
  Type baseObjTy = getFixedTypeRecursive(baseTy, /*wantRValue=*/true);
  bool isInstance = true;
  if (auto baseMeta = baseObjTy->getAs<AnyMetatypeType>()) {
    baseObjTy = baseMeta->getInstanceType();
    isInstance = false;
  }

  // If the base is a module type, just use the type of the decl.
  if (baseObjTy->is<ModuleType>()) {
    return getTypeOfReference(value, isTypeReference, /*isSpecialized=*/false,
                              functionRefKind, locator, base);
  }

  // Don't open existentials when accessing typealias members of
  // protocols.
  if (auto *alias = dyn_cast<TypeAliasDecl>(value)) {
    if (baseObjTy->isExistentialType()) {
      auto memberTy = alias->getUnderlyingType();
      auto openedType = FunctionType::get(baseObjTy, memberTy);
      return { openedType, memberTy };
    }
  }

  // Handle associated type lookup as a special case, horribly.
  // FIXME: This is an awful hack.
  if (auto assocType = dyn_cast<AssociatedTypeDecl>(value)) {
    // Error recovery path.
    if (baseObjTy->isOpenedExistential()) {
      Type memberTy = ErrorType::get(TC.Context);
      auto openedType = FunctionType::get(baseObjTy, memberTy);
      return { openedType, memberTy };
    }

    // Refer to a member of the archetype directly.
    if (auto archetype = baseObjTy->getAs<ArchetypeType>()) {
      Type memberTy = archetype->getNestedTypeValue(value->getName());
      if (!isTypeReference)
        memberTy = MetatypeType::get(memberTy);

      auto openedType = FunctionType::get(baseObjTy, memberTy);
      return { openedType, memberTy };
    }

    // If we have a nominal type that conforms to the protocol in which the
    // associated type resides, use the witness.
    if (!baseObjTy->isExistentialType() &&
        baseObjTy->getAnyNominal()) {
      auto proto = cast<ProtocolDecl>(assocType->getDeclContext());
      if (auto conformance =
            TC.conformsToProtocol(baseObjTy, proto, DC,
                                  ConformanceCheckFlags::InExpression)) {
        if (conformance->isConcrete()) {
          auto memberTy = conformance->getConcrete()->getTypeWitness(assocType,
                                                                     &TC)
            .getReplacement();
          if (!isTypeReference)
            memberTy = MetatypeType::get(memberTy);

          auto openedType = FunctionType::get(baseObjTy, memberTy);
          return { openedType, memberTy };
        }
      }
    }

    // FIXME: Totally bogus fallthrough.
    Type memberTy = isTypeReference? assocType->getDeclaredType()
                                   : assocType->getType();
    auto openedType = FunctionType::get(baseObjTy, memberTy);
    return { openedType, memberTy };
  }

  // Figure out the declaration context to use when opening this type.
  DeclContext *innerDC = value->getInnermostDeclContext();
  DeclContext *outerDC = value->getDeclContext();

  // Open the type of the generic function or member of a generic type.
  Type openedType;
  auto isClassBoundExistential = false;
  llvm::DenseMap<CanType, TypeVariableType *> localReplacements;
  auto &replacements = replacementsPtr ? *replacementsPtr : localReplacements;
  bool isCurriedInstanceReference = value->isInstanceMember() && !isInstance;
  unsigned numRemovedArgumentLabels =
    getNumRemovedArgumentLabels(TC.Context, value, isCurriedInstanceReference,
                                functionRefKind);

  if (auto genericFn = value->getInterfaceType()->getAs<GenericFunctionType>()){
    openedType = openFunctionType(genericFn, numRemovedArgumentLabels,
                                  locator, replacements, innerDC, outerDC,
                                  /*skipProtocolSelfConstraint=*/true);
  } else {
    openedType = TC.getUnopenedTypeOfReference(value, baseTy, DC, base,
                                               /*wantInterfaceType=*/true);

    // The type of 'Self' that will be added if the declaration
    // is not naturally a function type with a 'Self' parameter.
    Type selfTy;
    if (auto sig = innerDC->getGenericSignatureOfContext()) {

      // Open up the generic parameter list for the container.
      openGeneric(innerDC, outerDC, sig,
                  /*skipProtocolSelfConstraint=*/true,
                  locator, replacements);

      // Open up the type of the member.
      openedType = openType(openedType, locator, replacements);

      // Determine the object type of 'self'.
      auto nominal = outerDC->getAsNominalTypeOrNominalTypeExtensionContext();

      // We want to track if the generic context is represented by a
      // class-bound existential so we won't inappropriately wrap the
      // self type in an inout later on.
      if (auto metatype = nominal->getType()->getAs<AnyMetatypeType>()) {
        isClassBoundExistential = metatype->getInstanceType()->
                                            isClassExistentialType();
      }

      if (outerDC->getAsProtocolOrProtocolExtensionContext()) {
        // Retrieve the type variable for 'Self'.
        selfTy = replacements[outerDC->getSelfInterfaceType()
                                     ->getCanonicalType()];
      } else {
        // Open the nominal type.
        selfTy = openType(nominal->getDeclaredInterfaceType(), locator,
                          replacements);
      }
    } else {
      selfTy = outerDC->getDeclaredTypeOfContext();
    }

    // Remove argument labels, if needed.
    openedType = removeArgumentLabels(openedType, numRemovedArgumentLabels);

    // If we have a type reference, look through the metatype.
    if (isTypeReference)
      openedType = openedType->castTo<AnyMetatypeType>()->getInstanceType();

    // If we're not coming from something function-like, prepend the type
    // for 'self' to the type.
    if (!isa<AbstractFunctionDecl>(value) && !isa<EnumElementDecl>(value)) {
      // If self is a struct, properly qualify it based on our base
      // qualification.  If we have an lvalue coming in, we expect an inout.
      if (!isClassBoundExistential &&
          !selfTy->hasReferenceSemantics() &&
          baseTy->is<LValueType>() &&
          !selfTy->hasError())
        selfTy = InOutType::get(selfTy);

      openedType = FunctionType::get(selfTy, openedType);
    }
  }

  // If this is a method whose result type has a dynamic Self return, replace
  // DynamicSelf with the actual object type.
  if (auto func = dyn_cast<FuncDecl>(value)) {
    if (func->hasDynamicSelf() ||
        (baseObjTy->isExistentialType() &&
         func->hasArchetypeSelf())) {
      openedType = openedType->replaceCovariantResultType(
                     baseObjTy,
                     func->getNumParameterLists());
    }
  }
  // If this is an initializer, replace the result type with the base
  // object type.
  else if (auto ctor = dyn_cast<ConstructorDecl>(value)) {
    auto resultTy = baseObjTy;
    if (ctor->getFailability() != OTK_None)
      resultTy = OptionalType::get(ctor->getFailability(), resultTy);

    openedType = openedType->replaceCovariantResultType(
                     resultTy,
                     /*uncurryLevel=*/ 2,
                     /*preserveOptionality=*/ false);
  }

  // If we are looking at a member of an existential, open the existential.
  Type baseOpenedTy = baseObjTy;

  if (baseObjTy->isExistentialType()) {
    ArchetypeType *openedArchetype = ArchetypeType::getOpened(baseObjTy);
    OpenedExistentialTypes.push_back({ getConstraintLocator(locator),
                                       openedArchetype });
    baseOpenedTy = openedArchetype;
  }

  // Constrain the 'self' object type.
  auto openedFnType = openedType->castTo<FunctionType>();
  Type selfObjTy = openedFnType->getInput()->getRValueInstanceType();
  if (outerDC->getAsProtocolOrProtocolExtensionContext()) {
    // For a protocol, substitute the base object directly. We don't need a
    // conformance constraint because we wouldn't have found the declaration
    // if it didn't conform.
    addConstraint(ConstraintKind::Equal, baseOpenedTy, selfObjTy,
                  getConstraintLocator(locator));
  } else if (!isDynamicResult) {
    addSelfConstraint(*this, baseOpenedTy, selfObjTy, locator);
  }

  // Compute the type of the reference.
  Type type;
  if (auto subscript = dyn_cast<SubscriptDecl>(value)) {
    // For a subscript, turn the element type into an (@unchecked)
    // optional or lvalue, depending on whether the result type is
    // optional/dynamic, is settable, or is not.
    auto fnType = openedFnType->getResult()->castTo<FunctionType>();
    auto elementTy = fnType->getResult();
    if (!isRequirementOrWitness(locator)) {
      if (subscript->getAttrs().hasAttribute<OptionalAttr>())
        elementTy = OptionalType::get(elementTy->getRValueType());
      else if (isDynamicResult) {
        elementTy = ImplicitlyUnwrappedOptionalType::get(
                      elementTy->getRValueType());
      }
    }

    type = FunctionType::get(fnType->getInput(), elementTy);
  } else if (isa<ProtocolDecl>(outerDC) &&
             isa<AssociatedTypeDecl>(value)) {
    // When we have an associated type, the base type conforms to the
    // given protocol, so use the type witness directly.
    // FIXME: Diagnose existentials properly.
    auto proto = cast<ProtocolDecl>(outerDC);
    auto assocType = cast<AssociatedTypeDecl>(value);

    type = openedFnType->getResult();
    if (baseOpenedTy->is<ArchetypeType>()) {
      // For an archetype, we substitute the base object for the base.
      // FIXME: Feels like a total hack.
    } else if (!baseOpenedTy->isExistentialType() &&
               !baseOpenedTy->is<ArchetypeType>()) {
      if (auto conformance =
            TC.conformsToProtocol(baseOpenedTy, proto, DC,
                                  ConformanceCheckFlags::InExpression)) {
        if (conformance->isConcrete()) {
          type = conformance->getConcrete()->getTypeWitness(assocType, &TC)
                   .getReplacement();
        }
      }
    }
  } else if (!value->isInstanceMember() || isInstance) {
    // For a constructor, enum element, static method, static property,
    // or an instance method referenced through an instance, we've consumed the
    // curried 'self' already. For a type, strip off the 'self' we artificially
    // added.
    type = openedFnType->getResult();
  } else if (isDynamicResult && isa<AbstractFunctionDecl>(value)) {
    // For a dynamic result referring to an instance function through
    // an object of metatype type, replace the 'Self' parameter with
    // a AnyObject member.
    Type anyObjectTy = TC.getProtocol(SourceLoc(),
                                      KnownProtocolKind::AnyObject)
                                          ->getDeclaredTypeOfContext();

    type = openedFnType->replaceSelfParameterType(anyObjectTy);
  } else {
    // For an unbound instance method reference, replace the 'Self'
    // parameter with the base type.
    type = openedFnType->replaceSelfParameterType(baseObjTy);
  }

  // If we opened up any type variables, record the replacements.
  recordOpenedTypes(locator, replacements);

  return { openedType, type };
}

void ConstraintSystem::addOverloadSet(Type boundType,
                                      ArrayRef<OverloadChoice> choices,
                                      ConstraintLocator *locator,
                                      OverloadChoice *favoredChoice) {
  assert(!choices.empty() && "Empty overload set");

  // If there is a single choice, add the bind overload directly.
  if (choices.size() == 1) {
    addBindOverloadConstraint(boundType, choices.front(), locator);
    return;
  }

  SmallVector<Constraint *, 4> overloads;
  
  // As we do for other favored constraints, if a favored overload has been
  // specified, let it be the first term in the disjunction.
  if (favoredChoice) {
    auto bindOverloadConstraint =
        Constraint::createBindOverload(*this,
                                       boundType,
                                       *favoredChoice,
                                       locator);
    
    bindOverloadConstraint->setFavored();
    
    overloads.push_back(bindOverloadConstraint);
  }
  
  for (auto choice : choices) {
    if (favoredChoice && (favoredChoice == &choice))
      continue;
    
    overloads.push_back(Constraint::createBindOverload(*this, boundType, choice,
                                                       locator));
  }

  addDisjunctionConstraint(overloads, locator, ForgetChoice, favoredChoice);
}

void ConstraintSystem::resolveOverload(ConstraintLocator *locator,
                                       Type boundType,
                                       OverloadChoice choice) {
  // Determine the type to which we'll bind the overload set's type.
  Type refType;
  Type openedFullType;
  switch (choice.getKind()) {
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaDynamic:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
  case OverloadChoiceKind::TypeDecl: {
    bool isTypeReference = choice.getKind() == OverloadChoiceKind::TypeDecl;
    bool isDynamicResult
      = choice.getKind() == OverloadChoiceKind::DeclViaDynamic;
    // Retrieve the type of a reference to the specific declaration choice.
    if (choice.getBaseType()) {
      auto getDotBase = [](const Expr *E) -> const DeclRefExpr * {
        if (E == nullptr) return nullptr;
        switch (E->getKind()) {
        case ExprKind::MemberRef: {
          auto Base = cast<MemberRefExpr>(E)->getBase();
          return dyn_cast<const DeclRefExpr>(Base);
        }
        case ExprKind::UnresolvedDot: {
          auto Base = cast<UnresolvedDotExpr>(E)->getBase();
          return dyn_cast<const DeclRefExpr>(Base);
        }
        default:
          return nullptr;
        }
      };
      auto anchor = locator ? locator->getAnchor() : nullptr;
      auto base = getDotBase(anchor);
      std::tie(openedFullType, refType)
        = getTypeOfMemberReference(choice.getBaseType(), choice.getDecl(),
                                   isTypeReference, isDynamicResult,
                                   choice.getFunctionRefKind(),
                                   locator, base, nullptr);
    } else {
      std::tie(openedFullType, refType)
        = getTypeOfReference(choice.getDecl(), isTypeReference,
                             choice.isSpecialized(),
                             choice.getFunctionRefKind(), locator);
    }

    if (!isRequirementOrWitness(locator) &&
        choice.getDecl()->getAttrs().hasAttribute<OptionalAttr>() &&
        !isa<SubscriptDecl>(choice.getDecl())) {
      // For a non-subscript declaration that is an optional
      // requirement in a protocol, strip off the lvalue-ness (FIXME:
      // one cannot assign to such declarations for now) and make a
      // reference to that declaration be optional.
      //
      // Subscript declarations are handled within
      // getTypeOfMemberReference(); their result types are optional.
      refType = OptionalType::get(refType->getRValueType());
    } 
    // For a non-subscript declaration found via dynamic lookup, strip
    // off the lvalue-ness (FIXME: as a temporary hack. We eventually
    // want this to work) and make a reference to that declaration be
    // an implicitly unwrapped optional.
    //
    // Subscript declarations are handled within
    // getTypeOfMemberReference(); their result types are unchecked
    // optional.
    else if (isDynamicResult && !isa<SubscriptDecl>(choice.getDecl())) {    
      refType = ImplicitlyUnwrappedOptionalType::get(refType->getRValueType());
    } 

    // If the declaration is unavailable, note that in the score.
    if (choice.getDecl()->getAttrs().isUnavailable(getASTContext())) {
      increaseScore(SK_Unavailable);
    }

    break;
  }

  case OverloadChoiceKind::BaseType:
    refType = choice.getBaseType();
    break;

  case OverloadChoiceKind::TupleIndex:
    if (auto lvalueTy = choice.getBaseType()->getAs<LValueType>()) {
      // When the base of a tuple lvalue, the member is always an lvalue.
      auto tuple = lvalueTy->getObjectType()->castTo<TupleType>();
      refType = tuple->getElementType(choice.getTupleIndex())->getRValueType();
      refType = LValueType::get(refType);
    } else {
      // When the base is a tuple rvalue, the member is always an rvalue.
      auto tuple = choice.getBaseType()->castTo<TupleType>();
      refType = tuple->getElementType(choice.getTupleIndex())->getRValueType();
    }
    break;
  }
  
  assert(!refType->hasTypeParameter() && "Cannot have a dependent type here");
  
  // If we're binding to an init member, the 'throws' need to line up between
  // the bound and reference types.
  if (choice.isDecl()) {
    auto decl = choice.getDecl();
    if (auto CD = dyn_cast<ConstructorDecl>(decl)) {
      auto boundFunctionType = boundType->getAs<AnyFunctionType>();
        
      if (boundFunctionType &&
          CD->hasThrows() != boundFunctionType->throws()) {
        boundType = FunctionType::get(boundFunctionType->getInput(),
                                      boundFunctionType->getResult(),
                                      boundFunctionType->getExtInfo().
                                                          withThrows());
      }
    }
  }

  // Add the type binding constraint.
  addConstraint(ConstraintKind::Bind, boundType, refType, locator);

  // Note that we have resolved this overload.
  resolvedOverloadSets
    = new (*this) ResolvedOverloadSetListItem{resolvedOverloadSets,
                                              boundType,
                                              choice,
                                              locator,
                                              openedFullType,
                                              refType};
  if (TC.getLangOpts().DebugConstraintSolver) {
    auto &log = getASTContext().TypeCheckerDebug->getStream();
    log.indent(solverState ? solverState->depth * 2 : 2)
      << "(overload set choice binding "
      << boundType->getString() << " := "
      << refType->getString() << ")\n";
  }
}

/// Given that we're accessing a member of an ImplicitlyUnwrappedOptional<T>, is
/// the DC one of the special cases where we should not instead look at T?
static bool isPrivilegedAccessToImplicitlyUnwrappedOptional(DeclContext *DC,
                                                  NominalTypeDecl *D) {
  assert(D == DC->getASTContext().getImplicitlyUnwrappedOptionalDecl());

  // Walk up through the chain of current contexts.
  for (; ; DC = DC->getParent()) {
    assert(DC && "ran out of contexts before finding a module scope?");

    // Look through local contexts.
    if (DC->isLocalContext()) {
      continue;

    // If we're in a type context that's defining or extending
    // ImplicitlyUnwrappedOptional<T>, we're privileged.
    } else if (DC->isTypeContext()) {
      if (DC->getAsNominalTypeOrNominalTypeExtensionContext() == D)
        return true;

    // Otherwise, we're privileged if we're within the same file that
    // defines ImplicitlyUnwrappedOptional<T>.
    } else {
      assert(DC->isModuleScopeContext());
      return (DC == D->getModuleScopeContext());
    }
  }
}

Type ConstraintSystem::lookThroughImplicitlyUnwrappedOptionalType(Type type) {
  if (auto boundTy = type->getAs<BoundGenericEnumType>()) {
    auto boundDecl = boundTy->getDecl();
    if (boundDecl == TC.Context.getImplicitlyUnwrappedOptionalDecl() &&
        !isPrivilegedAccessToImplicitlyUnwrappedOptional(DC, boundDecl))
      return boundTy->getGenericArgs()[0];
  }
  return Type();
}

Type ConstraintSystem::simplifyType(Type type) {
  return type.transform([&](Type type) -> Type {
    if (auto tvt = dyn_cast<TypeVariableType>(type.getPointer())) {
      tvt = getRepresentative(tvt);
      if (auto fixed = getFixedType(tvt)) {
        return simplifyType(fixed);
      }
      
      return tvt;
    }

    // If this is a dependent member type for which we end up simplifying
    // the base to a non-type-variable, perform lookup.
    if (auto depMemTy = type->getAs<DependentMemberType>()) {
      // Simplify the base.
      Type newBase = simplifyType(depMemTy->getBase());
      if (!newBase) return type;

      // If nothing changed, we're done.
      if (newBase.getPointer() == depMemTy->getBase().getPointer())
        return type;

      Type lookupBaseType = newBase;

      // Look through an inout type.
      if (auto inout = lookupBaseType->getAs<InOutType>())
        lookupBaseType = inout->getObjectType();

      // Look through a metatype.
      if (auto metatype = lookupBaseType->getAs<AnyMetatypeType>())
        lookupBaseType = metatype->getInstanceType();

      // If the new base is still something we can't handle, just build a
      // new dependent member type.
      if (lookupBaseType->is<TypeVariableType>() ||
          lookupBaseType->is<UnresolvedType>()) {
        if (auto assocType = depMemTy->getAssocType())
          return DependentMemberType::get(newBase, assocType);
        else
          return DependentMemberType::get(newBase, assocType);
      }

      // Dependent member types should only be created for associated types.
      auto assocType = depMemTy->getAssocType();
      assert(depMemTy->getAssocType());

      auto result = lookupBaseType->getTypeOfMember(
                      DC->getParentModule(), assocType, &TC,
                      assocType->getDeclaredInterfaceType());

      // FIXME: Record failure somehow?
      if (!result) return type;

      return result;
    }

    // If this is a FunctionType and we inferred new function attributes, apply
    // them.
    if (auto ft = dyn_cast<FunctionType>(type.getPointer())) {
      auto it = extraFunctionAttrs.find(ft);
      if (it != extraFunctionAttrs.end()) {
        auto extInfo = ft->getExtInfo();
        if (it->second.isNoEscape())
          extInfo = extInfo.withNoEscape();
        if (it->second.throws())
          extInfo = extInfo.withThrows();
        return FunctionType::get(ft->getInput(), ft->getResult(), extInfo);
      }
    }
    

    return type;
  });
}

Type Solution::simplifyType(TypeChecker &tc, Type type) const {
  // FIXME: Nearly identical to ConstraintSystem::simplifyType().
  return type.transform([&](Type type) -> Type {
    if (auto tvt = dyn_cast<TypeVariableType>(type.getPointer())) {
      auto known = typeBindings.find(tvt);
      assert(known != typeBindings.end());
      return known->second;
    }

    // If this is a dependent member type for which we end up simplifying
    // the base to a non-type-variable, perform lookup.
    if (auto depMemTy = type->getAs<DependentMemberType>()) {
      // Simplify the base.
      Type newBase = simplifyType(tc, depMemTy->getBase());
      if (!newBase) return type;

      // If nothing changed, we're done.
      if (newBase.getPointer() == depMemTy->getBase().getPointer())
        return type;

      Type lookupBaseType = newBase;

      // Look through an inout type.
      if (auto inout = lookupBaseType->getAs<InOutType>())
        lookupBaseType = inout->getObjectType();

      // Look through a metatype.
      if (auto metatype = lookupBaseType->getAs<AnyMetatypeType>())
        lookupBaseType = metatype->getInstanceType();

      // Dependent member types should only be created for associated types.
      auto assocType = depMemTy->getAssocType();
      assert(depMemTy->getAssocType());

      return lookupBaseType->getTypeOfMember(
               getConstraintSystem().DC->getParentModule(), assocType, &tc,
               assocType->getDeclaredInterfaceType());
    }

    // If this is a FunctionType and we inferred new function attributes, apply
    // them.
    if (auto ft = dyn_cast<FunctionType>(type.getPointer())) {
      auto &CS = getConstraintSystem();
      auto it = CS.extraFunctionAttrs.find(ft);
      if (it != CS.extraFunctionAttrs.end()) {
        auto extInfo = ft->getExtInfo();
        if (it->second.isNoEscape())
          extInfo = extInfo.withNoEscape();
        if (it->second.throws())
          extInfo = extInfo.withThrows();
        return FunctionType::get(simplifyType(tc, ft->getInput()),
                                 simplifyType(tc, ft->getResult()),
                                 extInfo);
      }
    }

    return type;
  });
}
