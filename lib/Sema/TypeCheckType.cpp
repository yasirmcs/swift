//===--- TypeCheckType.cpp - Type Validation ------------------------------===//
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
// This file implements validation for Swift types, emitting semantic errors as
// appropriate and checking default initializer values.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "GenericTypeResolver.h"
#include "MiscDiagnostics.h"

#include "swift/Strings.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

#define DEBUG_TYPE "TypeCheckType"

GenericTypeResolver::~GenericTypeResolver() { }

Type TypeChecker::getArraySliceType(SourceLoc loc, Type elementType) {
  if (!Context.getArrayDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 0);
    return Type();
  }

  return ArraySliceType::get(elementType);
}

Type TypeChecker::getDictionaryType(SourceLoc loc, Type keyType, 
                                    Type valueType) {
  if (!Context.getDictionaryDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 3);
    return Type();
  }

    return DictionaryType::get(keyType, valueType);
}

Type TypeChecker::getOptionalType(SourceLoc loc, Type elementType) {
  if (!Context.getOptionalDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 1);
    return Type();
  }

  return OptionalType::get(elementType);
}

Type TypeChecker::getImplicitlyUnwrappedOptionalType(SourceLoc loc, Type elementType) {
  if (!Context.getImplicitlyUnwrappedOptionalDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 2);
    return Type();
  }

  return ImplicitlyUnwrappedOptionalType::get(elementType);
}

static Type getStdlibType(TypeChecker &TC, Type &cached, DeclContext *dc,
                          StringRef name) {
  if (cached.isNull()) {
    Module *stdlib = TC.Context.getStdlibModule();
    LookupTypeResult lookup = TC.lookupMemberType(dc, ModuleType::get(stdlib),
                                                  TC.Context.getIdentifier(
                                                    name));
    if (lookup)
      cached = lookup.back().second;
  }
  return cached;
}

Type TypeChecker::getStringType(DeclContext *dc) {
  return ::getStdlibType(*this, StringType, dc, "String");
}
Type TypeChecker::getInt8Type(DeclContext *dc) {
  return ::getStdlibType(*this, Int8Type, dc, "Int8");
}
Type TypeChecker::getUInt8Type(DeclContext *dc) {
  return ::getStdlibType(*this, UInt8Type, dc, "UInt8");
}

/// Find the standard type of exceptions.
///
/// We call this the "exception type" to try to avoid confusion with
/// the AST's ErrorType node.
Type TypeChecker::getExceptionType(DeclContext *dc, SourceLoc loc) {
  if (NominalTypeDecl *decl = Context.getErrorDecl())
    return decl->getDeclaredType();

  // Not really sugar, but the actual diagnostic text is fine.
  diagnose(loc, diag::sugar_type_not_found, 4);
  return Type();
}

static Type getObjectiveCNominalType(TypeChecker &TC,
                                     Type &cache,
                                     Identifier ModuleName,
                                     Identifier TypeName,
                                     DeclContext *dc) {
  if (cache)
    return cache;

  auto &Context = TC.Context;

  // FIXME: Does not respect visibility of the module.
  Module *module = Context.getLoadedModule(ModuleName);
  if (!module)
    return nullptr;

  NameLookupOptions lookupOptions
    = defaultMemberLookupOptions |
      NameLookupFlags::KnownPrivate |
      NameLookupFlags::OnlyTypes;
  if (auto result = TC.lookupMember(dc, ModuleType::get(module), TypeName,
                                    lookupOptions)) {
    for (auto decl : result) {
      if (auto nominal = dyn_cast<NominalTypeDecl>(decl.Decl)) {
        cache = nominal->getDeclaredType();
        return cache;
      }
    }
  }

  return nullptr;
}

Type TypeChecker::getNSObjectType(DeclContext *dc) {
  return getObjectiveCNominalType(*this, NSObjectType, Context.Id_ObjectiveC,
                                Context.getSwiftId(
                                  KnownFoundationEntity::NSObject),
                                dc);
}

Type TypeChecker::getNSErrorType(DeclContext *dc) {
  return getObjectiveCNominalType(*this, NSObjectType, Context.Id_Foundation,
                                  Context.getSwiftId(
                                    KnownFoundationEntity::NSError),
                                  dc);
}

Type TypeChecker::getObjCSelectorType(DeclContext *dc) {
  return getObjectiveCNominalType(*this, ObjCSelectorType,
                                  Context.Id_ObjectiveC,
                                  Context.Id_Selector,
                                  dc);
}

Type
TypeChecker::getDynamicBridgedThroughObjCClass(DeclContext *dc,
                                               Type dynamicType,
                                               Type valueType) {
  // We can only bridge from class or Objective-C existential types.
  if (!dynamicType->isObjCExistentialType() &&
      !dynamicType->getClassOrBoundGenericClass())
    return Type();

  // If the value type cannot be bridged, we're done.
  if (!valueType->isPotentiallyBridgedValueType())
    return Type();

  return Context.getBridgedToObjC(dc, valueType);
}

void TypeChecker::forceExternalDeclMembers(NominalTypeDecl *nominalDecl) {
  // Force any delayed members added to the nominal type declaration.
  if (nominalDecl->hasDelayedMembers()) {
    this->handleExternalDecl(nominalDecl);
    nominalDecl->setHasDelayedMembers(false);
  }
}

Type TypeChecker::resolveTypeInContext(
       TypeDecl *typeDecl,
       DeclContext *fromDC,
       TypeResolutionOptions options,
       bool isSpecialized,
       GenericTypeResolver *resolver,
       UnsatisfiedDependency *unsatisfiedDependency) {
  PartialGenericTypeToArchetypeResolver defaultResolver;
  if (!resolver)
    resolver = &defaultResolver;

  // If we have a callback to report dependencies, do so.
  if (unsatisfiedDependency &&
      (*unsatisfiedDependency)(requestResolveTypeDecl(typeDecl)))
    return Type();

  // If we found a generic parameter, map to the archetype if there is one.
  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(typeDecl)) {
    return resolver->resolveGenericTypeParamType(
             genericParam->getDeclaredType()->castTo<GenericTypeParamType>());
  }

  auto nominalType = dyn_cast<NominalTypeDecl>(typeDecl);
  if (nominalType && (!nominalType->getGenericParams() || !isSpecialized)) {
    forceExternalDeclMembers(nominalType);
  } else {
    nominalType = nullptr;
  }

  // Walk up through the type scopes to find the context containing the type
  // being resolved.
  auto ownerDC = typeDecl->getDeclContext();
  bool nonTypeOwner = !ownerDC->isTypeContext();
  auto ownerNominal = ownerDC->getAsNominalTypeOrNominalTypeExtensionContext();

  // We might have an invalid extension that didn't resolve.
  if (ownerNominal == nullptr && ownerDC->isExtensionContext()) {
    assert(cast<ExtensionDecl>(ownerDC)->isInvalid());
    return ErrorType::get(Context);
  }

  auto assocType = dyn_cast<AssociatedTypeDecl>(typeDecl);
  assert((ownerNominal || nonTypeOwner) &&
         "Owner must be a nominal type or a non type context");

  // If true, we could not resolve some types, so we did not visit all
  // relevant contexts.
  bool incomplete = false;

  for (auto parentDC = fromDC; !parentDC->isModuleContext();
       parentDC = parentDC->getParent()) {

    // If we are referring to a type within its own context, and we have either
    // a generic type with no generic arguments or a non-generic type, use the
    // type within the context.
    if (nominalType) {
      if (parentDC->getAsNominalTypeOrNominalTypeExtensionContext() == nominalType)
        return resolver->resolveTypeOfContext(parentDC);
      if (!parentDC->isModuleScopeContext() && !isa<TopLevelCodeDecl>(parentDC))
        continue;

      // If we didn't find a matching declaration, the iteration is restarted
      // but we won't look anymore for the specific nominal type declaration
      parentDC = fromDC;
      nominalType = nullptr;
    }

    if (nonTypeOwner) {
      // If this is a typealias not in type context, we still need the
      // interface type; the typealias might be in a function context, and
      // its underlying type might reference outer generic parameters.
      if (isa<TypeAliasDecl>(typeDecl))
        return resolver->resolveTypeOfDecl(typeDecl);

      // When a nominal type used outside its context, return the unbound
      // generic form of the type.
      assert(isa<NominalTypeDecl>(typeDecl) || isa<ModuleDecl>(typeDecl));
      return typeDecl->getDeclaredType();
    }

    // For the next steps we need our parentDC to be a type context
    if (!parentDC->isTypeContext())
      continue;

    // Search the type of this context and its supertypes (if its a
    // class) or refined protocols (if its a protocol).
    llvm::SmallPtrSet<const NominalTypeDecl *, 8> visited;
    llvm::SmallVector<Type, 8> stack;

    // Start with the type of the current context.
    auto fromType = resolver->resolveTypeOfContext(parentDC);
    if (!fromType || fromType->hasError())
      incomplete = true;
    else
      stack.push_back(fromType);

    // If we are in a protocol extension there might be other type aliases and
    // nominal types brought into the context through requirements on Self,
    // for example:
    //
    // extension MyProtocol where Self : YourProtocol { ... }
    if (parentDC->getAsProtocolExtensionContext()) {
      auto ED = cast<ExtensionDecl>(parentDC);
      if (auto genericSig = ED->getGenericSignature()) {
        for (auto req : genericSig->getRequirements()) {
          if (req.getKind() == RequirementKind::Conformance ||
              req.getKind() == RequirementKind::Superclass) {
            if (req.getFirstType()->isEqual(ED->getSelfInterfaceType()))
              stack.push_back(req.getSecondType());
          }
        }
      }
    }

    while (!stack.empty()) {
      auto fromType = stack.back();
      auto *fromProto = parentDC->getAsProtocolOrProtocolExtensionContext();

      stack.pop_back();

      // If we hit circularity, we will diagnose at some point in typeCheckDecl().
      // However we have to explicitly guard against that here because we get
      // called as part of validateDecl().
      if (!visited.insert(fromType->getAnyNominal()).second)
        continue;

      // Handle this case:
      // - Current context: concrete type
      // - Nested type: associated type
      // - Nested type's context: protocol or protocol extension
      //
      if (!fromProto &&
          ownerNominal->getAsProtocolOrProtocolExtensionContext()) {
        Optional<ProtocolConformanceRef> conformance;

        // If the conformance check failed, the associated type is for a
        // conformance of an outer context.
        if (!options.contains(TR_InheritanceClause) &&
            (conformance = conformsToProtocol(
                             fromType,
                             cast<ProtocolDecl>(ownerNominal),
                             parentDC, ConformanceCheckFlags::Used)) &&
            conformance->isConcrete()) {
          if (assocType) {
            return conformance->getConcrete()->getTypeWitness(assocType, this)
                     .getReplacement();
          }

          return substMemberTypeWithBase(parentDC->getParentModule(), typeDecl,
                                         fromType);
        }
      }

      // Handle these cases:
      // - Current context: concrete type
      // - Nested type: concrete type or type alias
      // - Nested type's context: concrete type
      //
      // - Current context: protocol or protocol extension
      // - Nested type: type alias
      // - Nested type's context: protocol or protocol extension
      //
      // Note: this is not supported yet, FIXME:
      // - Current context: concrete type
      // - Nested type: type alias
      // - Nested type's context: protocol or protocol extension
      //
      if (fromType->getAnyNominal() == ownerNominal) {
        if (fromProto &&
            ownerNominal->getAsProtocolOrProtocolExtensionContext()) {
          // If we are looking up an associated type or a protocol's type alias
          // from a protocol or protocol extension, use the archetype for 'Self'
          // instead of the existential type.
          assert(fromType->is<ProtocolType>());

          auto selfType = parentDC->getSelfInterfaceType();
          if (!selfType)
            return ErrorType::get(Context);
          fromType = resolver->resolveGenericTypeParamType(
              selfType->castTo<GenericTypeParamType>());

          if (assocType) {
            // Odd special case, ask Doug to explain it over pizza one day
            if (fromType->isTypeParameter())
              return resolver->resolveSelfAssociatedType(
                  fromType, parentDC, assocType);
          }
        }

        return substMemberTypeWithBase(parentDC->getParentModule(), typeDecl,
                                       fromType);
      }

      if (auto superclassTy = getSuperClassOf(fromType))
        stack.push_back(superclassTy);
      else if (auto protoTy = fromType->getAs<ProtocolType>()) {
        for (auto *proto : protoTy->getDecl()->getInheritedProtocols(this))
          if (auto refinedTy = proto->getDeclaredTypeInContext())
            stack.push_back(refinedTy);
      }
    }
  }

  assert(incomplete && "Should have found type by now");
  return ErrorType::get(Context);
}

/// This function checks if a bound generic type is UnsafePointer<Void> or
/// UnsafeMutablePointer<Void>. For these two type representations, we should
/// warn users that they are deprecated and replace them with more handy
/// UnsafeRawPointer and UnsafeMutableRawPointer, respectively.
static bool isPointerToVoid(ASTContext &Ctx, Type Ty, bool &IsMutable) {
  if (Ty.isNull())
    return false;
  auto *BGT = Ty->getAs<BoundGenericType>();
  if (!BGT)
    return false;
  if (BGT->getDecl() != Ctx.getUnsafePointerDecl() &&
      BGT->getDecl() != Ctx.getUnsafeMutablePointerDecl())
    return false;
  IsMutable = BGT->getDecl() == Ctx.getUnsafeMutablePointerDecl();
  assert(BGT->getGenericArgs().size() == 1);
  return BGT->getGenericArgs().front()->isVoid();
}

Type TypeChecker::applyGenericArguments(Type type, TypeDecl *decl,
                                        SourceLoc loc, DeclContext *dc,
                                        GenericIdentTypeRepr *generic,
                                        TypeResolutionOptions options,
                                        GenericTypeResolver *resolver) {

  if (type->hasError()) {
    generic->setInvalid();
    return type;
  }

  // We must either have an unbound generic type, or a generic type alias.
  if (!type->is<UnboundGenericType>() &&
      !(isa<TypeAliasDecl>(decl) &&
        cast<TypeAliasDecl>(decl)->getGenericParams())) {

    auto diag = diagnose(loc, diag::not_a_generic_type, type);

    // Don't add fixit on module type; that isn't the right type regardless
    // of whether it had generic arguments.
    if (!type->is<ModuleType>())
      diag.fixItRemove(generic->getAngleBrackets());

    generic->setInvalid();
    return type;
  }

  // If we have a non-generic type alias, we have an unbound generic type.
  // Grab the decl from the unbound generic type.
  //
  // The idea is if you write:
  //
  // typealias Foo = Bar.Baz
  //
  // Then 'Foo<Int>' applies arguments to Bar.Baz, whereas if you write:
  //
  // typealias Foo<T> = Bar.Baz<T>
  //
  // Then 'Foo<Int>' applies arguments to Foo itself.
  //
  if (isa<TypeAliasDecl>(decl) &&
      !cast<TypeAliasDecl>(decl)->getGenericParams()) {
    decl = type->castTo<UnboundGenericType>()->getDecl();
  }

  // Make sure we have the right number of generic arguments.
  // FIXME: If we have fewer arguments than we need, that might be okay, if
  // we're allowed to deduce the remaining arguments from context.
  auto genericDecl = cast<GenericTypeDecl>(decl);
  auto genericArgs = generic->getGenericArgs();
  auto genericParams = genericDecl->getGenericParams();
  if (genericParams->size() != genericArgs.size()) {
    diagnose(loc, diag::type_parameter_count_mismatch, decl->getName(),
             genericParams->size(), genericArgs.size(),
             genericArgs.size() < genericParams->size())
        .highlight(generic->getAngleBrackets());
    diagnose(decl, diag::generic_type_declared_here,
             decl->getName());
    return nullptr;
  }

  // In SIL mode, Optional<T> interprets T as a SIL type.
  if (options.contains(TR_SILType)) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      if (nominal->classifyAsOptionalType()) {
        // Validate the generic argument.
        TypeLoc arg = genericArgs[0];
        if (validateType(arg, dc, withoutContext(options, true), resolver))
          return nullptr;

        Type objectType = arg.getType();
        return BoundGenericType::get(nominal, /*parent*/ Type(), objectType);
      }
    }  
  }

  SmallVector<TypeLoc, 8> args;
  for (auto tyR : genericArgs)
    args.push_back(tyR);

  bool isGenericSignature = options.contains(TR_GenericSignature);
  auto result = applyUnboundGenericArguments(type, genericDecl, loc, dc, args,
                                             isGenericSignature, resolver);
  bool isMutablePointer;
  if (isPointerToVoid(dc->getASTContext(), result, isMutablePointer)) {
    if (isMutablePointer)
      diagnose(loc, diag::use_of_void_pointer, "Mutable").
        fixItReplace(generic->getSourceRange(), "UnsafeMutableRawPointer");
    else
      diagnose(loc, diag::use_of_void_pointer, "").
        fixItReplace(generic->getSourceRange(), "UnsafeRawPointer");
  }
  return result;
}

/// Apply generic arguments to the given type.
Type TypeChecker::applyUnboundGenericArguments(
    Type type, GenericTypeDecl *decl, SourceLoc loc, DeclContext *dc,
    MutableArrayRef<TypeLoc> genericArgs, bool isGenericSignature,
    GenericTypeResolver *resolver) {

  assert(genericArgs.size() == decl->getGenericParams()->size() &&
         "invalid arguments, use applyGenericArguments for diagnostic emitting");

  // Make sure we always have a resolver to use.
  PartialGenericTypeToArchetypeResolver defaultResolver;
  if (!resolver)
    resolver = &defaultResolver;

  TypeResolutionOptions options;
  if (isGenericSignature)
    options |= TR_GenericSignature;

  // Validate the generic arguments and capture just the types.
  SmallVector<Type, 4> genericArgTypes;
  for (auto &genericArg : genericArgs) {
    // Validate the generic argument.
    if (validateType(genericArg, dc, options, resolver))
      return nullptr;

    genericArgTypes.push_back(genericArg.getType());
  }

  // If we're completing a generic TypeAlias, then we map the types provided
  // onto the underlying type.
  if (auto *TAD = dyn_cast<TypeAliasDecl>(decl)) {
    auto signature = TAD->getGenericSignature();

    TypeSubstitutionMap subs;
    for (unsigned i = 0, e = genericArgs.size(); i < e; i++) {
      auto t = signature->getInnermostGenericParams()[i];
      subs[t->getCanonicalType()->castTo<GenericTypeParamType>()] =
        genericArgs[i].getType();
    }

    if (auto outerSig = TAD->getDeclContext()->getGenericSignatureOfContext()) {
      for (auto outerParam : outerSig->getGenericParams()) {
        subs[outerParam->getCanonicalType()->castTo<GenericTypeParamType>()] =
            resolver->resolveTypeOfDecl(outerParam->getDecl());
      }
    }

    // FIXME: Change callers to pass the right type in for generic typealiases
    if (type->is<UnboundGenericType>() || isa<NameAliasType>(type.getPointer())) {
      type = ArchetypeBuilder::mapTypeOutOfContext(TAD, TAD->getUnderlyingType());
    }

    type = type.subst(dc->getParentModule(), subs, SubstFlags::UseErrorType);

    // FIXME: return a SubstitutedType to preserve the fact that
    // we resolved a generic TypeAlias, for availability diagnostics.
    // A better fix might be to introduce a BoundGenericAliasType
    // which desugars as appropriate.
    return SubstitutedType::get(TAD->getDeclaredType(), type, Context);
  }
  
  // Form the bound generic type.
  auto *UGT = type->castTo<UnboundGenericType>();
  auto *BGT = BoundGenericType::get(cast<NominalTypeDecl>(decl),
                                    UGT->getParent(), genericArgTypes);

  // Check protocol conformance.
  if (!BGT->hasTypeParameter() && !BGT->hasTypeVariable()) {
    SourceLoc noteLoc = decl->getLoc();
    if (noteLoc.isInvalid())
      noteLoc = loc;

    // FIXME: Record that we're checking substitutions, so we can't end up
    // with infinite recursion.

    // Check the generic arguments against the generic signature.
    auto genericSig = decl->getGenericSignature();
    if (!decl->hasType() || decl->isValidatingGenericSignature()) {
      diagnose(loc, diag::recursive_requirement_reference);
      return nullptr;
    }

    // Collect the complete set of generic arguments.
    assert(genericSig != nullptr);
    auto substitutions = BGT->getMemberSubstitutions(BGT->getDecl());

    if (checkGenericArguments(dc, loc, noteLoc, UGT, genericSig,
                              substitutions))
      return nullptr;

    useObjectiveCBridgeableConformancesOfArgs(dc, BGT);
  }

  return BGT;
}

static Type applyGenericTypeReprArgs(TypeChecker &TC, Type type,
                                     TypeDecl *decl,
                                     SourceLoc loc,
                                     DeclContext *dc,
                                     GenericIdentTypeRepr *generic,
                                     TypeResolutionOptions options,
                                     GenericTypeResolver *resolver) {

  Type ty = TC.applyGenericArguments(type, decl, loc, dc, generic,
                                     options, resolver);
  if (!ty)
    return ErrorType::get(TC.Context);
  return ty;
}

/// \brief Diagnose a use of an unbound generic type.
static void diagnoseUnboundGenericType(TypeChecker &tc, Type ty,SourceLoc loc) {
  auto unbound = ty->castTo<UnboundGenericType>();
  {
    InFlightDiagnostic diag = tc.diagnose(loc,
        diag::generic_type_requires_arguments, ty);
    if (auto *genericD = unbound->getDecl()) {
      SmallString<64> genericArgsToAdd;
      if (tc.getDefaultGenericArgumentsString(genericArgsToAdd, genericD))
        diag.fixItInsertAfter(loc, genericArgsToAdd);
    }
  }
  tc.diagnose(unbound->getDecl()->getLoc(), diag::generic_type_declared_here,
              unbound->getDecl()->getName());
}

/// \brief Returns a valid type or ErrorType in case of an error.
static Type resolveTypeDecl(TypeChecker &TC, TypeDecl *typeDecl, SourceLoc loc,
                            DeclContext *dc,
                            GenericIdentTypeRepr *generic,
                            TypeResolutionOptions options,
                            GenericTypeResolver *resolver,
                            UnsatisfiedDependency *unsatisfiedDependency) {
  assert(dc && "No declaration context for type resolution?");

  // If we have a callback to report dependencies, do so.
  if (unsatisfiedDependency) {
    if ((*unsatisfiedDependency)(requestResolveTypeDecl(typeDecl)))
      return nullptr;
  } else {
    // Validate the declaration.
    TC.validateDecl(typeDecl);

    // FIXME: More principled handling of circularity.
    if (!isa<AssociatedTypeDecl>(typeDecl) && !typeDecl->hasType())
      return ErrorType::get(TC.Context);
  }

  // Resolve the type declaration to a specific type. How this occurs
  // depends on the current context and where the type was found.
  Type type =
      TC.resolveTypeInContext(typeDecl, dc, options, generic, resolver);

  // FIXME: Defensive check that shouldn't be needed, but prevents a
  // huge number of crashes on ill-formed code.
  if (!type)
    return ErrorType::get(TC.Context);

  if (type->is<UnboundGenericType>() && !generic &&
      !options.contains(TR_AllowUnboundGenerics) &&
      !options.contains(TR_ResolveStructure)) {
    diagnoseUnboundGenericType(TC, type, loc);
    return ErrorType::get(TC.Context);
  }

  // If we found a generic parameter, try to resolve it.
  // FIXME: Jump through hoops to maintain syntactic sugar. We shouldn't care
  // about this, because we shouldn't have to do this at all.
  if (auto genericParam = type->getAs<GenericTypeParamType>()) {
    auto resolvedGP = resolver->resolveGenericTypeParamType(genericParam);
    if (auto substituted = dyn_cast<SubstitutedType>(type.getPointer())) {
      type = SubstitutedType::get(substituted->getOriginal(), resolvedGP,
                                  TC.Context);
    } else {
      type = resolvedGP;
    }
  }

  if (generic && !options.contains(TR_ResolveStructure)) {
    // Apply the generic arguments to the type.
    type = applyGenericTypeReprArgs(TC, type, typeDecl, loc, dc, generic,
                                    options, resolver);
  }

  assert(type);
  return type;
}

/// Retrieve the nearest enclosing nominal type context.
static NominalTypeDecl *getEnclosingNominalContext(DeclContext *dc) {
  while (dc->isLocalContext())
    dc = dc->getParent();

  if (auto nominal = dc->getAsNominalTypeOrNominalTypeExtensionContext())
    return nominal;

  return nullptr;
}

/// Diagnose a reference to an unknown type.
///
/// This routine diagnoses a reference to an unknown type, and
/// attempts to fix the reference via various means.
///
/// \param tc The type checker through which we should emit the diagnostic.
/// \param dc The context in which name lookup occurred.
///
/// \returns either the corrected type, if possible, or an error type to
/// that correction failed.
static Type diagnoseUnknownType(TypeChecker &tc, DeclContext *dc,
                                Type parentType,
                                SourceRange parentRange,
                                ComponentIdentTypeRepr *comp,
                                TypeResolutionOptions options,
                                NameLookupOptions lookupOptions,
                                GenericTypeResolver *resolver,
                                UnsatisfiedDependency *unsatisfiedDependency) {
  // Unqualified lookup case.
  if (parentType.isNull()) {
    // Attempt to refer to 'Self' within a non-protocol nominal
    // type. Fix this by replacing 'Self' with the nominal type name.
    NominalTypeDecl *nominal = nullptr;
    if (comp->getIdentifier() == tc.Context.Id_Self &&
        !isa<GenericIdentTypeRepr>(comp) &&
        (nominal = getEnclosingNominalContext(dc))) {
      // Retrieve the nominal type and resolve it within this context.
      assert(!isa<ProtocolDecl>(nominal) && "Cannot be a protocol");
      auto type = resolveTypeDecl(tc, nominal, comp->getIdLoc(), dc, nullptr,
                                  options, resolver, unsatisfiedDependency);
      if (type->hasError())
        return type;

      // Produce a Fix-It replacing 'Self' with the nominal type name.
      tc.diagnose(comp->getIdLoc(), diag::self_in_nominal, nominal->getName())
        .fixItReplace(comp->getIdLoc(), nominal->getName().str());
      comp->overwriteIdentifier(nominal->getName());
      comp->setValue(nominal);
      return type;
    }


    // Try ignoring access control.
    DeclContext *lookupDC = dc;
    if (options.contains(TR_GenericSignature))
      lookupDC = dc->getParent();

    NameLookupOptions relookupOptions = lookupOptions;
    relookupOptions |= NameLookupFlags::KnownPrivate;
    relookupOptions |= NameLookupFlags::IgnoreAccessibility;
    LookupResult inaccessibleResults =
        tc.lookupUnqualified(lookupDC, comp->getIdentifier(), comp->getIdLoc(),
                             relookupOptions);
    if (inaccessibleResults) {
      // FIXME: What if the unviable candidates have different levels of access?
      auto first = cast<TypeDecl>(inaccessibleResults.front().Decl);
      tc.diagnose(comp->getIdLoc(), diag::candidate_inaccessible,
                  comp->getIdentifier(), first->getFormalAccess());

      // FIXME: If any of the candidates (usually just one) are in the same
      // module we could offer a fix-it.
      for (auto lookupResult : inaccessibleResults)
        tc.diagnose(lookupResult.Decl, diag::type_declared_here);

      // Don't try to recover here; we'll get more access-related diagnostics
      // downstream if we do.
      return ErrorType::get(tc.Context);
    }

    // Fallback.
    SourceLoc L = comp->getIdLoc();
    SourceRange R = SourceRange(comp->getIdLoc());

    // Check if the unknown type is in the type remappings.
    auto &Remapped = tc.Context.RemappedTypes;
    auto TypeName = comp->getIdentifier().str();
    auto I = Remapped.find(TypeName);
    if (I != Remapped.end()) {
      auto RemappedTy = I->second->getString();
      tc.diagnose(L, diag::use_undeclared_type_did_you_mean,
                  comp->getIdentifier(), RemappedTy)
        .highlight(R)
        .fixItReplace(R, RemappedTy);

      // Replace the computed type with the suggested type.
      comp->overwriteIdentifier(tc.Context.getIdentifier(RemappedTy));

      // HACK: 'NSUInteger' suggests both 'UInt' and 'Int'.
      if (TypeName
            == tc.Context.getSwiftName(KnownFoundationEntity::NSUInteger)) {
        tc.diagnose(L, diag::note_remapped_type, "UInt")
          .fixItReplace(R, "UInt");
      }

      return I->second;
    }

    tc.diagnose(L, diag::use_undeclared_type,
                comp->getIdentifier())
      .highlight(R);

    return ErrorType::get(tc.Context);
  }

  // Qualified lookup case.

  // Try ignoring access control.
  NameLookupOptions relookupOptions = lookupOptions;
  relookupOptions |= NameLookupFlags::KnownPrivate;
  relookupOptions |= NameLookupFlags::IgnoreAccessibility;
  auto inaccessibleMembers = tc.lookupMemberType(dc, parentType,
                                                 comp->getIdentifier(),
                                                 relookupOptions);
  if (inaccessibleMembers) {
    // FIXME: What if the unviable candidates have different levels of access?
    const TypeDecl *first = inaccessibleMembers.front().first;
    tc.diagnose(comp->getIdLoc(), diag::candidate_inaccessible,
                comp->getIdentifier(), first->getFormalAccess());

    // FIXME: If any of the candidates (usually just one) are in the same module
    // we could offer a fix-it.
    for (auto lookupResult : inaccessibleMembers)
      tc.diagnose(lookupResult.first, diag::type_declared_here);

    // Don't try to recover here; we'll get more access-related diagnostics
    // downstream if we do.
    return ErrorType::get(tc.Context);
  }

  // FIXME: Typo correction!

  // Lookup into a type.
  if (auto moduleType = parentType->getAs<ModuleType>()) {
    tc.diagnose(comp->getIdLoc(), diag::no_module_type,
                comp->getIdentifier(), moduleType->getModule()->getName());
  } else {
    // Situation where class tries to inherit from itself, such
    // would produce an assertion when trying to lookup members of the class.
    auto lazyResolver = tc.Context.getLazyResolver();
    if (auto superClass = parentType->getSuperclass(lazyResolver)) {
      if (superClass->isEqual(parentType)) {
        auto decl = parentType->castTo<NominalType>()->getDecl();
        tc.diagnose(decl->getLoc(), diag::circular_class_inheritance,
                    decl->getNameStr());
        return ErrorType::get(tc.Context);
      }
    }

    LookupResult memberLookup;
    // Let's try to lookup given identifier as a member of the parent type,
    // this allows for more precise diagnostic, which distinguishes between
    // identifier not found as a member type vs. not found at all.
    NameLookupOptions memberLookupOptions = lookupOptions;
    memberLookupOptions |= NameLookupFlags::IgnoreAccessibility;
    memberLookupOptions |= NameLookupFlags::KnownPrivate;
    memberLookupOptions -= NameLookupFlags::OnlyTypes;

    memberLookup = tc.lookupMember(dc, parentType, comp->getIdentifier(),
                                   memberLookupOptions);

    // Looks like this is not a member type, but simply a member of parent type.
    if (!memberLookup.empty()) {
      auto &member = memberLookup[0];
      tc.diagnose(comp->getIdLoc(), diag::invalid_member_reference,
                  member->getDescriptiveKind(), comp->getIdentifier(),
                  parentType)
          .highlight(parentRange);
    } else {
      tc.diagnose(comp->getIdLoc(), diag::invalid_member_type,
                  comp->getIdentifier(), parentType)
          .highlight(parentRange);
    }
  }
  return ErrorType::get(tc.Context);
}

/// Resolve the given identifier type representation as an unqualified type,
/// returning the type it references.
///
/// \returns Either the resolved type or a null type, the latter of
/// which indicates that some dependencies were unsatisfied.
static Type
resolveTopLevelIdentTypeComponent(TypeChecker &TC, DeclContext *DC,
                                  ComponentIdentTypeRepr *comp,
                                  TypeResolutionOptions options,
                                  bool diagnoseErrors,
                                  GenericTypeResolver *resolver,
                                  UnsatisfiedDependency *unsatisfiedDependency){
  // Short-circuiting.
  if (comp->isInvalid()) return ErrorType::get(TC.Context);

  // If the component has already been bound to a declaration, handle
  // that now.
  if (ValueDecl *VD = comp->getBoundDecl()) {
    auto *typeDecl = cast<TypeDecl>(VD);

    // Resolve the type declaration within this context.
    return resolveTypeDecl(TC, typeDecl, comp->getIdLoc(), DC,
                           dyn_cast<GenericIdentTypeRepr>(comp), options,
                           resolver, unsatisfiedDependency);
  }

  // Resolve the first component, which is the only one that requires
  // unqualified name lookup.
  DeclContext *lookupDC = DC;

  // Dynamic 'Self' in the result type of a function body.
  if (options.contains(TR_DynamicSelfResult) &&
      comp->getIdentifier() == TC.Context.Id_Self) {
    auto func = cast<FuncDecl>(DC);
    assert(func->hasDynamicSelf() && "Not marked as having dynamic Self?");

    // FIXME: The passed-in TypeRepr should get 'typechecked' as well.
    // The issue is though that ComponentIdentTypeRepr only accepts a ValueDecl
    // while the 'Self' type is more than just a reference to a TypeDecl.

    return func->getDynamicSelf();
  }

  // For lookups within the generic signature, look at the generic
  // parameters (only), then move up to the enclosing context.
  if (options.contains(TR_GenericSignature)) {
    GenericParamList *genericParams;
    if (auto *generic = dyn_cast<GenericTypeDecl>(DC)) {
      genericParams = generic->getGenericParams();
    } else if (auto *ext = dyn_cast<ExtensionDecl>(DC)) {
      genericParams = ext->getGenericParams();
    } else {
      genericParams = cast<AbstractFunctionDecl>(DC)->getGenericParams();
    }

    if (genericParams) {
      auto matchingParam =
          std::find_if(genericParams->begin(), genericParams->end(),
                       [comp](const GenericTypeParamDecl *param) {
        return param->getFullName().matchesRef(comp->getIdentifier());
      });

      if (matchingParam != genericParams->end()) {
        comp->setValue(*matchingParam);
        return resolveTopLevelIdentTypeComponent(TC, DC, comp, options,
                                                 diagnoseErrors, resolver,
                                                 unsatisfiedDependency);
      }
    }

    // If the lookup occurs from within a trailing 'where' clause of
    // a constrained extension, also look for associated types.
    if (genericParams && genericParams->hasTrailingWhereClause() &&
        isa<ExtensionDecl>(DC) && comp->getIdLoc().isValid() &&
        TC.Context.SourceMgr.rangeContainsTokenLoc(
          genericParams->getTrailingWhereClauseSourceRange(),
          comp->getIdLoc())) {
      // We need to be able to perform qualified lookup into the given
      // declaration context.
      if (unsatisfiedDependency &&
          (*unsatisfiedDependency)(
            requestQualifiedLookupInDeclContext({ DC, comp->getIdentifier(),
                                                  comp->getIdLoc() })))
        return nullptr;

      auto nominal = DC->getAsNominalTypeOrNominalTypeExtensionContext();
      SmallVector<ValueDecl *, 4> decls;
      if (DC->lookupQualified(nominal->getDeclaredInterfaceType(),
                              comp->getIdentifier(),
                              NL_QualifiedDefault|NL_ProtocolMembers,
                              &TC,
                              decls)) {
        for (const auto decl : decls) {
          // FIXME: Better ambiguity handling.
          if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
            comp->setValue(assocType);
            return resolveTopLevelIdentTypeComponent(TC, DC, comp, options,
                                                     diagnoseErrors, resolver,
                                                     unsatisfiedDependency);
          }
        }
      }
    }

    if (!DC->isCascadingContextForLookup(/*excludeFunctions*/false))
      options |= TR_KnownNonCascadingDependency;

    // The remaining lookups will be in the parent context.
    lookupDC = DC->getParent();
  }

  // We need to be able to perform unqualified lookup into the given
  // declaration context.
  if (unsatisfiedDependency &&
      (*unsatisfiedDependency)(
        requestUnqualifiedLookupInDeclContext({ lookupDC, 
                                                comp->getIdentifier(),
                                                comp->getIdLoc() })))
    return nullptr;

  NameLookupOptions lookupOptions = defaultUnqualifiedLookupOptions;
  lookupOptions |= NameLookupFlags::OnlyTypes;
  if (options.contains(TR_KnownNonCascadingDependency))
    lookupOptions |= NameLookupFlags::KnownPrivate;
  // FIXME: Eliminate this once we can handle finding protocol members
  // in resolveTypeInContext.
  lookupOptions -= NameLookupFlags::ProtocolMembers;
  LookupResult globals = TC.lookupUnqualified(lookupDC, comp->getIdentifier(),
                                              comp->getIdLoc(), lookupOptions);

  // Process the names we found.
  Type current;
  TypeDecl *currentDecl = nullptr;
  bool isAmbiguous = false;
  for (const auto &result : globals) {
    auto typeDecl = cast<TypeDecl>(result.Decl);

    // If necessary, add delayed members to the declaration.
    if (auto nomDecl = dyn_cast<NominalTypeDecl>(typeDecl)) {
      TC.forceExternalDeclMembers(nomDecl);
    }

    Type type = resolveTypeDecl(TC, typeDecl, comp->getIdLoc(), DC,
                                dyn_cast<GenericIdentTypeRepr>(comp), options,
                                resolver, unsatisfiedDependency);

    if (!type || type->hasError())
      return type;

    // If this is the first result we found, record it.
    if (current.isNull()) {
      current = type;
      currentDecl = typeDecl;
      continue;
    }

    // Otherwise, check for an ambiguity.
    if (!current->isEqual(type)) {
      isAmbiguous = true;
      break;
    }

    // We have a found multiple type aliases that refer to the same thing.
    // Ignore the duplicate.
  }

  // Complain about any ambiguities we detected.
  // FIXME: We could recover by looking at later components.
  if (isAmbiguous) {
    if (diagnoseErrors) {
      TC.diagnose(comp->getIdLoc(), diag::ambiguous_type_base,
                  comp->getIdentifier())
        .highlight(comp->getIdLoc());
      for (auto result : globals) {
        TC.diagnose(result.Decl, diag::found_candidate);
      }
    }

    comp->setInvalid();
    return ErrorType::get(TC.Context);
  }

  // If we found nothing, complain and give ourselves a chance to recover.
  if (current.isNull()) {
    // If we're not allowed to complain or we couldn't fix the
    // source, bail out.
    if (!diagnoseErrors)
      return ErrorType::get(TC.Context);

    return diagnoseUnknownType(TC, DC, nullptr, SourceRange(), comp, options,
                               lookupOptions, resolver, unsatisfiedDependency);
  }

  comp->setValue(currentDecl);
  return current;
}

/// Resolve the given identifier type representation as a qualified
/// lookup within the given parent type, returning the type it
/// references.
static Type resolveNestedIdentTypeComponent(
              TypeChecker &TC, DeclContext *DC,
              Type parentTy,
              SourceRange parentRange,
              ComponentIdentTypeRepr *comp,
              TypeResolutionOptions options,
              bool diagnoseErrors,
              GenericTypeResolver *resolver,
              UnsatisfiedDependency *unsatisfiedDependency) {
  // Short-circuiting.
  if (comp->isInvalid()) return ErrorType::get(TC.Context);

  // Phase 2: If a declaration has already been bound, use it.
  if (ValueDecl *decl = comp->getBoundDecl()) {
    auto *typeDecl = cast<TypeDecl>(decl);

    Type memberType;

    if (parentTy->isTypeParameter()) {
      // If the parent is a type parameter, the member is a dependent member.

      // FIXME: We either have an associated type here or a member of
      // some member of the superclass bound (or its superclasses),
      // which should allow us to skip much of the work in
      // resolveDependentMemberType.

      // Try to resolve the dependent member type to a specific associated
      // type.
      memberType = resolver->resolveDependentMemberType(parentTy, DC,
                                                        parentRange, comp);
      assert(memberType && "Received null dependent member type");
      return memberType;
    }

    if (isa<AssociatedTypeDecl>(typeDecl) &&
        !parentTy->is<ArchetypeType>()) {
      assert(!parentTy->isExistentialType());

      auto assocType = cast<AssociatedTypeDecl>(typeDecl);

      // Find the conformance and dig out the type witness.
      ConformanceCheckOptions conformanceOptions;
      if (options.contains(TR_InExpression))
        conformanceOptions |= ConformanceCheckFlags::InExpression;

      auto *protocol = cast<ProtocolDecl>(assocType->getDeclContext());
      auto conformance = TC.conformsToProtocol(parentTy, protocol, DC,
                                               conformanceOptions);
      if (!conformance || conformance->isAbstract()) {
        return nullptr;
      }

      // FIXME: Establish that we need a type witness.
      return conformance->getConcrete()->getTypeWitness(assocType, &TC)
               .getReplacement();
    }

    // Otherwise, simply substitute the parent type into the member.
    memberType = TC.substMemberTypeWithBase(DC->getParentModule(), typeDecl,
                                            parentTy);

    // Propagate failure.
    if (!memberType || memberType->hasError()) return memberType;

    // If there are generic arguments, apply them now.
    if (auto genComp = dyn_cast<GenericIdentTypeRepr>(comp)) {
      memberType = applyGenericTypeReprArgs(
          TC, memberType, typeDecl, comp->getIdLoc(), DC, genComp,
          options, resolver);

      // Propagate failure.
      if (!memberType || memberType->hasError()) return memberType;
    }

    // We're done.
    return memberType;
  }

  // Phase 1: Find and bind the component decl.

  // If the parent is a dependent type, the member is a dependent member.
  if (parentTy->isTypeParameter()) {
    // Try to resolve the dependent member type to a specific associated
    // type.
    Type memberType = resolver->resolveDependentMemberType(parentTy, DC,
                                                           parentRange,
                                                           comp);
    assert(memberType && "Received null dependent member type");
    // If we know what type declaration we're referencing, store it.
    if (auto typeDecl = memberType->getDirectlyReferencedTypeDecl()) {
      comp->setValue(typeDecl);
    }

    return memberType;
  }

  // Look for member types with the given name.
  bool isKnownNonCascading = options.contains(TR_KnownNonCascadingDependency);
  if (!isKnownNonCascading && options.contains(TR_InExpression)) {
    // Expressions cannot affect a function's signature.
    isKnownNonCascading = isa<AbstractFunctionDecl>(DC);
  }

  // We need to be able to perform qualified lookup into the given type.
  if (unsatisfiedDependency) {
    DeclContext *dc;
    if (auto parentNominal = parentTy->getAnyNominal())
      dc = parentNominal;
    else if (auto parentModule = parentTy->getAs<ModuleType>())
      dc = parentModule->getModule();
    else
      dc = nullptr;

    if (dc &&
        (*unsatisfiedDependency)(
          requestQualifiedLookupInDeclContext({ dc, comp->getIdentifier(),
                                                comp->getIdLoc() })))
      return nullptr;
  }

  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  if (isKnownNonCascading)
    lookupOptions |= NameLookupFlags::KnownPrivate;
  // FIXME: Lift the restriction for TR_InheritanceClause
  if (options.contains(TR_ExtensionBinding) ||
      options.contains(TR_InheritanceClause))
    lookupOptions -= NameLookupFlags::ProtocolMembers;
  auto memberTypes = TC.lookupMemberType(DC, parentTy, comp->getIdentifier(),
                                         lookupOptions);

  // Name lookup was ambiguous. Complain.
  // FIXME: Could try to apply generic arguments first, and see whether
  // that resolves things. But do we really want that to succeed?
  if (memberTypes.size() > 1) {
    if (diagnoseErrors)
      TC.diagnoseAmbiguousMemberType(parentTy, parentRange,
                                     comp->getIdentifier(), comp->getIdLoc(),
                                     memberTypes);
    return ErrorType::get(TC.Context);
  }

  // If we didn't find anything, complain.
  bool recovered = false;
  Type memberType;
  TypeDecl *member = nullptr;
  if (!memberTypes) {
    // If we're not allowed to complain or we couldn't fix the
    // source, bail out.
    if (!diagnoseErrors) {
      return ErrorType::get(TC.Context);
    }

    Type ty = diagnoseUnknownType(TC, DC, parentTy, parentRange, comp, options,
                                  lookupOptions, resolver,
                                  unsatisfiedDependency);
    if (!ty || ty->hasError()) {
      return ErrorType::get(TC.Context);
    }

    recovered = true;
    memberType = ty;
    member = cast_or_null<TypeDecl>(comp->getBoundDecl());
  } else {
    memberType = memberTypes.back().second;
    member = memberTypes.back().first;
  }

  if (parentTy->isExistentialType() && isa<AssociatedTypeDecl>(member)) {
    if (diagnoseErrors)
      TC.diagnose(comp->getIdLoc(), diag::assoc_type_outside_of_protocol,
                  comp->getIdentifier());

    return ErrorType::get(TC.Context);
  }

  if (parentTy->isExistentialType() && isa<TypeAliasDecl>(member) &&
      memberType->hasTypeParameter()) {
    if (diagnoseErrors)
      TC.diagnose(comp->getIdLoc(), diag::typealias_outside_of_protocol,
                  comp->getIdentifier());

    return ErrorType::get(TC.Context);
  }

  // If there are generic arguments, apply them now.
  if (auto genComp = dyn_cast<GenericIdentTypeRepr>(comp))
    memberType = applyGenericTypeReprArgs(
        TC, memberType, member, comp->getIdLoc(), DC, genComp,
        options, resolver);

  // If we found a reference to an associated type or other member type that
  // was marked invalid, just return ErrorType to silence downstream errors.
  if (member && member->isInvalid())
    memberType = ErrorType::get(TC.Context);

  if (member)
    comp->setValue(member);
  return memberType;
}

static Type resolveIdentTypeComponent(
              TypeChecker &TC, DeclContext *DC,
              ArrayRef<ComponentIdentTypeRepr *> components,
              TypeResolutionOptions options,
              bool diagnoseErrors,
              GenericTypeResolver *resolver,
              UnsatisfiedDependency *unsatisfiedDependency) {
  auto comp = components.back();

  // The first component uses unqualified lookup.
  auto parentComps = components.slice(0, components.size()-1);
  if (parentComps.empty()) {
    return resolveTopLevelIdentTypeComponent(TC, DC, comp, options,
                                             diagnoseErrors, resolver,
                                             unsatisfiedDependency);
  }

  // All remaining components use qualified lookup.

  // Resolve the parent type.
  Type parentTy = resolveIdentTypeComponent(TC, DC, parentComps, options,
                                            diagnoseErrors, resolver,
                                            unsatisfiedDependency);
  if (!parentTy || parentTy->hasError()) return parentTy;
  
  SourceRange parentRange(parentComps.front()->getIdLoc(),
                          parentComps.back()->getSourceRange().End);

  // Resolve the nested type.
  return resolveNestedIdentTypeComponent(TC, DC, parentTy,
                                         parentRange, comp,
                                         options, diagnoseErrors,
                                         resolver,
                                         unsatisfiedDependency);
}

static bool checkTypeDeclAvailability(const TypeDecl *TypeDecl,
                                      IdentTypeRepr *IdType,
                                      SourceLoc Loc, DeclContext *DC,
                                      TypeChecker &TC,
                                      bool AllowPotentiallyUnavailableProtocol){
  if (isa<ComponentIdentTypeRepr>(IdType)) {
    return diagnoseDeclAvailability(TypeDecl, TC, DC, Loc,
                                    AllowPotentiallyUnavailableProtocol,
                                    false);
  }

  return false;
}


static bool diagnoseAvailability(Type ty, IdentTypeRepr *IdType, SourceLoc Loc,
                                 DeclContext *DC, TypeChecker &TC,
                                 bool AllowPotentiallyUnavailableProtocol) {
  if (auto *NAT = dyn_cast<NameAliasType>(ty.getPointer())) {
    if (checkTypeDeclAvailability(NAT->getDecl(), IdType, Loc, DC, TC,
                                  AllowPotentiallyUnavailableProtocol))
      return true;
  }

  if (auto *GPT = dyn_cast<GenericTypeParamType>(ty.getPointer())) {
    if (auto GP = GPT->getDecl()) {
      if (checkTypeDeclAvailability(GP, IdType, Loc, DC, TC,
                                    AllowPotentiallyUnavailableProtocol)) {
        return true;
      }
    }
  }

  // Look through substituted types to diagnose when the original
  // type is marked unavailable.
  if (auto *ST = dyn_cast<SubstitutedType>(ty.getPointer())) {
    if (diagnoseAvailability(ST->getOriginal(), IdType, Loc, DC, TC,
                             AllowPotentiallyUnavailableProtocol)) {
      return true;
    }
  }

  CanType canTy = ty.getCanonicalTypeOrNull();
  if (canTy.isNull())
    return false;
  if (auto NTD = canTy.getAnyNominal())
    return checkTypeDeclAvailability(NTD, IdType, Loc, DC, TC,
                                     AllowPotentiallyUnavailableProtocol);

  return false;
}

// Hack to apply context-specific @escaping to an AST function type.
static Type applyNonEscapingFromContext(DeclContext *DC,
                                        Type ty,
                                        TypeResolutionOptions options) {
  // Remember whether this is a function parameter.
  bool isFunctionParam =
    options.contains(TR_FunctionInput) ||
    options.contains(TR_ImmediateFunctionInput);

  bool defaultNoEscape = isFunctionParam;

  // Desugar here
  auto *funcTy = ty->castTo<FunctionType>();
  auto extInfo = funcTy->getExtInfo();
  if (defaultNoEscape && !extInfo.isNoEscape()) {
    extInfo = extInfo.withNoEscape();

    // We lost the sugar to flip the isNoEscape bit.
    //
    // FIXME: It would be better to add a new AttributedType sugared type,
    // which would wrap the NameAliasType or ParenType, and apply the
    // isNoEscape bit when de-sugaring.
    // <https://bugs.swift.org/browse/SR-2520>
    return FunctionType::get(funcTy->getInput(), funcTy->getResult(), extInfo);
  }

  // Note: original sugared type
  return ty;
}

/// \brief Returns a valid type or ErrorType in case of an error.
Type TypeChecker::resolveIdentifierType(
       DeclContext *DC,
       IdentTypeRepr *IdType,
       TypeResolutionOptions options,
       bool diagnoseErrors,
       GenericTypeResolver *resolver,
       UnsatisfiedDependency *unsatisfiedDependency) {
  assert(resolver && "Missing generic type resolver");

  auto ComponentRange = IdType->getComponentRange();
  auto Components = llvm::makeArrayRef(ComponentRange.begin(),
                                       ComponentRange.end());
  Type result = resolveIdentTypeComponent(*this, DC, Components, options, 
                                          diagnoseErrors, resolver,
                                          unsatisfiedDependency);
  if (!result) return nullptr;

  if (auto moduleTy = result->getAs<ModuleType>()) {
    if (diagnoseErrors) {
      auto moduleName = moduleTy->getModule()->getName();
      diagnose(Components.back()->getIdLoc(),
               diag::use_undeclared_type, moduleName);
      diagnose(Components.back()->getIdLoc(),
               diag::note_module_as_type, moduleName);
    }
    Components.back()->setInvalid();
    return ErrorType::get(Context);
  }

  // Hack to apply context-specific @escaping to a typealias with an underlying
  // function type.
  if (result->is<FunctionType>())
    result = applyNonEscapingFromContext(DC, result, options);

  // We allow a type to conform to a protocol that is less available than
  // the type itself. This enables a type to retroactively model or directly
  // conform to a protocol only available on newer OSes and yet still be used on
  // older OSes.
  // To support this, inside inheritance clauses we allow references to
  // protocols that are unavailable in the current type refinement context.
  bool AllowPotentiallyUnavailableProtocol =
      options.contains(TR_InheritanceClause);

  // Check the availability of the type.
  if (!(options & TR_AllowUnavailable) &&
      diagnoseAvailability(result, IdType,
                           Components.back()->getIdLoc(), DC, *this,
                           AllowPotentiallyUnavailableProtocol)) {
    Components.back()->setInvalid();
    return ErrorType::get(Context);
  }
  
  return result;
}

/// Returns true if any illegal IUOs were found. If inference of IUO type is
/// disabled, IUOs may only be specified in the following positions:
///  * outermost type
///  * function param
///  * function return type
static bool checkForIllegalIUOs(TypeChecker &TC, TypeRepr *Repr,
                                TypeResolutionOptions Options) {
  class IllegalIUOWalker : public ASTWalker {
    TypeChecker &TC;
    SmallVector<bool, 4> IUOsAllowed;
    bool FoundIllegalIUO = false;

  public:
    IllegalIUOWalker(TypeChecker &TC, bool IsGenericParameter)
      : TC(TC)
      , IUOsAllowed{!IsGenericParameter} {}

    bool walkToTypeReprPre(TypeRepr *T) {
      bool iuoAllowedHere = IUOsAllowed.back();

      // Raise a diagnostic if we run into a prohibited IUO.
      if (!iuoAllowedHere) {
        if (auto *iuoTypeRepr =
            dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(T)) {
          TC.diagnose(iuoTypeRepr->getStartLoc(), diag::iuo_in_illegal_position)
            .fixItReplace(iuoTypeRepr->getExclamationLoc(), "?");
          FoundIllegalIUO = true;
        }
      }

      bool childIUOsAllowed = false;
      if (iuoAllowedHere) {
        if (auto *tupleTypeRepr = dyn_cast<TupleTypeRepr>(T)) {
          if (tupleTypeRepr->isParenType()) {
            childIUOsAllowed = true;
          }
        } else if (isa<FunctionTypeRepr>(T)) {
          childIUOsAllowed = true;
        } else if (isa<AttributedTypeRepr>(T) || isa<InOutTypeRepr>(T)) {
          childIUOsAllowed = true;
        }
      }
      IUOsAllowed.push_back(childIUOsAllowed);
      return true;
    }

    bool walkToTypeReprPost(TypeRepr *T) {
      IUOsAllowed.pop_back();
      return true;
    }

    bool getFoundIllegalIUO() const { return FoundIllegalIUO; }
  };

  IllegalIUOWalker Walker(TC, Options.contains(TR_GenericSignature));
  Repr->walk(Walker);
  return Walker.getFoundIllegalIUO();
}

bool TypeChecker::validateType(TypeLoc &Loc, DeclContext *DC,
                               TypeResolutionOptions options,
                               GenericTypeResolver *resolver,
                               UnsatisfiedDependency *unsatisfiedDependency) {
  // FIXME: Verify that these aren't circular and infinite size.
  
  // If we've already validated this type, don't do so again.
  if (Loc.wasValidated())
    return Loc.isError();

  SWIFT_FUNC_STAT;

  if (Loc.getType().isNull()) {
    // Raise error if we parse an IUO type in an illegal position.
    checkForIllegalIUOs(*this, Loc.getTypeRepr(), options);

    // Special case: in computed property setter, newValue closure is escaping
    if (isa<FuncDecl>(DC) && cast<FuncDecl>(DC)->isSetter())
      options |= TR_ImmediateSetterNewValue;

    auto type = resolveType(Loc.getTypeRepr(), DC, options, resolver,
                            unsatisfiedDependency);
    if (!type) {
      // If a dependency went unsatisfied, just return false.
      if (unsatisfiedDependency) return false;

      type = ErrorType::get(Context);

      // Diagnose types that are illegal in SIL.
    } else if (options.contains(TR_SILType) && !type->isLegalSILType()) {
      diagnose(Loc.getLoc(), diag::illegal_sil_type, type);
      Loc.setType(ErrorType::get(Context), true);
      return true;
    }

    // Special case: in computed property setter, newValue closure is escaping
    if (auto funcDecl = dyn_cast<FuncDecl>(DC))
      if (funcDecl->isSetter())
        if (auto funTy = type->getAs<AnyFunctionType>())
          type = funTy->withExtInfo(funTy->getExtInfo().withNoEscape(false));

    Loc.setType(type, true);
    return Loc.isError();
  }

  Loc.setType(Loc.getType(), true);
  return Loc.isError();
}

namespace {
  const auto DefaultParameterConvention = ParameterConvention::Direct_Unowned;
  const auto DefaultResultConvention = ResultConvention::Unowned;

  class TypeResolver {
    TypeChecker &TC;
    ASTContext &Context;
    DeclContext *DC;
    GenericTypeResolver *Resolver;
    swift::UnsatisfiedDependency *UnsatisfiedDependency;
  public:
    TypeResolver(TypeChecker &tc, DeclContext *DC,
                 GenericTypeResolver *resolver,
                 swift::UnsatisfiedDependency *unsatisfiedDependency)
      : TC(tc), Context(tc.Context), DC(DC), Resolver(resolver),
        UnsatisfiedDependency(unsatisfiedDependency)
    {
      assert(resolver);
    }

    Type resolveType(TypeRepr *repr, TypeResolutionOptions options);

  private:

    Type resolveAttributedType(AttributedTypeRepr *repr,
                               TypeResolutionOptions options);
    Type resolveAttributedType(TypeAttributes &attrs, TypeRepr *repr,
                               TypeResolutionOptions options);
    Type resolveASTFunctionType(FunctionTypeRepr *repr,
                                TypeResolutionOptions options,
                                FunctionType::ExtInfo extInfo
                                  = FunctionType::ExtInfo());
    Type resolveSILFunctionType(FunctionTypeRepr *repr,
                                TypeResolutionOptions options,
                                SILFunctionType::ExtInfo extInfo
                                  = SILFunctionType::ExtInfo(),
                                ParameterConvention calleeConvention
                                  = DefaultParameterConvention);
    SILParameterInfo resolveSILParameter(TypeRepr *repr,
                                         TypeResolutionOptions options);
    bool resolveSILResults(TypeRepr *repr, TypeResolutionOptions options,
                           SmallVectorImpl<SILResultInfo> &results,
                           Optional<SILResultInfo> &errorResult);
    bool resolveSingleSILResult(TypeRepr *repr, TypeResolutionOptions options,
                                SmallVectorImpl<SILResultInfo> &results,
                                Optional<SILResultInfo> &errorResult);
    Type resolveInOutType(InOutTypeRepr *repr,
                          TypeResolutionOptions options);
    Type resolveArrayType(ArrayTypeRepr *repr,
                          TypeResolutionOptions options);
    Type resolveDictionaryType(DictionaryTypeRepr *repr,
                               TypeResolutionOptions options);
    Type resolveOptionalType(OptionalTypeRepr *repr,
                             TypeResolutionOptions options);
    Type resolveImplicitlyUnwrappedOptionalType(ImplicitlyUnwrappedOptionalTypeRepr *repr,
                                      TypeResolutionOptions options);
    Type resolveTupleType(TupleTypeRepr *repr,
                          TypeResolutionOptions options);
    Type resolveCompositionType(CompositionTypeRepr *repr,
                                TypeResolutionOptions options);
    Type resolveMetatypeType(MetatypeTypeRepr *repr,
                             TypeResolutionOptions options);
    Type resolveProtocolType(ProtocolTypeRepr *repr,
                             TypeResolutionOptions options);

    Type buildMetatypeType(MetatypeTypeRepr *repr,
                           Type instanceType,
                           Optional<MetatypeRepresentation> storedRepr);
    Type buildProtocolType(ProtocolTypeRepr *repr,
                           Type instanceType,
                           Optional<MetatypeRepresentation> storedRepr);
  };
}

Type TypeChecker::resolveType(TypeRepr *TyR, DeclContext *DC,
                              TypeResolutionOptions options,
                              GenericTypeResolver *resolver,
                              UnsatisfiedDependency *unsatisfiedDependency) {
  PrettyStackTraceTypeRepr stackTrace(Context, "resolving", TyR);

  // Make sure we always have a resolver to use.
  PartialGenericTypeToArchetypeResolver defaultResolver;
  if (!resolver)
    resolver = &defaultResolver;

  TypeResolver typeResolver(*this, DC, resolver, unsatisfiedDependency);
  auto result = typeResolver.resolveType(TyR, options);
  
  // If we resolved down to an error, make sure to mark the typeRepr as invalid
  // so we don't produce a redundant diagnostic.
  if (result && result->hasError())
    TyR->setInvalid();
  return result;
}

Type TypeResolver::resolveType(TypeRepr *repr, TypeResolutionOptions options) {
  assert(repr && "Cannot validate null TypeReprs!");

  // If we know the type representation is invalid, just return an
  // error type.
  if (repr->isInvalid()) return ErrorType::get(TC.Context);

  // Strip the "is function input" bits unless this is a type that knows about
  // them.
  if (!isa<InOutTypeRepr>(repr) &&
      !isa<TupleTypeRepr>(repr) &&
      !isa<AttributedTypeRepr>(repr) &&
      !isa<FunctionTypeRepr>(repr) &&
      !isa<IdentTypeRepr>(repr)) {
    options -= TR_ImmediateFunctionInput;
    options -= TR_FunctionInput;
  }

  bool isImmediateSetterNewValue = options.contains(TR_ImmediateSetterNewValue);
  options -= TR_ImmediateSetterNewValue;

  if (Context.LangOpts.DisableAvailabilityChecking)
    options |= TR_AllowUnavailable;

  switch (repr->getKind()) {
  case TypeReprKind::Error:
    return ErrorType::get(Context);

  case TypeReprKind::Attributed:
    return resolveAttributedType(cast<AttributedTypeRepr>(repr), options);
  case TypeReprKind::InOut:
    return resolveInOutType(cast<InOutTypeRepr>(repr), options);

  case TypeReprKind::SimpleIdent:
  case TypeReprKind::GenericIdent:
  case TypeReprKind::CompoundIdent:
    return TC.resolveIdentifierType(DC, cast<IdentTypeRepr>(repr), options,
                                    /*diagnoseErrors*/ true, Resolver,
                                    UnsatisfiedDependency);

  case TypeReprKind::Function:
    if (!(options & TR_SILType)) {
      // Default non-escaping for closure parameters
      auto result =
          resolveASTFunctionType(cast<FunctionTypeRepr>(repr), options);
      if (result && result->is<FunctionType>() && !isImmediateSetterNewValue)
        return applyNonEscapingFromContext(DC, result, options);
      return result;
    }
    return resolveSILFunctionType(cast<FunctionTypeRepr>(repr), options);

  case TypeReprKind::Array:
    return resolveArrayType(cast<ArrayTypeRepr>(repr), options);

  case TypeReprKind::Dictionary:
    return resolveDictionaryType(cast<DictionaryTypeRepr>(repr), options);

  case TypeReprKind::Optional:
    return resolveOptionalType(cast<OptionalTypeRepr>(repr), options);

  case TypeReprKind::ImplicitlyUnwrappedOptional:
    return resolveImplicitlyUnwrappedOptionalType(
             cast<ImplicitlyUnwrappedOptionalTypeRepr>(repr),
             options);

  case TypeReprKind::Tuple:
    return resolveTupleType(cast<TupleTypeRepr>(repr), options);

  case TypeReprKind::Composition:
    return resolveCompositionType(cast<CompositionTypeRepr>(repr), options);

  case TypeReprKind::Metatype:
    return resolveMetatypeType(cast<MetatypeTypeRepr>(repr), options);

  case TypeReprKind::Protocol:
    return resolveProtocolType(cast<ProtocolTypeRepr>(repr), options);

  case TypeReprKind::Fixed:
    return cast<FixedTypeRepr>(repr)->getType();
  }
  llvm_unreachable("all cases should be handled");
}

static Type rebuildWithDynamicSelf(ASTContext &Context, Type ty) {
  OptionalTypeKind OTK;
  if (auto metatypeTy = ty->getAs<MetatypeType>()) {
    return MetatypeType::get(
        rebuildWithDynamicSelf(Context, metatypeTy->getInstanceType()),
        metatypeTy->getRepresentation());
  } else if (auto optionalTy = ty->getAnyOptionalObjectType(OTK)) {
    return OptionalType::get(
        OTK, rebuildWithDynamicSelf(Context, optionalTy));
  } else {
    return DynamicSelfType::get(ty, Context);
  }
}

Type TypeResolver::resolveAttributedType(AttributedTypeRepr *repr,
                                         TypeResolutionOptions options) {
  // Copy the attributes, since we're about to start hacking on them.
  TypeAttributes attrs = repr->getAttrs();
  assert(!attrs.empty());

  return resolveAttributedType(attrs, repr->getTypeRepr(), options);
}

Type TypeResolver::resolveAttributedType(TypeAttributes &attrs,
                                         TypeRepr *repr,
                                         TypeResolutionOptions options) {
  // Remember whether this is a function parameter.
  bool isFunctionParam =
    options.contains(TR_FunctionInput) ||
    options.contains(TR_ImmediateFunctionInput);
  bool isVariadicFunctionParam =
    options.contains(TR_VariadicFunctionInput);

  // The type we're working with, in case we want to build it differently
  // based on the attributes we see.
  Type ty;
  
  // In SIL *only*, allow @thin, @thick, or @objc_metatype to apply to
  // a metatype.
  if (attrs.has(TAK_thin) || attrs.has(TAK_thick) || 
      attrs.has(TAK_objc_metatype)) {
    if (auto SF = DC->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        TypeRepr *base;
        if (auto metatypeRepr = dyn_cast<MetatypeTypeRepr>(repr)) {
          base = metatypeRepr->getBase();
        } else if (auto protocolRepr = dyn_cast<ProtocolTypeRepr>(repr)) {
          base = protocolRepr->getBase();
        } else {
          base = nullptr;
        }

        if (base) {
          Optional<MetatypeRepresentation> storedRepr;
          // The instance type is not a SIL type.
          auto instanceOptions = options;
          instanceOptions -= TR_SILType;
          instanceOptions -= TR_ImmediateFunctionInput;
          instanceOptions -= TR_FunctionInput;

          auto instanceTy = resolveType(base, instanceOptions);
          if (!instanceTy || instanceTy->hasError())
            return instanceTy;

          // Check for @thin.
          if (attrs.has(TAK_thin)) {
            storedRepr = MetatypeRepresentation::Thin;
            attrs.clearAttribute(TAK_thin);
          }

          // Check for @thick.
          if (attrs.has(TAK_thick)) {
            if (storedRepr)
              TC.diagnose(repr->getStartLoc(), 
                          diag::sil_metatype_multiple_reprs);
              
            storedRepr = MetatypeRepresentation::Thick;
            attrs.clearAttribute(TAK_thick);
          }

          // Check for @objc_metatype.
          if (attrs.has(TAK_objc_metatype)) {
            if (storedRepr)
              TC.diagnose(repr->getStartLoc(), 
                          diag::sil_metatype_multiple_reprs);
              
            storedRepr = MetatypeRepresentation::ObjC;
            attrs.clearAttribute(TAK_objc_metatype);
          }

          if (instanceTy->hasError()) {
            ty = instanceTy;
          } else if (auto metatype = dyn_cast<MetatypeTypeRepr>(repr)) {
            ty = buildMetatypeType(metatype, instanceTy, storedRepr);
          } else {
            ty = buildProtocolType(cast<ProtocolTypeRepr>(repr),
                                   instanceTy, storedRepr);
          }
        }
      }
    }
  }

  // Pass down the variable function type attributes to the
  // function-type creator.
  static const TypeAttrKind FunctionAttrs[] = {
    TAK_convention, TAK_noreturn, TAK_pseudogeneric,
    TAK_callee_owned, TAK_callee_guaranteed, TAK_noescape, TAK_autoclosure,
    TAK_escaping
  };

  auto checkUnsupportedAttr = [&](TypeAttrKind attr) {
    if (attrs.has(attr)) {
      TC.diagnose(attrs.getLoc(attr), diag::attribute_not_supported);
      attrs.clearAttribute(attr);
    }
  };
  
  // Some function representation attributes are not supported at source level;
  // only SIL knows how to handle them.  Reject them unless this is a SIL input.
  if (!(options & TR_SILType)) {
    for (auto silOnlyAttr : {TAK_callee_owned, TAK_callee_guaranteed}) {
      checkUnsupportedAttr(silOnlyAttr);
    }
  }  

  // Other function representation attributes are not normally supported at
  // source level, but we want to support them there in SIL files.
  auto SF = DC->getParentSourceFile();
  if (!SF || SF->Kind != SourceFileKind::SIL) {
    for (auto silOnlyAttr : {TAK_thin, TAK_thick}) {
      checkUnsupportedAttr(silOnlyAttr);
    }
  }
  
  bool hasFunctionAttr = false;
  for (auto i : FunctionAttrs)
    if (attrs.has(i)) {
      hasFunctionAttr = true;
      break;
    }

  // Function attributes require a syntactic function type.
  FunctionTypeRepr *fnRepr = dyn_cast<FunctionTypeRepr>(repr);

  if (hasFunctionAttr && fnRepr && (options & TR_SILType)) {
    SILFunctionType::Representation rep;

    auto calleeConvention = ParameterConvention::Direct_Unowned;
    if (attrs.has(TAK_callee_owned)) {
      if (attrs.has(TAK_callee_guaranteed)) {
        TC.diagnose(attrs.getLoc(TAK_callee_owned),
                    diag::sil_function_repeat_convention, /*callee*/ 2);
      }
      calleeConvention = ParameterConvention::Direct_Owned;
    } else if (attrs.has(TAK_callee_guaranteed)) {
      calleeConvention = ParameterConvention::Direct_Guaranteed;
    }

    if (!attrs.hasConvention()) {
      rep = SILFunctionType::Representation::Thick;
    } else {
      // SIL exposes a greater number of conventions than Swift source.
      auto parsedRep =
      llvm::StringSwitch<Optional<SILFunctionType::Representation>>
      (attrs.getConvention())
      .Case("thick", SILFunctionType::Representation::Thick)
      .Case("block", SILFunctionType::Representation::Block)
      .Case("thin", SILFunctionType::Representation::Thin)
      .Case("c", SILFunctionType::Representation::CFunctionPointer)
      .Case("method", SILFunctionType::Representation::Method)
      .Case("objc_method", SILFunctionType::Representation::ObjCMethod)
      .Case("witness_method", SILFunctionType::Representation::WitnessMethod)
      .Default(None);
      if (!parsedRep) {
        TC.diagnose(attrs.getLoc(TAK_convention),
                    diag::unsupported_sil_convention, attrs.getConvention());
        rep = SILFunctionType::Representation::Thin;
      } else {
        rep = *parsedRep;
      }
    }

    // Resolve the function type directly with these attributes.
    SILFunctionType::ExtInfo extInfo(rep, attrs.has(TAK_pseudogeneric));

    ty = resolveSILFunctionType(fnRepr, options, extInfo, calleeConvention);
    if (!ty || ty->hasError()) return ty;
  } else if (hasFunctionAttr && fnRepr) {

    FunctionType::Representation rep = FunctionType::Representation::Swift;
    if (attrs.hasConvention()) {
      auto parsedRep =
        llvm::StringSwitch<Optional<FunctionType::Representation>>
          (attrs.getConvention())
          .Case("swift", FunctionType::Representation::Swift)
          .Case("block", FunctionType::Representation::Block)
          .Case("thin", FunctionType::Representation::Thin)
          .Case("c", FunctionType::Representation::CFunctionPointer)
          .Default(None);
      if (!parsedRep) {
        TC.diagnose(attrs.getLoc(TAK_convention),
                    diag::unsupported_convention, attrs.getConvention());
        rep = FunctionType::Representation::Swift;
      } else {
        rep = *parsedRep;
      }
    }

    // @autoclosure is only valid on parameters.
    if (!isFunctionParam && attrs.has(TAK_autoclosure)) {
      TC.diagnose(attrs.getLoc(TAK_autoclosure),
                  isVariadicFunctionParam
                      ? diag::attr_not_on_variadic_parameters
                      : diag::attr_only_on_parameters, "@autoclosure");
      attrs.clearAttribute(TAK_autoclosure);
    }

    // @noreturn has been replaced with a 'Never' return type.
    if (fnRepr && attrs.has(TAK_noreturn)) {
      auto &SM = TC.Context.SourceMgr;
      auto loc = attrs.getLoc(TAK_noreturn);
      auto attrRange = SourceRange(
        loc.getAdvancedLoc(-1),
        Lexer::getLocForEndOfToken(SM, loc));

      auto resultRange = fnRepr->getResultTypeRepr()->getSourceRange();

      TC.diagnose(loc, diag::noreturn_not_supported)
          .fixItRemove(attrRange)
          .fixItReplace(resultRange, "Never");
    }

    // Resolve the function type directly with these attributes.
    FunctionType::ExtInfo extInfo(rep,
                                  attrs.has(TAK_autoclosure),
                                  attrs.has(TAK_noescape),
                                  fnRepr->throws());

    ty = resolveASTFunctionType(fnRepr, options, extInfo);
    if (!ty || ty->hasError()) return ty;
  }

  auto instanceOptions = options;
  instanceOptions -= TR_ImmediateFunctionInput;
  instanceOptions -= TR_FunctionInput;

  // If we didn't build the type differently above, we might have
  // a typealias pointing at a function type with the @escaping
  // attribute. Resolve the type as if it were in non-parameter
  // context, and then set isNoEscape if @escaping is not present.
  if (!ty) ty = resolveType(repr, instanceOptions);
  if (!ty || ty->hasError()) return ty;

  // Handle @escaping
  if (hasFunctionAttr && ty->is<FunctionType>()) {
    if (attrs.has(TAK_escaping)) {
      // For compatibility with 3.0, we don't emit an error if it appears on a
      // variadic argument list.
      bool skipDiagnostic =
          isVariadicFunctionParam && Context.isSwiftVersion3();

      // The attribute is meaningless except on parameter types.
      bool shouldDiagnose = !isFunctionParam && !skipDiagnostic;
      if (shouldDiagnose) {
        auto &SM = TC.Context.SourceMgr;
        auto loc = attrs.getLoc(TAK_escaping);
        auto attrRange = SourceRange(
          loc.getAdvancedLoc(-1),
          Lexer::getLocForEndOfToken(SM, loc));

        TC.diagnose(loc, diag::escaping_non_function_parameter)
            .fixItRemove(attrRange);

        // Try to find a helpful note based on how the type is being used
        if (options.contains(TR_ImmediateOptionalTypeArgument)) {
          TC.diagnose(repr->getLoc(), diag::escaping_optional_type_argument);
        }
      }

      attrs.clearAttribute(TAK_escaping);
    } else {
      // No attribute; set the isNoEscape bit if we're in parameter context.
      ty = applyNonEscapingFromContext(DC, ty, options);
    }
  }

  if (hasFunctionAttr && !fnRepr) {
    // @autoclosure usually auto-implies @noescape, don't complain about both
    // of them.
    if (attrs.has(TAK_autoclosure))
      attrs.clearAttribute(TAK_noescape);

    for (auto i : FunctionAttrs) {
      if (attrs.has(i)) {
        TC.diagnose(attrs.getLoc(i), diag::attribute_requires_function_type,
                    TypeAttributes::getAttrName(i));
        attrs.clearAttribute(i);
      }
    }
  } else if (hasFunctionAttr && fnRepr) {
    // Remove the function attributes from the set so that we don't diagnose.
    for (auto i : FunctionAttrs)
      attrs.clearAttribute(i);
    attrs.convention = None;
  }

  // In SIL, handle @opened (n), which creates an existential archetype.
  if (attrs.has(TAK_opened)) {
    if (!ty->isExistentialType()) {
      TC.diagnose(attrs.getLoc(TAK_opened), diag::opened_non_protocol, ty);
    } else {
      ty = ArchetypeType::getOpened(ty, attrs.OpenedID);
    }
    attrs.clearAttribute(TAK_opened);
  }

  // In SIL files *only*, permit @weak and @unowned to apply directly to types.
  if (attrs.hasOwnership()) {
    if (auto SF = DC->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        if (((attrs.has(TAK_sil_weak) || attrs.has(TAK_sil_unmanaged)) &&
             ty->getAnyOptionalObjectType()) ||
            (!attrs.has(TAK_sil_weak) && ty->hasReferenceSemantics())) {
          ty = ReferenceStorageType::get(ty, attrs.getOwnership(), Context);
          attrs.clearOwnership();
        }
      }
    }
  }
  
  // In SIL *only*, allow @block_storage to specify a block storage type.
  if ((options & TR_SILType) && attrs.has(TAK_block_storage)) {
    ty = SILBlockStorageType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_block_storage);
  }
  
  // In SIL *only*, allow @box to specify a box type.
  if ((options & TR_SILType) && attrs.has(TAK_box)) {
    ty = SILBoxType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_box);
  }

  // In SIL *only*, allow @dynamic_self to specify a dynamic Self type.
  if ((options & TR_SILMode) && attrs.has(TAK_dynamic_self)) {
    ty = rebuildWithDynamicSelf(TC.Context, ty);
    attrs.clearAttribute(TAK_dynamic_self);
  }

  for (unsigned i = 0; i != TypeAttrKind::TAK_Count; ++i)
    if (attrs.has((TypeAttrKind)i))
      TC.diagnose(attrs.getLoc((TypeAttrKind)i),
                  diag::attribute_does_not_apply_to_type);

  return ty;
}

Type TypeResolver::resolveASTFunctionType(FunctionTypeRepr *repr,
                                          TypeResolutionOptions options,
                                          FunctionType::ExtInfo extInfo) {
  options -= TR_ImmediateFunctionInput;
  options -= TR_FunctionInput;

  Type inputTy = resolveType(repr->getArgsTypeRepr(),
                             options | TR_ImmediateFunctionInput);
  if (!inputTy || inputTy->hasError()) return inputTy;

  Type outputTy = resolveType(repr->getResultTypeRepr(), options);
  if (!outputTy || outputTy->hasError()) return outputTy;

  extInfo = extInfo.withThrows(repr->throws());

  ModuleDecl *M = DC->getParentModule();
  
  
  // If this is a function type without parens around the parameter list,
  // diagnose this and produce a fixit to add them.
  if (!isa<TupleTypeRepr>(repr->getArgsTypeRepr()) &&
      !repr->isWarnedAbout()) {
    auto args = repr->getArgsTypeRepr();
    TC.diagnose(args->getStartLoc(), diag::function_type_no_parens)
      .highlight(args->getSourceRange())
      .fixItInsert(args->getStartLoc(), "(")
      .fixItInsertAfter(args->getEndLoc(), ")");
    
    // Don't emit this warning three times when in generics.
    repr->setWarned();
  }

  // SIL uses polymorphic function types to resolve overloaded member functions.
  if (auto genericEnv = repr->getGenericEnvironment()) {
    auto *genericSig = repr->getGenericSignature();
    assert(genericSig != nullptr && "Did not call handleSILGenericParams()?");
    inputTy = ArchetypeBuilder::mapTypeOutOfContext(M, genericEnv, inputTy);
    outputTy = ArchetypeBuilder::mapTypeOutOfContext(M, genericEnv, outputTy);
    return GenericFunctionType::get(genericSig, inputTy, outputTy, extInfo);
  }

  auto fnTy = FunctionType::get(inputTy, outputTy, extInfo);
  // If the type is a block or C function pointer, it must be representable in
  // ObjC.
  switch (auto rep = extInfo.getRepresentation()) {
  case AnyFunctionType::Representation::Block:
  case AnyFunctionType::Representation::CFunctionPointer:
    if (!fnTy->isRepresentableIn(ForeignLanguage::ObjectiveC, DC)) {
      StringRef strName =
        rep == AnyFunctionType::Representation::Block ? "block" : "c";
      auto extInfo2 =
        extInfo.withRepresentation(AnyFunctionType::Representation::Swift);
      auto simpleFnTy = FunctionType::get(inputTy, outputTy, extInfo2);
      TC.diagnose(repr->getStartLoc(), diag::objc_convention_invalid,
                  simpleFnTy, strName);
    }
    break;

  case AnyFunctionType::Representation::Thin:
  case AnyFunctionType::Representation::Swift:
    break;
  }
  
  return fnTy;
}

Type TypeResolver::resolveSILFunctionType(FunctionTypeRepr *repr,
                                          TypeResolutionOptions options,
                                          SILFunctionType::ExtInfo extInfo,
                                          ParameterConvention callee) {
  options -= TR_ImmediateFunctionInput;
  options -= TR_FunctionInput;

  bool hasError = false;

  SmallVector<SILParameterInfo, 4> params;
  if (auto tuple = dyn_cast<TupleTypeRepr>(repr->getArgsTypeRepr())) {
    // SIL functions cannot be variadic.
    if (tuple->hasEllipsis()) {
      TC.diagnose(tuple->getEllipsisLoc(), diag::sil_function_ellipsis);
    }
    // SIL functions cannot have parmeter names.
    for (auto nameLoc : tuple->getUnderscoreLocs()) {
      if (nameLoc.isValid())
        TC.diagnose(nameLoc, diag::sil_function_input_label);
    }

    for (auto elt : tuple->getElements()) {
      auto param = resolveSILParameter(elt,options | TR_ImmediateFunctionInput);
      params.push_back(param);
      if (!param.getType()) return nullptr;

      if (param.getType()->hasError())
        hasError = true;
    }
  } else {
    SILParameterInfo param = resolveSILParameter(repr->getArgsTypeRepr(),
                                           options | TR_ImmediateFunctionInput);
    params.push_back(param);
    if (!param.getType()) return nullptr;

    if (param.getType()->hasError())
      hasError = true;
  }

  SmallVector<SILResultInfo, 4> results;
  Optional<SILResultInfo> errorResult;
  {
    // FIXME: Deal with unsatisfied dependencies.
    if (resolveSILResults(repr->getResultTypeRepr(), options,
                          results, errorResult)) {
      hasError = true;
    }
  }

  if (hasError) {
    return ErrorType::get(Context);
  }

  ModuleDecl *M = DC->getParentModule();

  // FIXME: Remap the parsed context types to interface types.
  CanGenericSignature genericSig;
  SmallVector<SILParameterInfo, 4> interfaceParams;
  SmallVector<SILResultInfo, 4> interfaceResults;
  Optional<SILResultInfo> interfaceErrorResult;
  if (auto *genericEnv = repr->getGenericEnvironment()) {
    genericSig = repr->getGenericSignature()->getCanonicalSignature();
 
    for (auto &param : params) {
      auto transParamType = ArchetypeBuilder::mapTypeOutOfContext(
          M, genericEnv, param.getType())->getCanonicalType();
      interfaceParams.push_back(param.getWithType(transParamType));
    }
    for (auto &result : results) {
      auto transResultType =
        ArchetypeBuilder::mapTypeOutOfContext(
          M, genericEnv, result.getType())->getCanonicalType();
      interfaceResults.push_back(result.getWithType(transResultType));
    }

    if (errorResult) {
      auto transErrorResultType = ArchetypeBuilder::mapTypeOutOfContext(
          M, genericEnv, errorResult->getType())->getCanonicalType();
      interfaceErrorResult =
        errorResult->getWithType(transErrorResultType);
    }
  } else {
    interfaceParams = params;
    interfaceResults = results;
    interfaceErrorResult = errorResult;
  }
  return SILFunctionType::get(genericSig, extInfo,
                              callee,
                              interfaceParams, interfaceResults,
                              interfaceErrorResult,
                              Context);
}

SILParameterInfo TypeResolver::resolveSILParameter(
                                 TypeRepr *repr,
                                 TypeResolutionOptions options) {
  assert((options & TR_FunctionInput) | (options & TR_ImmediateFunctionInput) &&
         "Parameters should be marked as inputs");
  auto convention = DefaultParameterConvention;
  Type type;
  bool hadError = false;

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    auto attrs = attrRepr->getAttrs();
    auto checkFor = [&](TypeAttrKind tak, ParameterConvention attrConv) {
      if (!attrs.has(tak)) return;
      if (convention != DefaultParameterConvention) {
        TC.diagnose(attrs.getLoc(tak), diag::sil_function_repeat_convention,
                    /*input*/ 0);
        hadError = true;
      }
      attrs.clearAttribute(tak);
      convention = attrConv;
    };
    checkFor(TypeAttrKind::TAK_in_guaranteed,
             ParameterConvention::Indirect_In_Guaranteed);
    checkFor(TypeAttrKind::TAK_in, ParameterConvention::Indirect_In);
    checkFor(TypeAttrKind::TAK_inout, ParameterConvention::Indirect_Inout);
    checkFor(TypeAttrKind::TAK_inout_aliasable,
             ParameterConvention::Indirect_InoutAliasable);
    checkFor(TypeAttrKind::TAK_owned, ParameterConvention::Direct_Owned);
    checkFor(TypeAttrKind::TAK_guaranteed,
             ParameterConvention::Direct_Guaranteed);
    checkFor(TypeAttrKind::TAK_deallocating,
             ParameterConvention::Direct_Deallocating);

    type = resolveAttributedType(attrs, attrRepr->getTypeRepr(), options);
  } else {
    type = resolveType(repr, options);
  }

  if (!type || type->hasError()) {
    hadError = true;

  // Diagnose types that are illegal in SIL.
  } else if (!type->isLegalSILType()) {
    TC.diagnose(repr->getLoc(), diag::illegal_sil_type, type);
    hadError = true;
  }

  if (hadError) type = ErrorType::get(Context);
  return SILParameterInfo(type->getCanonicalType(), convention);
}

bool TypeResolver::resolveSingleSILResult(TypeRepr *repr,
                                          TypeResolutionOptions options,
                              SmallVectorImpl<SILResultInfo> &ordinaryResults,
                                       Optional<SILResultInfo> &errorResult) {
  Type type;
  auto convention = DefaultResultConvention;
  bool isErrorResult = false;

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    // Copy the attributes out; we're going to destructively modify them.
    auto attrs = attrRepr->getAttrs();

    // Recognize @error.
    if (attrs.has(TypeAttrKind::TAK_error)) {
      attrs.clearAttribute(TypeAttrKind::TAK_error);
      isErrorResult = true;

      // Error results are always implicitly @owned.
      convention = ResultConvention::Owned;
    }

    // Recognize result conventions.
    bool hadError = false;
    auto checkFor = [&](TypeAttrKind tak, ResultConvention attrConv) {
      if (!attrs.has(tak)) return;
      if (convention != DefaultResultConvention) {
        TC.diagnose(attrs.getLoc(tak), diag::sil_function_repeat_convention,
                    /*result*/ 1);
        hadError = true;
      }
      attrs.clearAttribute(tak);
      convention = attrConv;
    };
    checkFor(TypeAttrKind::TAK_out, ResultConvention::Indirect);
    checkFor(TypeAttrKind::TAK_owned, ResultConvention::Owned);
    checkFor(TypeAttrKind::TAK_unowned_inner_pointer,
             ResultConvention::UnownedInnerPointer);
    checkFor(TypeAttrKind::TAK_autoreleased, ResultConvention::Autoreleased);
    if (hadError) return true;

    type = resolveAttributedType(attrs, attrRepr->getTypeRepr(), options);
  } else {
    type = resolveType(repr, options);
  }

  // Propagate type-resolution errors out.
  if (!type || type->hasError()) return true;

  // Diagnose types that are illegal in SIL.
  if (!type->isLegalSILType()) {
    TC.diagnose(repr->getStartLoc(), diag::illegal_sil_type, type);
    return false;
  }

  assert(!isErrorResult || convention == ResultConvention::Owned);
  SILResultInfo resolvedResult(type->getCanonicalType(), convention);

  if (!isErrorResult) {
    ordinaryResults.push_back(resolvedResult);
    return false;
  }

  // Error result types must have pointer-like representation.
  // FIXME: check that here?

  // We don't expect to have a reason to support multiple independent
  // error results.  (Would this be disjunctive or conjunctive?)
  if (errorResult.hasValue()) {
    TC.diagnose(repr->getStartLoc(),
                diag::sil_function_multiple_error_results);
    return true;
  }

  errorResult = resolvedResult;
  return false;
}

bool TypeResolver::resolveSILResults(TypeRepr *repr,
                                     TypeResolutionOptions options,
                                SmallVectorImpl<SILResultInfo> &ordinaryResults,
                                Optional<SILResultInfo> &errorResult) {
  if (auto tuple = dyn_cast<TupleTypeRepr>(repr)) {
    bool hadError = false;
    for (auto nameLoc : tuple->getUnderscoreLocs()) {
      if (nameLoc.isValid())
        TC.diagnose(nameLoc, diag::sil_function_output_label);
    }
    for (auto elt : tuple->getElements()) {
      if (resolveSingleSILResult(elt, options, ordinaryResults, errorResult))
        hadError = true;
    }
    return hadError;
  }

  return resolveSingleSILResult(repr, options, ordinaryResults, errorResult);
}

Type TypeResolver::resolveInOutType(InOutTypeRepr *repr,
                                    TypeResolutionOptions options) {
  // inout is only valid for function parameters.
  if (!(options & TR_FunctionInput) &&
      !(options & TR_ImmediateFunctionInput)) {
    TC.diagnose(repr->getInOutLoc(),
                (options & TR_VariadicFunctionInput)
                    ? diag::attr_not_on_variadic_parameters
                    : diag::attr_only_on_parameters,
                "'inout'");
    repr->setInvalid();
    return ErrorType::get(Context);
  }

  // Anything within the inout isn't a parameter anymore.
  options -= TR_ImmediateFunctionInput;
  options -= TR_FunctionInput;

  Type ty = resolveType(cast<InOutTypeRepr>(repr)->getBase(), options);
  if (!ty || ty->hasError()) return ty;
  return InOutType::get(ty);
}


Type TypeResolver::resolveArrayType(ArrayTypeRepr *repr,
                                    TypeResolutionOptions options) {
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), withoutContext(options));
  if (!baseTy || baseTy->hasError()) return baseTy;

  auto sliceTy = TC.getArraySliceType(repr->getBrackets().Start, baseTy);
  if (!sliceTy)
    return ErrorType::get(Context);

  // Check for _ObjectiveCBridgeable conformances in the element type.
  TC.useObjectiveCBridgeableConformances(DC, baseTy);

  return sliceTy;
}

Type TypeResolver::resolveDictionaryType(DictionaryTypeRepr *repr,
                                         TypeResolutionOptions options) {
  // FIXME: diagnose non-materializability of key/value type?
  Type keyTy = resolveType(repr->getKey(), withoutContext(options));
  if (!keyTy || keyTy->hasError()) return keyTy;

  Type valueTy = resolveType(repr->getValue(), withoutContext(options));
  if (!valueTy || valueTy->hasError()) return valueTy;

  auto dictDecl = TC.Context.getDictionaryDecl();

  if (auto dictTy = TC.getDictionaryType(repr->getBrackets().Start, keyTy, 
                                         valueTy)) {
    // Check the requirements on the generic arguments.
    auto unboundTy = dictDecl->getDeclaredType();
    TypeLoc args[2] = { TypeLoc(repr->getKey()), TypeLoc(repr->getValue()) };
    args[0].setType(keyTy, true);
    args[1].setType(valueTy, true);

    if (!TC.applyUnboundGenericArguments(
            unboundTy, dictDecl, repr->getStartLoc(), DC, args,
            options.contains(TR_GenericSignature), Resolver)) {
      return ErrorType::get(TC.Context);
    }

    // Check for _ObjectiveCBridgeable conformances in the key and value
    // types.
    TC.useObjectiveCBridgeableConformances(DC, keyTy);
    TC.useObjectiveCBridgeableConformances(DC, valueTy);

    return dictTy;
  }

  return ErrorType::get(Context);
}

Type TypeResolver::resolveOptionalType(OptionalTypeRepr *repr,
                                       TypeResolutionOptions options) {
  auto elementOptions = withoutContext(options, true);
  elementOptions |= TR_ImmediateOptionalTypeArgument;

  // The T in T? is a generic type argument and therefore always an AST type.
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), elementOptions);
  if (!baseTy || baseTy->hasError()) return baseTy;

  auto optionalTy = TC.getOptionalType(repr->getQuestionLoc(), baseTy);
  if (!optionalTy) return ErrorType::get(Context);

  return optionalTy;
}

Type TypeResolver::resolveImplicitlyUnwrappedOptionalType(
       ImplicitlyUnwrappedOptionalTypeRepr *repr,
       TypeResolutionOptions options) {
  auto elementOptions = withoutContext(options, true);
  elementOptions |= TR_ImmediateOptionalTypeArgument;

  // The T in T! is a generic type argument and therefore always an AST type.
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), elementOptions);
  if (!baseTy || baseTy->hasError()) return baseTy;

  auto uncheckedOptionalTy =
    TC.getImplicitlyUnwrappedOptionalType(repr->getExclamationLoc(), baseTy);
  if (!uncheckedOptionalTy)
    return ErrorType::get(Context);

  return uncheckedOptionalTy;
}

Type TypeResolver::resolveTupleType(TupleTypeRepr *repr,
                                    TypeResolutionOptions options) {
  bool isImmediateFunctionInput = options.contains(TR_ImmediateFunctionInput);
  SmallVector<TupleTypeElt, 8> elements;
  elements.reserve(repr->getNumElements());
  
  // If this is the top level of a function input list, peel off the
  // ImmediateFunctionInput marker and install a FunctionInput one instead.
  //
  // If we have a single ParenType though, don't clear these bits; we
  // still want to parse the type contained therein as if it were in
  // parameter position, meaning function types are not @escaping by
  // default.
  auto elementOptions = options;
  if (!repr->isParenType()) {
    elementOptions = withoutContext(elementOptions, true);
    if (isImmediateFunctionInput)
      elementOptions |= TR_FunctionInput;
  }

  bool complained = false;

  // Variadic tuples are not permitted.
  if (repr->hasEllipsis() &&
      !isImmediateFunctionInput) {
    TC.diagnose(repr->getEllipsisLoc(), diag::tuple_ellipsis);
    repr->removeEllipsis();
    complained = true;
  }

  for (unsigned i = 0, end = repr->getNumElements(); i != end; ++i) {
    auto *tyR = repr->getElement(i);
    Type ty;
    Identifier name;
    bool variadic = false;

    // If the element has a label, stash the label.
    // FIXME: Preserve and serialize parameter names in function types, maybe
    // with a new sugar type.
    if (!isImmediateFunctionInput)
      name = repr->getElementName(i);

    // If the element is a variadic parameter, resolve the parameter type as if
    // it were in non-parameter position, since we want functions to be
    // @escaping in this case.
    auto thisElementOptions = elementOptions;
    if (repr->hasEllipsis() &&
        elements.size() == repr->getEllipsisIndex()) {
      thisElementOptions = withoutContext(elementOptions);
      thisElementOptions |= TR_VariadicFunctionInput;
      variadic = true;
    }

    ty = resolveType(tyR, thisElementOptions);
    if (!ty || ty->hasError()) return ty;

    // If the element is a variadic parameter, the underlying type is actually
    // an ArraySlice of the element type.
    if (variadic)
      ty = TC.getArraySliceType(repr->getEllipsisLoc(), ty);

    auto paramFlags = isImmediateFunctionInput
                          ? ParameterTypeFlags::fromParameterType(ty, variadic)
                          : ParameterTypeFlags();
    elements.emplace_back(ty, name, paramFlags);
  }

  // Single-element labeled tuples are not permitted outside of declarations
  // or SIL, either.
  if (!isImmediateFunctionInput) {
    if (elements.size() == 1 && elements[0].hasName()
        && !(options & TR_SILType)
        && !(options & TR_EnumCase)) {
      if (!complained) {
        TC.diagnose(repr->getElementNameLoc(0),
                    diag::tuple_single_element)
          .fixItRemoveChars(repr->getElementNameLoc(0),
                            repr->getElement(0)->getStartLoc());
      }

      elements[0] = TupleTypeElt(elements[0].getType());
    }
  }

  return TupleType::get(elements, Context);
}

/// Restore Swift3 behavior of ambiguous composition for source compatibility.
///
/// Currently, 'P1 & P2.Type' is parsed as (composition P1, (metatype P2))
/// In Swift3, that was (metatype (composition P1, P2)).
/// For source compatibility, before resolving Type of that, reconstruct
/// TypeRepr as so, and emit a warning with fix-it to enclose it with
/// parenthesis; '(P1 & P2).Type'
//
/// \param Comp The type composition to be checked and fixed.
///
/// \returns Fixed TypeRepr, or nullptr that indicates no need to fix.
static TypeRepr *fixCompositionWithPostfix(TypeChecker &TC,
                                           CompositionTypeRepr *Comp) {
  // Only for Swift3
  if (!TC.Context.isSwiftVersion3())
    return nullptr;

  auto Types = Comp->getTypes();
  TypeRepr *LastType = nullptr;
  for (auto i = Types.begin(), e = Types.end(); i != e; ++i) {
    if (!isa<IdentTypeRepr>(*i)) {
      // Found non-IdentType not at the last, can't help.
      if (i + 1 != e)
        return nullptr;
      LastType = *i;
    }
  }
  // Only IdentType(s) it's OK.
  if (!LastType)
    return nullptr;

  // Strip off the postfix type repr.
  SmallVector<TypeRepr *, 2> Postfixes;
  while (true) {
    if (auto T = dyn_cast<ProtocolTypeRepr>(LastType)) {
      Postfixes.push_back(LastType);
      LastType = T->getBase();
    } else if (auto T = dyn_cast<MetatypeTypeRepr>(LastType)) {
      Postfixes.push_back(LastType);
      LastType = T->getBase();
    } else if (auto T = dyn_cast<OptionalTypeRepr>(LastType)) {
      Postfixes.push_back(LastType);
      LastType = T->getBase();
    } else if (auto T =
        dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(LastType)) {
      Postfixes.push_back(LastType);
      LastType = T->getBase();
    } else if (!isa<IdentTypeRepr>(LastType)) {
      // Found non-IdentTypeRepr, can't help;
      return nullptr;
    } else {
      break;
    }
  }
  assert(!Postfixes.empty() && isa<IdentTypeRepr>(LastType));

  // Now, we know we can fix-it. do it.
  SmallVector<TypeRepr *, 4> Protocols(Types.begin(), Types.end() - 1);
  Protocols.push_back(LastType);

  // Emit fix-it to enclose composition part into parentheses.
  TypeRepr *InnerMost = Postfixes.back();
  TC.diagnose(InnerMost->getLoc(), diag::protocol_composition_with_postfix,
      isa<ProtocolTypeRepr>(InnerMost) ? ".Protocol" :
      isa<MetatypeTypeRepr>(InnerMost) ? ".Type" :
      isa<OptionalTypeRepr>(InnerMost) ? "?" :
      isa<ImplicitlyUnwrappedOptionalTypeRepr>(InnerMost) ? "!" :
      /* unreachable */"")
    .highlight({Comp->getStartLoc(), LastType->getEndLoc()})
    .fixItInsert(Comp->getStartLoc(), "(")
    .fixItInsertAfter(LastType->getEndLoc(), ")");

  // Reconstruct postfix type repr with collected protocols.
  TypeRepr *Fixed = CompositionTypeRepr::create(
    TC.Context, Protocols, Comp->getStartLoc(),
    {Comp->getCompositionRange().Start, LastType->getEndLoc()});

  // Add back postix TypeRepr(s) to the composition.
  while (Postfixes.size()) {
    auto Postfix = Postfixes.pop_back_val();
    if (auto T = dyn_cast<ProtocolTypeRepr>(Postfix))
      Fixed = new (TC.Context) ProtocolTypeRepr(Fixed, T->getProtocolLoc());
    else if (auto T = dyn_cast<MetatypeTypeRepr>(Postfix))
      Fixed = new (TC.Context) MetatypeTypeRepr(Fixed, T->getMetaLoc());
    else if (auto T = dyn_cast<OptionalTypeRepr>(Postfix))
      Fixed = new (TC.Context) OptionalTypeRepr(Fixed, T->getQuestionLoc());
    else if (auto T = dyn_cast<ImplicitlyUnwrappedOptionalTypeRepr>(Postfix))
      Fixed = new (TC.Context)
        ImplicitlyUnwrappedOptionalTypeRepr(Fixed, T->getExclamationLoc());
    else
      llvm_unreachable("unexpected type repr");
  }

  return Fixed;
}

Type TypeResolver::resolveCompositionType(CompositionTypeRepr *repr,
                                          TypeResolutionOptions options) {

  // Fix 'P1 & P2.Type' to '(P1 & P2).Type' for Swift3
  if (auto fixed = fixCompositionWithPostfix(TC, repr))
    return resolveType(fixed, options);

  SmallVector<Type, 4> ProtocolTypes;
  for (auto tyR : repr->getTypes()) {
    Type ty = TC.resolveType(tyR, DC, withoutContext(options), Resolver);
    if (!ty || ty->hasError()) return ty;

    if (!ty->isExistentialType()) {
      TC.diagnose(tyR->getStartLoc(), diag::protocol_composition_not_protocol,
                  ty);
      continue;
    }

    ProtocolTypes.push_back(ty);
  }
  return ProtocolCompositionType::get(Context, ProtocolTypes);
}

Type TypeResolver::resolveMetatypeType(MetatypeTypeRepr *repr,
                                       TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  Type ty = resolveType(repr->getBase(), withoutContext(options));
  if (!ty || ty->hasError()) return ty;

  Optional<MetatypeRepresentation> storedRepr;
  
  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TR_SILType) {
    TC.diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildMetatypeType(repr, ty, storedRepr);
}

Type TypeResolver::buildMetatypeType(
       MetatypeTypeRepr *repr,
       Type instanceType,
       Optional<MetatypeRepresentation> storedRepr) {
  if (instanceType->isAnyExistentialType()) {
    // TODO: diagnose invalid representations?
    return ExistentialMetatypeType::get(instanceType, storedRepr);
  } else {
    return MetatypeType::get(instanceType, storedRepr);
  }
}

Type TypeResolver::resolveProtocolType(ProtocolTypeRepr *repr,
                                       TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  Type ty = resolveType(repr->getBase(), withoutContext(options));
  if (!ty || ty->hasError()) return ty;

  Optional<MetatypeRepresentation> storedRepr;
  
  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TR_SILType) {
    TC.diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildProtocolType(repr, ty, storedRepr);
}

Type TypeResolver::buildProtocolType(
       ProtocolTypeRepr *repr,
       Type instanceType,
       Optional<MetatypeRepresentation> storedRepr) {
  if (!instanceType->isAnyExistentialType()) {
    TC.diagnose(repr->getProtocolLoc(), diag::dot_protocol_on_non_existential,
                instanceType);
    return ErrorType::get(TC.Context);
  }

  return MetatypeType::get(instanceType, storedRepr);
}

Type TypeChecker::substMemberTypeWithBase(Module *module,
                                          const TypeDecl *member,
                                          Type baseTy) {
  // The declared interface type for a generic type will have the type
  // arguments; strip them off.
  if (isa<NominalTypeDecl>(member)) {
    auto memberType = member->getDeclaredType();

    if (baseTy->is<ModuleType>())
      return memberType;

    if (baseTy->getAnyNominal() ||
        baseTy->is<UnboundGenericType>()) {
      if (auto *unboundGenericType = memberType->getAs<UnboundGenericType>()) {
        return UnboundGenericType::get(
            unboundGenericType->getDecl(), baseTy,
            unboundGenericType->getASTContext());
      } else if (auto *nominalType = memberType->getAs<NominalType>()) {
        return NominalType::get(
            nominalType->getDecl(), baseTy,
            nominalType->getASTContext());
      }
      llvm_unreachable("Not a nominal type?");
    }

    return memberType;
  } else {
    auto memberType = member->getDeclaredInterfaceType();
    return baseTy->getTypeOfMember(module, member, this, memberType);
  }
}

Type TypeChecker::getSuperClassOf(Type type) {
  return type->getSuperclass(this);
}

Type TypeChecker::resolveMemberType(DeclContext *dc, Type type,
                                    Identifier name) {
  LookupTypeResult memberTypes = lookupMemberType(dc, type, name);
  if (!memberTypes)
    return Type();

  // FIXME: Detect ambiguities here?
  return memberTypes.back().second;
}

static void lookupAndAddLibraryTypes(TypeChecker &TC,
                                     Module *Stdlib,
                                     ArrayRef<Identifier> TypeNames,
                                     llvm::DenseSet<CanType> &Types) {
  SmallVector<ValueDecl *, 4> Results;
  for (Identifier Id : TypeNames) {
    Stdlib->lookupValue({}, Id, NLKind::UnqualifiedLookup, Results);
    for (auto *VD : Results) {
      if (auto *TD = dyn_cast<TypeDecl>(VD)) {
        TC.validateDecl(TD);
        Types.insert(TD->getDeclaredType()->getCanonicalType());
      }
    }
    Results.clear();
  }
}

/// Emit an additional diagnostic describing why we are applying @objc to the
/// decl, if this is not obvious from the decl itself.
static void describeObjCReason(TypeChecker &TC, const ValueDecl *VD,
                               ObjCReason Reason) {
  if (Reason == ObjCReason::MemberOfObjCProtocol) {
    TC.diagnose(VD->getLoc(), diag::objc_inferring_on_objc_protocol_member);
  } else if (Reason == ObjCReason::OverridesObjC) {
    unsigned kind = isa<VarDecl>(VD) ? 0
                  : isa<SubscriptDecl>(VD) ? 1
                  : isa<ConstructorDecl>(VD) ? 2
                  : 3;

    auto overridden = VD->getOverriddenDecl();
    TC.diagnose(overridden, diag::objc_overriding_objc_decl,
                kind, VD->getOverriddenDecl()->getFullName());
  } else if (Reason == ObjCReason::WitnessToObjC) {
    auto requirement = TC.findWitnessedObjCRequirements(VD).front();
    TC.diagnose(requirement, diag::objc_witness_objc_requirement,
                VD->getDescriptiveKind(), requirement->getFullName(),
                cast<ProtocolDecl>(requirement->getDeclContext())
                  ->getFullName());
  }
}

static void diagnoseFunctionParamNotRepresentable(
    TypeChecker &TC, const AbstractFunctionDecl *AFD, unsigned NumParams,
    unsigned ParamIndex, const ParamDecl *P, ObjCReason Reason) {
  if (Reason == ObjCReason::DoNotDiagnose)
    return;

  if (NumParams == 1) {
    TC.diagnose(AFD->getLoc(), diag::objc_invalid_on_func_single_param_type,
                getObjCDiagnosticAttrKind(Reason));
  } else {
    TC.diagnose(AFD->getLoc(), diag::objc_invalid_on_func_param_type,
                ParamIndex + 1, getObjCDiagnosticAttrKind(Reason));
  }
  if (P->hasType()) {
    Type ParamTy = P->getType();
    SourceRange SR;
    if (auto typeRepr = P->getTypeLoc().getTypeRepr())
      SR = typeRepr->getSourceRange();
    TC.diagnoseTypeNotRepresentableInObjC(AFD, ParamTy, SR);
  }
  describeObjCReason(TC, AFD, Reason);
}

static bool isParamListRepresentableInObjC(TypeChecker &TC,
                                           const AbstractFunctionDecl *AFD,
                                           const ParameterList *PL,
                                           ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsObjC.

  bool Diagnose = (Reason != ObjCReason::DoNotDiagnose);

  bool IsObjC = true;
  unsigned NumParams = PL->size();
  for (unsigned ParamIndex = 0; ParamIndex != NumParams; ParamIndex++) {
    auto param = PL->get(ParamIndex);
    
    // Swift Varargs are not representable in Objective-C.
    if (param->isVariadic()) {
      if (Diagnose && Reason != ObjCReason::DoNotDiagnose) {
        TC.diagnose(param->getStartLoc(), diag::objc_invalid_on_func_variadic,
                    getObjCDiagnosticAttrKind(Reason))
          .highlight(param->getSourceRange());
        describeObjCReason(TC, AFD, Reason);
      }
      
      return false;
    }
    
    if (param->getType()->isRepresentableIn(
          ForeignLanguage::ObjectiveC,
          const_cast<AbstractFunctionDecl *>(AFD)))
      continue;
    
    // Permit '()' when this method overrides a method with a
    // foreign error convention that replaces NSErrorPointer with ()
    // and this is the replaced parameter.
    AbstractFunctionDecl *overridden;
    if (param->getType()->isVoid() && AFD->hasThrows() &&
        (overridden = AFD->getOverriddenDecl())) {
      auto foreignError = overridden->getForeignErrorConvention();
      if (foreignError &&
          foreignError->isErrorParameterReplacedWithVoid() &&
          foreignError->getErrorParameterIndex() == ParamIndex) {
        continue;
      }
    }

    IsObjC = false;
    if (!Diagnose) {
      // Save some work and return as soon as possible if we are not
      // producing diagnostics.
      return IsObjC;
    }
    diagnoseFunctionParamNotRepresentable(TC, AFD, NumParams, ParamIndex,
                                          param, Reason);
  }
  return IsObjC;
}

/// Check whether the given declaration occurs within a constrained
/// extension, or an extension of a class with generic ancestry, or an
/// extension of an Objective-C runtime visible class, and
/// therefore is not representable in Objective-C.
static bool checkObjCInExtensionContext(TypeChecker &tc,
                                        const ValueDecl *value,
                                        bool diagnose) {
  auto DC = value->getDeclContext();

  if (auto ED = dyn_cast<ExtensionDecl>(DC)) {
    if (ED->getTrailingWhereClause()) {
      if (diagnose) {
        tc.diagnose(value->getLoc(), diag::objc_in_extension_context);
      }
      return true;
    }

    // Check if any classes in the inheritance hierarchy have generic
    // parameters.
    // FIXME: This is a current limitation, not inherent. We don't have
    // a concrete class to attach Objective-C category metadata to.
    Type extendedTy = ED->getDeclaredTypeInContext();
    while (!extendedTy.isNull()) {
      const ClassDecl *CD = extendedTy->getClassOrBoundGenericClass();
      if (!CD)
        break;

      if (!CD->hasClangNode() && CD->getGenericParams()) {
        if (diagnose) {
          tc.diagnose(value, diag::objc_in_generic_extension);
        }
        return true;
      }

      extendedTy = CD->getSuperclass();
    }
  }

  return false;
}

/// Check whether the given declaration contains its own generic parameters,
/// and therefore is not representable in Objective-C.
static bool checkObjCWithGenericParams(TypeChecker &TC,
                                       const AbstractFunctionDecl *AFD,
                                       ObjCReason Reason) {
  bool Diagnose = (Reason != ObjCReason::DoNotDiagnose);

  if (AFD->getGenericParams()) {
    // Diagnose this problem, if asked to.
    if (Diagnose) {
      TC.diagnose(AFD->getLoc(), diag::objc_invalid_with_generic_params,
                  getObjCDiagnosticAttrKind(Reason));
      describeObjCReason(TC, AFD, Reason);
    }

    return true;
  }

  return false;
}

/// CF types cannot have @objc methods, because they don't have real class
/// objects.
static bool checkObjCInForeignClassContext(TypeChecker &TC,
                                           const ValueDecl *VD,
                                           ObjCReason Reason) {
  bool Diagnose = (Reason != ObjCReason::DoNotDiagnose);

  auto type = VD->getDeclContext()->getDeclaredTypeInContext();
  if (!type)
    return false;

  auto clas = type->getClassOrBoundGenericClass();
  if (!clas)
    return false;

  switch (clas->getForeignClassKind()) {
  case ClassDecl::ForeignKind::Normal:
    return false;

  case ClassDecl::ForeignKind::CFType:
    if (Diagnose) {
      TC.diagnose(VD, diag::objc_invalid_on_foreign_class,
                  getObjCDiagnosticAttrKind(Reason));
      describeObjCReason(TC, VD, Reason);
    }
    break;

  case ClassDecl::ForeignKind::RuntimeOnly:
    if (Diagnose) {
      TC.diagnose(VD, diag::objc_in_objc_runtime_visible,
                  VD->getDescriptiveKind(), getObjCDiagnosticAttrKind(Reason),
                  clas->getName());
      describeObjCReason(TC, VD, Reason);
    }
    break;
  }

  return true;
}

bool TypeChecker::isCIntegerType(const DeclContext *DC, Type T) {
  if (CIntegerTypes.empty())
    fillObjCRepresentableTypeCache(DC);
  return CIntegerTypes.count(T->getCanonicalType());
}

/// Determines whether the given type is bridged to an Objective-C class type.
static bool isBridgedToObjectiveCClass(DeclContext *dc, Type type) {
  switch (type->getForeignRepresentableIn(ForeignLanguage::ObjectiveC, dc)
            .first) {
  case ForeignRepresentableKind::Trivial:
  case ForeignRepresentableKind::None:
    return false;

  case ForeignRepresentableKind::Object:
  case ForeignRepresentableKind::Bridged:
  case ForeignRepresentableKind::BridgedError:
  case ForeignRepresentableKind::StaticBridged:
    return true;
  }

  llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
}

bool TypeChecker::isRepresentableInObjC(
       const AbstractFunctionDecl *AFD,
       ObjCReason Reason,
       Optional<ForeignErrorConvention> &errorConvention) {
  // Clear out the error convention. It will be added later if needed.
  errorConvention = None;

  // If you change this function, you must add or modify a test in PrintAsObjC.

  bool Diagnose = (Reason != ObjCReason::DoNotDiagnose);

  if (checkObjCInForeignClassContext(*this, AFD, Reason))
    return false;
  if (checkObjCWithGenericParams(*this, AFD, Reason))
    return false;
  if (checkObjCInExtensionContext(*this, AFD, Diagnose))
    return false;

  if (AFD->isOperator()) {
    assert(isa<ProtocolDecl>(AFD->getDeclContext()) &&
           "all other cases should be caught earlier");
    diagnose(AFD, diag::objc_operator_proto);
    return false;
  }

  if (auto *FD = dyn_cast<FuncDecl>(AFD)) {
    if (FD->isAccessor()) {
      // Accessors can only be @objc if the storage declaration is.
      // Global computed properties may however @_cdecl their accessors.
      auto storage = FD->getAccessorStorageDecl();
      validateDecl(storage);
      if (!storage->isObjC() && Reason != ObjCReason::ExplicitlyCDecl) {
        if (Diagnose) {
          auto error = FD->isGetter()
                    ? (isa<VarDecl>(storage) 
                         ? diag::objc_getter_for_nonobjc_property
                         : diag::objc_getter_for_nonobjc_subscript)
                    : (isa<VarDecl>(storage)
                         ? diag::objc_setter_for_nonobjc_property
                         : diag::objc_setter_for_nonobjc_subscript);

          diagnose(FD->getLoc(), error);
          describeObjCReason(*this, AFD, Reason);
        }
        return false;
      }
    } else {
      unsigned ExpectedParamPatterns = 1;
      if (FD->getImplicitSelfDecl())
        ExpectedParamPatterns++;
      if (FD->getParameterLists().size() != ExpectedParamPatterns) {
        if (Diagnose) {
          diagnose(AFD->getLoc(), diag::objc_invalid_on_func_curried,
                   getObjCDiagnosticAttrKind(Reason));
          describeObjCReason(*this, AFD, Reason);
        }
        return false;
      }
    }

    // willSet/didSet implementations are never exposed to objc, they are always
    // directly dispatched from the synthesized setter.
    if (FD->isObservingAccessor()) {
      if (Diagnose) {
        diagnose(AFD->getLoc(), diag::objc_observing_accessor);
        describeObjCReason(*this, AFD, Reason);
      }
      return false;
    }
  }

  // As a special case, an initializer with a single, named parameter of type
  // '()' is always representable in Objective-C. This allows us to cope with
  // zero-parameter methods with selectors that are longer than "init". For
  // example, this allows:
  //
  // \code
  // class Foo {
  //   @objc init(malice: ()) { } // selector is "initWithMalice"
  // }
  // \endcode
  bool isSpecialInit = false;
  if (auto init = dyn_cast<ConstructorDecl>(AFD))
    isSpecialInit = init->isObjCZeroParameterWithLongSelector();

  if (!isSpecialInit &&
      !isParamListRepresentableInObjC(*this, AFD,
                                      AFD->getParameterLists().back(),
                                      Reason)) {
    if (!Diagnose) {
      // Return as soon as possible if we are not producing diagnostics.
      return false;
    }
  }

  if (auto FD = dyn_cast<FuncDecl>(AFD)) {
    Type ResultType = FD->getResultInterfaceType();
    if (!ResultType->isVoid() &&
        !ResultType->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                       const_cast<FuncDecl *>(FD))) {
      if (Diagnose) {
        diagnose(AFD->getLoc(), diag::objc_invalid_on_func_result_type,
                 getObjCDiagnosticAttrKind(Reason));
        SourceRange Range =
            FD->getBodyResultTypeLoc().getTypeRepr()->getSourceRange();
        diagnoseTypeNotRepresentableInObjC(FD, ResultType, Range);
        describeObjCReason(*this, FD, Reason);
      }
      return false;
    }
  }

  // Throwing functions must map to a particular error convention.
  if (AFD->hasThrows()) {
    DeclContext *dc = const_cast<AbstractFunctionDecl *>(AFD);
    SourceLoc throwsLoc;
    Type resultType;

    const ConstructorDecl *ctor = nullptr;
    if (auto func = dyn_cast<FuncDecl>(AFD)) {
      resultType = func->getResultInterfaceType();
      throwsLoc = func->getThrowsLoc();
    } else {
      ctor = cast<ConstructorDecl>(AFD);
      throwsLoc = ctor->getThrowsLoc();
    }

    ForeignErrorConvention::Kind kind;
    CanType errorResultType;
    Type optOptionalType;
    if (ctor) {
      // Initializers always use the nil result convention.
      kind = ForeignErrorConvention::NilResult;

      // Only non-failing initializers can throw.
      if (ctor->getFailability() != OTK_None) {
        if (Diagnose) {
          diagnose(AFD->getLoc(), diag::objc_invalid_on_failing_init,
                   getObjCDiagnosticAttrKind(Reason))
            .highlight(throwsLoc);
          describeObjCReason(*this, AFD, Reason);
        }

        return false;
      }
    } else if (resultType->isVoid()) {
      // Functions that return nothing (void) can be throwing; they indicate
      // failure with a 'false' result.
      kind = ForeignErrorConvention::ZeroResult;
      NominalTypeDecl *boolDecl = Context.getObjCBoolDecl();
      // On Linux, we might still run @objc tests even though there's
      // no ObjectiveC Foundation, so use Swift.Bool instead of crapping
      // out.
      if (boolDecl == nullptr)
        boolDecl = Context.getBoolDecl();

      if (boolDecl == nullptr) {
        diagnose(AFD->getLoc(), diag::broken_bool);
        return false;
      }

      errorResultType = boolDecl->getDeclaredType()->getCanonicalType();
    } else if (!resultType->getAnyOptionalObjectType() &&
               isBridgedToObjectiveCClass(dc, resultType)) {
      // Functions that return a (non-optional) type bridged to Objective-C
      // can be throwing; they indicate failure with a nil result.
      kind = ForeignErrorConvention::NilResult;
    } else if ((optOptionalType = resultType->getAnyOptionalObjectType()) &&
               isBridgedToObjectiveCClass(dc, optOptionalType)) {
      // Cannot return an optional bridged type, because 'nil' is reserved
      // to indicate failure. Call this out in a separate diagnostic.
      if (Diagnose) {
        diagnose(AFD->getLoc(),
                 diag::objc_invalid_on_throwing_optional_result,
                 getObjCDiagnosticAttrKind(Reason),
                 resultType)
          .highlight(throwsLoc);
        describeObjCReason(*this, AFD, Reason);
      }
      return false;
    } else {
      // Other result types are not permitted.
      if (Diagnose) {
        diagnose(AFD->getLoc(),
                 diag::objc_invalid_on_throwing_result,
                 getObjCDiagnosticAttrKind(Reason),
                 resultType)
          .highlight(throwsLoc);
        describeObjCReason(*this, AFD, Reason);
      }
      return false;
    }

    // The error type is always 'AutoreleasingUnsafeMutablePointer<NSError?>?'.
    Type errorParameterType = getNSErrorType(dc);
    if (errorParameterType) {
      errorParameterType = OptionalType::get(errorParameterType);
      errorParameterType
        = BoundGenericType::get(
            Context.getAutoreleasingUnsafeMutablePointerDecl(),
            nullptr,
            errorParameterType);
      errorParameterType = OptionalType::get(errorParameterType);
    }

    // Determine the parameter index at which the error will go.
    unsigned errorParameterIndex;
    bool foundErrorParameterIndex = false;

    // If there is an explicit @objc attribute with a name, look for
    // the "error" selector piece.
    if (auto objc = AFD->getAttrs().getAttribute<ObjCAttr>()) {
      if (auto objcName = objc->getName()) {
        auto selectorPieces = objcName->getSelectorPieces();
        for (unsigned i = selectorPieces.size(); i > 0; --i) {
          // If the selector piece is "error", this is the location of
          // the error parameter.
          auto piece = selectorPieces[i-1];
          if (piece == Context.Id_error) {
            errorParameterIndex = i-1;
            foundErrorParameterIndex = true;
            break;
          }

          // If the first selector piece ends with "Error", it's here.
          if (i == 1 && camel_case::getLastWord(piece.str()) == "Error") {
            errorParameterIndex = i-1;
            foundErrorParameterIndex = true;
            break;
          }
        }
      }
    }

    // If the selector did not provide an index for the error, find
    // the last parameter that is not a trailing closure.
    if (!foundErrorParameterIndex) {
      auto *paramList = AFD->getParameterLists().back();
      errorParameterIndex = paramList->size();
      while (errorParameterIndex > 0) {
        // Skip over trailing closures.
        auto type = paramList->get(errorParameterIndex - 1)->getType();

        // It can't be a trailing closure unless it has a specific form.
        // Only consider the rvalue type.
        type = type->getRValueType();
        
        // Look through one level of optionality.
        if (auto objectType = type->getAnyOptionalObjectType())
          type = objectType;
        
        // Is it a function type?
        if (!type->is<AnyFunctionType>()) break;
        --errorParameterIndex;
      }
    }

    // Form the error convention.
    CanType canErrorParameterType;
    if (errorParameterType)
      canErrorParameterType = errorParameterType->getCanonicalType();
    switch (kind) {
    case ForeignErrorConvention::ZeroResult:
      errorConvention = ForeignErrorConvention::getZeroResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType,
                          errorResultType);
      break;

    case ForeignErrorConvention::NonZeroResult:
      errorConvention = ForeignErrorConvention::getNonZeroResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType,
                          errorResultType);
      break;

    case ForeignErrorConvention::ZeroPreservedResult:
      errorConvention = ForeignErrorConvention::getZeroPreservedResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;

    case ForeignErrorConvention::NilResult:
      errorConvention = ForeignErrorConvention::getNilResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;

    case ForeignErrorConvention::NonNilError:
      errorConvention = ForeignErrorConvention::getNilResult(
                          errorParameterIndex,
                          ForeignErrorConvention::IsNotOwned,
                          ForeignErrorConvention::IsNotReplaced,
                          canErrorParameterType);
      break;
    }
  }

  return true;
}

bool TypeChecker::isRepresentableInObjC(const VarDecl *VD, ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsObjC.

  if (VD->isInvalid())
    return false;

  Type T = VD->getType();
  if (auto *RST = T->getAs<ReferenceStorageType>()) {
    // In-memory layout of @weak and @unowned does not correspond to anything
    // in Objective-C, but this does not really matter here, since Objective-C
    // uses getters and setters to operate on the property.
    // Because of this, look through @weak and @unowned.
    T = RST->getReferentType();
  }
  bool Result = T->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                     VD->getDeclContext());
  bool Diagnose = (Reason != ObjCReason::DoNotDiagnose);

  if (Result && checkObjCInExtensionContext(*this, VD, Diagnose))
    return false;

  if (checkObjCInForeignClassContext(*this, VD, Reason))
    return false;

  if (!Diagnose || Result)
    return Result;

  SourceRange TypeRange = VD->getTypeSourceRangeForDiagnostics();
  diagnose(VD->getLoc(), diag::objc_invalid_on_var,
           getObjCDiagnosticAttrKind(Reason))
      .highlight(TypeRange);
  diagnoseTypeNotRepresentableInObjC(VD->getDeclContext(), VD->getType(),
                                     TypeRange);
  describeObjCReason(*this, VD, Reason);

  return Result;
}

bool TypeChecker::isRepresentableInObjC(const SubscriptDecl *SD,
                                        ObjCReason Reason) {
  // If you change this function, you must add or modify a test in PrintAsObjC.

  bool Diagnose = (Reason != ObjCReason::DoNotDiagnose);

  if (checkObjCInForeignClassContext(*this, SD, Reason))
    return false;

  // Figure out the type of the indices.
  Type IndicesType = SD->getIndicesType();
  if (auto TupleTy = IndicesType->getAs<TupleType>()) {
    if (TupleTy->getNumElements() == 1 && !TupleTy->getElement(0).isVararg())
      IndicesType = TupleTy->getElementType(0);
  }
  
  if (IndicesType->hasError())
    return false;

  bool IndicesResult =
    IndicesType->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                   SD->getDeclContext());
  bool ElementResult =
    SD->getElementType()->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                            SD->getDeclContext());
  bool Result = IndicesResult && ElementResult;

  if (Result && checkObjCInExtensionContext(*this, SD, Diagnose))
    return false;

  // Make sure we know how to map the selector appropriately.
  if (Result && SD->getObjCSubscriptKind(this) == ObjCSubscriptKind::None) {
    SourceRange IndexRange = SD->getIndices()->getSourceRange();
    diagnose(SD->getLoc(), diag::objc_invalid_subscript_key_type,
             getObjCDiagnosticAttrKind(Reason), IndicesType)
      .highlight(IndexRange);
    return false;
  }

  if (!Diagnose || Result)
    return Result;

  SourceRange TypeRange;
  if (!IndicesResult)
    TypeRange = SD->getIndices()->getSourceRange();
  else
    TypeRange = SD->getElementTypeLoc().getSourceRange();
  diagnose(SD->getLoc(), diag::objc_invalid_on_subscript,
           getObjCDiagnosticAttrKind(Reason))
    .highlight(TypeRange);

  diagnoseTypeNotRepresentableInObjC(SD->getDeclContext(),
                                     !IndicesResult? IndicesType
                                                   : SD->getElementType(),
                                     TypeRange);
  describeObjCReason(*this, SD, Reason);

  return Result;
}

void TypeChecker::diagnoseTypeNotRepresentableInObjC(const DeclContext *DC,
                                                     Type T,
                                                     SourceRange TypeRange) {
  // Special diagnostic for tuples.
  if (T->is<TupleType>()) {
    if (T->isVoid())
      diagnose(TypeRange.Start, diag::not_objc_empty_tuple)
          .highlight(TypeRange);
    else
      diagnose(TypeRange.Start, diag::not_objc_tuple)
          .highlight(TypeRange);
    return;
  }

  // Special diagnostic for classes.
  if (auto *CD = T->getClassOrBoundGenericClass()) {
    if (!CD->isObjC())
      diagnose(TypeRange.Start, diag::not_objc_swift_class)
          .highlight(TypeRange);
    return;
  }

  // Special diagnostic for structs.
  if (T->is<StructType>()) {
    diagnose(TypeRange.Start, diag::not_objc_swift_struct)
        .highlight(TypeRange);
    return;
  }

  // Special diagnostic for enums.
  if (T->is<EnumType>()) {
    diagnose(TypeRange.Start, diag::not_objc_swift_enum)
        .highlight(TypeRange);
    return;
  }

  // Special diagnostic for protocols and protocol compositions.
  SmallVector<ProtocolDecl *, 4> Protocols;
  if (T->isExistentialType(Protocols)) {
    if (Protocols.empty()) {
      // Any is not @objc.
      diagnose(TypeRange.Start, diag::not_objc_empty_protocol_composition);
      return;
    }
    // Find a protocol that is not @objc.
    bool sawErrorProtocol = false;
    for (auto PD : Protocols) {
      if (PD->isSpecificProtocol(KnownProtocolKind::Error)) {
        sawErrorProtocol = true;
        break;
      }

      if (!PD->isObjC()) {
        diagnose(TypeRange.Start, diag::not_objc_protocol,
                 PD->getDeclaredType());
        return;
      }
    }

    if (sawErrorProtocol) {
      diagnose(TypeRange.Start, diag::not_objc_error_protocol_composition);
      return;
    }

    return;
  }

  if (T->is<ArchetypeType>()) {
    diagnose(TypeRange.Start, diag::not_objc_generic_type_param)
        .highlight(TypeRange);
    return;
  }

  if (auto fnTy = T->getAs<FunctionType>()) {
    if (fnTy->getExtInfo().throws() ) {
      diagnose(TypeRange.Start, diag::not_objc_function_type_throwing)
        .highlight(TypeRange);
      return;
    }

    diagnose(TypeRange.Start, diag::not_objc_function_type_param)
      .highlight(TypeRange);
    return;
  }
}

void TypeChecker::fillObjCRepresentableTypeCache(const DeclContext *DC) {
  if (!CIntegerTypes.empty())
    return;

  SmallVector<Identifier, 32> StdlibTypeNames;
  Module *Stdlib = getStdlibModule(DC);
#define MAP_BUILTIN_TYPE(_, __)
#define MAP_BUILTIN_INTEGER_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME) \
  StdlibTypeNames.push_back(Context.getIdentifier(#SWIFT_TYPE_NAME));
#include "swift/ClangImporter/BuiltinMappedTypes.def"
  lookupAndAddLibraryTypes(*this, Stdlib, StdlibTypeNames, CIntegerTypes);
}

namespace {

class UnsupportedProtocolVisitor
  : public TypeReprVisitor<UnsupportedProtocolVisitor>, public ASTWalker
{
  TypeChecker &TC;
  bool recurseIntoSubstatements;
  bool hitTopStmt;
    
public:
  UnsupportedProtocolVisitor(TypeChecker &tc) : TC(tc) {
    recurseIntoSubstatements = true;
    hitTopStmt = false;
  }

  void setRecurseIntoSubstatements(bool recurse) {
    recurseIntoSubstatements = recurse;
  }

  bool walkToTypeReprPre(TypeRepr *T) {
    if (T->isInvalid())
      return false;
    if (auto compound = dyn_cast<CompoundIdentTypeRepr>(T)) {
      // Only visit the last component to check, because nested typealiases in
      // existentials are okay.
      visit(compound->getComponentRange().back());
      return false;
    }
    visit(T);
    return true;
  }

  std::pair<bool, Stmt*> walkToStmtPre(Stmt *S) {
    if (recurseIntoSubstatements) {
      return { true, S };
    } else if (hitTopStmt) {
      return { false, S };
    } else {
      hitTopStmt = true;
      return { true, S };
    }
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    if (T->isInvalid())
      return;
    
    auto comp = T->getComponentRange().back();
    if (auto proto = dyn_cast_or_null<ProtocolDecl>(comp->getBoundDecl())) {
      if (!proto->existentialTypeSupported(&TC)) {
        TC.diagnose(comp->getIdLoc(), diag::unsupported_existential_type,
                    proto->getName());
        T->setInvalid();
      }
    } else if (auto alias = dyn_cast_or_null<TypeAliasDecl>(comp->getBoundDecl())) {
      if (!alias->hasUnderlyingType())
        return;
      auto type = alias->getUnderlyingType();
      type.findIf([&](Type type) -> bool {
        if (T->isInvalid())
          return false;
        SmallVector<ProtocolDecl*, 2> protocols;
        if (type->isExistentialType(protocols)) {
          for (auto *proto : protocols) {
            if (proto->existentialTypeSupported(&TC))
              continue;
            
            TC.diagnose(comp->getIdLoc(), diag::unsupported_existential_type,
                        proto->getName());
            T->setInvalid();
          }
        }
        return false;
      });
    }
  }
};

}

void TypeChecker::checkUnsupportedProtocolType(Decl *decl) {
  if (!decl || decl->isInvalid())
    return;

  // Global type aliases are okay.
  if (isa<TypeAliasDecl>(decl) &&
      decl->getDeclContext()->isModuleScopeContext())
    return;

  // Non-typealias type declarations are okay.
  if (isa<TypeDecl>(decl) && !isa<TypeAliasDecl>(decl))
    return;

  // Extensions are okay.
  if (isa<ExtensionDecl>(decl))
    return;

  UnsupportedProtocolVisitor visitor(*this);
  decl->walk(visitor);
}

void TypeChecker::checkUnsupportedProtocolType(Stmt *stmt) {
  if (!stmt)
    return;

  UnsupportedProtocolVisitor visitor(*this);
    
  // This method will already be called for all individual statements, so don't repeat
  // that checking by walking into any statement inside this one.
  visitor.setRecurseIntoSubstatements(false);
  stmt->walk(visitor);
}
