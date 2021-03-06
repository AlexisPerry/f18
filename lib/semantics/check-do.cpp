//===-- lib/semantics/check-do.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "check-do.h"
#include "flang/common/template.h"
#include "flang/evaluate/call.h"
#include "flang/evaluate/expression.h"
#include "flang/evaluate/tools.h"
#include "flang/parser/message.h"
#include "flang/parser/parse-tree-visitor.h"
#include "flang/parser/tools.h"
#include "flang/semantics/attr.h"
#include "flang/semantics/scope.h"
#include "flang/semantics/semantics.h"
#include "flang/semantics/symbol.h"
#include "flang/semantics/tools.h"
#include "flang/semantics/type.h"

namespace Fortran::evaluate {
using ActualArgumentRef = common::Reference<const ActualArgument>;

inline bool operator<(ActualArgumentRef x, ActualArgumentRef y) {
  return &*x < &*y;
}
}

namespace Fortran::semantics {

using namespace parser::literals;

using Bounds = parser::LoopControl::Bounds;

static const std::list<parser::ConcurrentControl> &GetControls(
    const parser::LoopControl &loopControl) {
  const auto &concurrent{
      std::get<parser::LoopControl::Concurrent>(loopControl.u)};
  const auto &header{std::get<parser::ConcurrentHeader>(concurrent.t)};
  return std::get<std::list<parser::ConcurrentControl>>(header.t);
}

static const Bounds &GetBounds(const parser::DoConstruct &doConstruct) {
  auto &loopControl{doConstruct.GetLoopControl().value()};
  return std::get<Bounds>(loopControl.u);
}

static const parser::Name &GetDoVariable(
    const parser::DoConstruct &doConstruct) {
  const Bounds &bounds{GetBounds(doConstruct)};
  return bounds.name.thing;
}

// Return the (possibly null)  name of the construct
template<typename A>
static const parser::Name *MaybeGetConstructName(const A &a) {
  return common::GetPtrFromOptional(std::get<0>(std::get<0>(a.t).statement.t));
}

static parser::MessageFixedText GetEnclosingDoMsg() {
  return "Enclosing DO CONCURRENT statement"_en_US;
}

static const parser::Name *MaybeGetConstructName(
    const parser::BlockConstruct &blockConstruct) {
  return common::GetPtrFromOptional(
      std::get<parser::Statement<parser::BlockStmt>>(blockConstruct.t)
          .statement.v);
}

static void SayWithDo(SemanticsContext &context, parser::CharBlock stmtLocation,
    parser::MessageFixedText &&message, parser::CharBlock doLocation) {
  context.Say(stmtLocation, message).Attach(doLocation, GetEnclosingDoMsg());
}

// 11.1.7.5 - enforce semantics constraints on a DO CONCURRENT loop body
class DoConcurrentBodyEnforce {
public:
  DoConcurrentBodyEnforce(
      SemanticsContext &context, parser::CharBlock doConcurrentSourcePosition)
    : context_{context}, doConcurrentSourcePosition_{
                             doConcurrentSourcePosition} {}
  std::set<parser::Label> labels() { return labels_; }
  template<typename T> bool Pre(const T &) { return true; }
  template<typename T> void Post(const T &) {}

  template<typename T> bool Pre(const parser::Statement<T> &statement) {
    currentStatementSourcePosition_ = statement.source;
    if (statement.label.has_value()) {
      labels_.insert(*statement.label);
    }
    return true;
  }

  // C1140 -- Can't deallocate a polymorphic entity in a DO CONCURRENT.
  // Deallocation can be caused by exiting a block that declares an allocatable
  // entity, assignment to an allocatable variable, or an actual DEALLOCATE
  // statement
  //
  // Note also that the deallocation of a derived type entity might cause the
  // invocation of an IMPURE final subroutine.
  //

  // Predicate for deallocations caused by block exit and direct deallocation
  static bool DeallocateAll(const Symbol &) { return true; }

  // Predicate for deallocations caused by intrinsic assignment
  static bool DeallocateNonCoarray(const Symbol &component) {
    return !IsCoarray(component);
  }

  static bool WillDeallocatePolymorphic(const Symbol &entity,
      const std::function<bool(const Symbol &)> &WillDeallocate) {
    return WillDeallocate(entity) && IsPolymorphicAllocatable(entity);
  }

  // Is it possible that we will we deallocate a polymorphic entity or one
  // of its components?
  static bool MightDeallocatePolymorphic(const Symbol &entity,
      const std::function<bool(const Symbol &)> &WillDeallocate) {
    if (const Symbol * root{GetAssociationRoot(entity)}) {
      // Check the entity itself, no coarray exception here
      if (IsPolymorphicAllocatable(*root)) {
        return true;
      }
      // Check the components
      if (const auto *details{root->detailsIf<ObjectEntityDetails>()}) {
        if (const DeclTypeSpec * entityType{details->type()}) {
          if (const DerivedTypeSpec * derivedType{entityType->AsDerived()}) {
            UltimateComponentIterator ultimates{*derivedType};
            for (const auto &ultimate : ultimates) {
              if (WillDeallocatePolymorphic(ultimate, WillDeallocate)) {
                return true;
              }
            }
          }
        }
      }
    }
    return false;
  }

  // Deallocation caused by block exit
  // Allocatable entities and all of their allocatable subcomponents will be
  // deallocated.  This test is different from the other two because it does
  // not deallocate in cases where the entity itself is not allocatable but
  // has allocatable polymorphic components
  void Post(const parser::BlockConstruct &blockConstruct) {
    const auto &endBlockStmt{
        std::get<parser::Statement<parser::EndBlockStmt>>(blockConstruct.t)};
    const Scope &blockScope{context_.FindScope(endBlockStmt.source)};
    const Scope &doScope{context_.FindScope(doConcurrentSourcePosition_)};
    if (DoesScopeContain(&doScope, blockScope)) {
      for (auto &pair : blockScope) {
        Symbol &entity{*pair.second};
        if (IsAllocatable(entity) && !entity.attrs().test(Attr::SAVE) &&
            MightDeallocatePolymorphic(entity, DeallocateAll)) {
          context_.SayWithDecl(entity, endBlockStmt.source,
              "Deallocation of a polymorphic entity caused by block"
              " exit not allowed in DO CONCURRENT"_err_en_US);
        }
        // TODO: Check for deallocation of a variable with an IMPURE FINAL
        // subroutine
      }
    }
  }

  // Deallocation caused by assignment
  // Note that this case does not cause deallocation of coarray components
  void Post(const parser::AssignmentStmt &stmt) {
    const auto &variable{std::get<parser::Variable>(stmt.t)};
    if (const Symbol * entity{GetLastName(variable).symbol}) {
      if (MightDeallocatePolymorphic(*entity, DeallocateNonCoarray)) {
        context_.SayWithDecl(*entity, variable.GetSource(),
            "Deallocation of a polymorphic entity caused by "
            "assignment not allowed in DO CONCURRENT"_err_en_US);
        // TODO: Check for deallocation of a variable with an IMPURE FINAL
        // subroutine
      }
    }
  }

  // Deallocation from a DEALLOCATE statement
  // This case is different because DEALLOCATE statements deallocate both
  // ALLOCATABLE and POINTER entities
  void Post(const parser::DeallocateStmt &stmt) {
    const auto &allocateObjectList{
        std::get<std::list<parser::AllocateObject>>(stmt.t)};
    for (const auto &allocateObject : allocateObjectList) {
      const parser::Name &name{GetLastName(allocateObject)};
      if (name.symbol) {
        const Symbol &entity{*name.symbol};
        const DeclTypeSpec *entityType{entity.GetType()};
        if ((entityType && entityType->IsPolymorphic()) ||  // POINTER case
            MightDeallocatePolymorphic(entity, DeallocateAll)) {
          context_.SayWithDecl(entity, currentStatementSourcePosition_,
              "Deallocation of a polymorphic entity not allowed in DO"
              " CONCURRENT"_err_en_US);
        }
        // TODO: Check for deallocation of a variable with an IMPURE FINAL
        // subroutine
      }
    }
  }

  // C1137 -- No image control statements in a DO CONCURRENT
  void Post(const parser::ExecutableConstruct &construct) {
    if (IsImageControlStmt(construct)) {
      const parser::CharBlock statementLocation{
          GetImageControlStmtLocation(construct)};
      auto &msg{context_.Say(statementLocation,
          "An image control statement is not allowed in DO"
          " CONCURRENT"_err_en_US)};
      if (auto coarrayMsg{GetImageControlStmtCoarrayMsg(construct)}) {
        msg.Attach(statementLocation, *coarrayMsg);
      }
      msg.Attach(doConcurrentSourcePosition_, GetEnclosingDoMsg());
    }
  }

  // C1136 -- No RETURN statements in a DO CONCURRENT
  void Post(const parser::ReturnStmt &) {
    context_
        .Say(currentStatementSourcePosition_,
            "RETURN is not allowed in DO CONCURRENT"_err_en_US)
        .Attach(doConcurrentSourcePosition_, GetEnclosingDoMsg());
  }

  // C1139: call to impure procedure and ...
  // C1141: cannot call ieee_get_flag, ieee_[gs]et_halting_mode
  // It's not necessary to check the ieee_get* procedures because they're
  // not pure, and impure procedures are caught by checks for constraint C1139
  void Post(const parser::ProcedureDesignator &procedureDesignator) {
    if (auto *name{std::get_if<parser::Name>(&procedureDesignator.u)}) {
      if (name->symbol && !IsPureProcedure(*name->symbol)) {
        SayWithDo(context_, currentStatementSourcePosition_,
            "Call to an impure procedure is not allowed in DO"
            " CONCURRENT"_err_en_US,
            doConcurrentSourcePosition_);
      }
      if (name->symbol && fromScope(*name->symbol, "ieee_exceptions"s)) {
        if (name->source == "ieee_set_halting_mode") {
          SayWithDo(context_, currentStatementSourcePosition_,
              "IEEE_SET_HALTING_MODE is not allowed in DO "
              "CONCURRENT"_err_en_US,
              doConcurrentSourcePosition_);
        }
      }
    } else {
      // C1139: this a procedure component
      auto &component{std::get<parser::ProcComponentRef>(procedureDesignator.u)
                          .v.thing.component};
      if (component.symbol && !IsPureProcedure(*component.symbol)) {
        SayWithDo(context_, currentStatementSourcePosition_,
            "Call to an impure procedure component is not allowed"
            " in DO CONCURRENT"_err_en_US,
            doConcurrentSourcePosition_);
      }
    }
  }

  // 11.1.7.5, paragraph 5, no ADVANCE specifier in a DO CONCURRENT
  void Post(const parser::IoControlSpec &ioControlSpec) {
    if (auto *charExpr{
            std::get_if<parser::IoControlSpec::CharExpr>(&ioControlSpec.u)}) {
      if (std::get<parser::IoControlSpec::CharExpr::Kind>(charExpr->t) ==
          parser::IoControlSpec::CharExpr::Kind::Advance) {
        SayWithDo(context_, currentStatementSourcePosition_,
            "ADVANCE specifier is not allowed in DO"
            " CONCURRENT"_err_en_US,
            doConcurrentSourcePosition_);
      }
    }
  }

private:
  // Return the (possibly null) name of the statement
  template<typename A> static const parser::Name *MaybeGetStmtName(const A &a) {
    return common::GetPtrFromOptional(std::get<0>(a.t));
  }

  bool fromScope(const Symbol &symbol, const std::string &moduleName) {
    if (symbol.GetUltimate().owner().IsModule() &&
        symbol.GetUltimate().owner().GetName().value().ToString() ==
            moduleName) {
      return true;
    }
    return false;
  }

  std::set<parser::Label> labels_;
  parser::CharBlock currentStatementSourcePosition_;
  SemanticsContext &context_;
  parser::CharBlock doConcurrentSourcePosition_;
};  // class DoConcurrentBodyEnforce

// Class for enforcing C1130 -- in a DO CONCURRENT with DEFAULT(NONE),
// variables from enclosing scopes must have their locality specified
class DoConcurrentVariableEnforce {
public:
  DoConcurrentVariableEnforce(
      SemanticsContext &context, parser::CharBlock doConcurrentSourcePosition)
    : context_{context},
      doConcurrentSourcePosition_{doConcurrentSourcePosition},
      blockScope_{context.FindScope(doConcurrentSourcePosition_)} {}

  template<typename T> bool Pre(const T &) { return true; }
  template<typename T> void Post(const T &) {}

  // Check to see if the name is a variable from an enclosing scope
  void Post(const parser::Name &name) {
    if (const Symbol * symbol{name.symbol}) {
      if (IsVariableName(*symbol)) {
        const Scope &variableScope{symbol->owner()};
        if (DoesScopeContain(&variableScope, blockScope_)) {
          context_.SayWithDecl(*symbol, name.source,
              "Variable '%s' from an enclosing scope referenced in DO "
              "CONCURRENT with DEFAULT(NONE) must appear in a "
              "locality-spec"_err_en_US,
              symbol->name());
        }
      }
    }
  }

private:
  SemanticsContext &context_;
  parser::CharBlock doConcurrentSourcePosition_;
  const Scope &blockScope_;
};  // class DoConcurrentVariableEnforce

// Find a DO statement and enforce semantics checks on its body
class DoContext {
public:
  DoContext(SemanticsContext &context) : context_{context} {}

  // Mark this DO construct as a point of definition for the DO variables
  // or index-names it contains.  If they're already defined, emit an error
  // message.  We need to remember both the variable and the source location of
  // the variable in the DO construct so that we can remove it when we leave
  // the DO construct and use its location in error messages.
  void DefineDoVariables(const parser::DoConstruct &doConstruct) {
    if (doConstruct.IsDoNormal()) {
      context_.ActivateDoVariable(GetDoVariable(doConstruct));
    } else if (doConstruct.IsDoConcurrent()) {
      if (const auto &loopControl{doConstruct.GetLoopControl()}) {
        const auto &controls{GetControls(*loopControl)};
        for (const parser::ConcurrentControl &control : controls) {
          context_.ActivateDoVariable(std::get<parser::Name>(control.t));
        }
      }
    }
  }

  // Called at the end of a DO construct to deactivate the DO construct
  void ResetDoVariables(const parser::DoConstruct &doConstruct) {
    if (doConstruct.IsDoNormal()) {
      context_.DeactivateDoVariable(GetDoVariable(doConstruct));
    } else if (doConstruct.IsDoConcurrent()) {
      if (const auto &loopControl{doConstruct.GetLoopControl()}) {
        const auto &controls{GetControls(*loopControl)};
        for (const parser::ConcurrentControl &control : controls) {
          context_.DeactivateDoVariable(std::get<parser::Name>(control.t));
        }
      }
    }
  }

  void Check(const parser::DoConstruct &doConstruct) {
    if (doConstruct.IsDoConcurrent()) {
      CheckDoConcurrent(doConstruct);
      return;
    }
    if (doConstruct.IsDoNormal()) {
      CheckDoNormal(doConstruct);
      return;
    }
    // TODO: handle the other cases
  }

private:
  void SayBadDoControl(parser::CharBlock sourceLocation) {
    context_.Say(sourceLocation, "DO controls should be INTEGER"_err_en_US);
  }

  void CheckDoControl(const parser::CharBlock &sourceLocation, bool isReal) {
    const bool warn{context_.warnOnNonstandardUsage() ||
        context_.ShouldWarn(common::LanguageFeature::RealDoControls)};
    if (isReal && !warn) {
      // No messages for the default case
    } else if (isReal && warn) {
      context_.Say(sourceLocation, "DO controls should be INTEGER"_en_US);
    } else {
      SayBadDoControl(sourceLocation);
    }
  }

  void CheckDoVariable(const parser::ScalarName &scalarName) {
    const parser::CharBlock &sourceLocation{scalarName.thing.source};
    if (const Symbol * symbol{scalarName.thing.symbol}) {
      if (!IsVariableName(*symbol)) {
        context_.Say(
            sourceLocation, "DO control must be an INTEGER variable"_err_en_US);
      } else {
        const DeclTypeSpec *symType{symbol->GetType()};
        if (!symType) {
          SayBadDoControl(sourceLocation);
        } else {
          if (!symType->IsNumeric(TypeCategory::Integer)) {
            CheckDoControl(
                sourceLocation, symType->IsNumeric(TypeCategory::Real));
          }
        }
      }  // No messages for INTEGER
    }
  }

  // Semantic checks for the limit and step expressions
  void CheckDoExpression(const parser::ScalarExpr &scalarExpression) {
    if (const SomeExpr * expr{GetExpr(scalarExpression)}) {
      if (!ExprHasTypeCategory(*expr, TypeCategory::Integer)) {
        // No warnings or errors for type INTEGER
        const parser::CharBlock &loc{scalarExpression.thing.value().source};
        CheckDoControl(loc, ExprHasTypeCategory(*expr, TypeCategory::Real));
      }
    }
  }

  void CheckDoNormal(const parser::DoConstruct &doConstruct) {
    // C1120 -- types of DO variables must be INTEGER, extended by allowing
    // REAL and DOUBLE PRECISION
    const Bounds &bounds{GetBounds(doConstruct)};
    CheckDoVariable(bounds.name);
    CheckDoExpression(bounds.lower);
    CheckDoExpression(bounds.upper);
    if (bounds.step) {
      CheckDoExpression(*bounds.step);
      if (IsZero(*bounds.step)) {
        context_.Say(bounds.step->thing.value().source,
            "DO step expression should not be zero"_en_US);
      }
    }
  }

  void CheckDoConcurrent(const parser::DoConstruct &doConstruct) {
    auto &doStmt{
        std::get<parser::Statement<parser::NonLabelDoStmt>>(doConstruct.t)};
    currentStatementSourcePosition_ = doStmt.source;

    const parser::Block &block{std::get<parser::Block>(doConstruct.t)};
    DoConcurrentBodyEnforce doConcurrentBodyEnforce{context_, doStmt.source};
    parser::Walk(block, doConcurrentBodyEnforce);

    LabelEnforce doConcurrentLabelEnforce{context_,
        doConcurrentBodyEnforce.labels(), currentStatementSourcePosition_,
        "DO CONCURRENT"};
    parser::Walk(block, doConcurrentLabelEnforce);

    const auto &loopControl{
        std::get<std::optional<parser::LoopControl>>(doStmt.statement.t)};
    const auto &concurrent{
        std::get<parser::LoopControl::Concurrent>(loopControl->u)};
    CheckConcurrentLoopControl(concurrent, block);
  }

  // Return a set of symbols whose names are in a Local locality-spec.  Look
  // the names up in the scope that encloses the DO construct to avoid getting
  // the local versions of them.  Then follow the host-, use-, and
  // construct-associations to get the root symbols
  SymbolSet GatherLocals(
      const std::list<parser::LocalitySpec> &localitySpecs) const {
    SymbolSet symbols;
    const Scope &parentScope{
        context_.FindScope(currentStatementSourcePosition_).parent()};
    // Loop through the LocalitySpec::Local locality-specs
    for (const auto &ls : localitySpecs) {
      if (const auto *names{std::get_if<parser::LocalitySpec::Local>(&ls.u)}) {
        // Loop through the names in the Local locality-spec getting their
        // symbols
        for (const parser::Name &name : names->v) {
          if (const Symbol * symbol{parentScope.FindSymbol(name.source)}) {
            if (const Symbol * root{GetAssociationRoot(*symbol)}) {
              symbols.insert(*root);
            }
          }
        }
      }
    }
    return symbols;
  }

  static SymbolSet GatherSymbolsFromExpression(const parser::Expr &expression) {
    SymbolSet result;
    if (const auto *expr{GetExpr(expression)}) {
      for (const Symbol &symbol : evaluate::CollectSymbols(*expr)) {
        if (const Symbol * root{GetAssociationRoot(symbol)}) {
          result.insert(*root);
        }
      }
    }
    return result;
  }

  // C1121 - procedures in mask must be pure
  void CheckMaskIsPure(const parser::ScalarLogicalExpr &mask) const {
    SymbolSet references{GatherSymbolsFromExpression(mask.thing.thing.value())};
    for (const Symbol &ref : references) {
      if (IsProcedure(ref) && !IsPureProcedure(ref)) {
        context_.SayWithDecl(ref, currentStatementSourcePosition_,
            "Concurrent-header mask expression cannot reference an impure"
            " procedure"_err_en_US);
        return;
      }
    }
  }

  void CheckNoCollisions(const SymbolSet &refs, const SymbolSet &uses,
      parser::MessageFixedText &&errorMessage,
      const parser::CharBlock &refPosition) const {
    for (const Symbol &ref : refs) {
      if (uses.find(ref) != uses.end()) {
        context_.SayWithDecl(
            ref, refPosition, std::move(errorMessage), ref.name());
        return;
      }
    }
  }

  void HasNoReferences(
      const SymbolSet &indexNames, const parser::ScalarIntExpr &expr) const {
    CheckNoCollisions(GatherSymbolsFromExpression(expr.thing.thing.value()),
        indexNames,
        "concurrent-control expression references index-name '%s'"_err_en_US,
        expr.thing.thing.value().source);
  }

  // C1129, names in local locality-specs can't be in mask expressions
  void CheckMaskDoesNotReferenceLocal(
      const parser::ScalarLogicalExpr &mask, const SymbolSet &localVars) const {
    CheckNoCollisions(GatherSymbolsFromExpression(mask.thing.thing.value()),
        localVars,
        "concurrent-header mask-expr references variable '%s'"
        " in LOCAL locality-spec"_err_en_US,
        mask.thing.thing.value().source);
  }

  // C1129, names in local locality-specs can't be in limit or step
  // expressions
  void CheckExprDoesNotReferenceLocal(
      const parser::ScalarIntExpr &expr, const SymbolSet &localVars) const {
    CheckNoCollisions(GatherSymbolsFromExpression(expr.thing.thing.value()),
        localVars,
        "concurrent-header expression references variable '%s'"
        " in LOCAL locality-spec"_err_en_US,
        expr.thing.thing.value().source);
  }

  // C1130, DEFAULT(NONE) locality requires names to be in locality-specs to
  // be used in the body of the DO loop
  void CheckDefaultNoneImpliesExplicitLocality(
      const std::list<parser::LocalitySpec> &localitySpecs,
      const parser::Block &block) const {
    bool hasDefaultNone{false};
    for (auto &ls : localitySpecs) {
      if (std::holds_alternative<parser::LocalitySpec::DefaultNone>(ls.u)) {
        if (hasDefaultNone) {
          // C1127, you can only have one DEFAULT(NONE)
          context_.Say(currentStatementSourcePosition_,
              "Only one DEFAULT(NONE) may appear"_en_US);
          break;
        }
        hasDefaultNone = true;
      }
    }
    if (hasDefaultNone) {
      DoConcurrentVariableEnforce doConcurrentVariableEnforce{
          context_, currentStatementSourcePosition_};
      parser::Walk(block, doConcurrentVariableEnforce);
    }
  }

  // C1123, concurrent limit or step expressions can't reference index-names
  void CheckConcurrentHeader(const parser::ConcurrentHeader &header) const {
    auto &controls{std::get<std::list<parser::ConcurrentControl>>(header.t)};
    SymbolSet indexNames;
    for (const auto &c : controls) {
      const auto &indexName{std::get<parser::Name>(c.t)};
      if (indexName.symbol) {
        indexNames.insert(*indexName.symbol);
      }
    }
    if (!indexNames.empty()) {
      for (const auto &c : controls) {
        HasNoReferences(indexNames, std::get<1>(c.t));
        HasNoReferences(indexNames, std::get<2>(c.t));
        if (const auto &expr{
                std::get<std::optional<parser::ScalarIntExpr>>(c.t)}) {
          HasNoReferences(indexNames, *expr);
          if (IsZero(*expr)) {
            context_.Say(expr->thing.thing.value().source,
                "DO CONCURRENT step expression should not be zero"_err_en_US);
          }
        }
      }
    }
  }

  void CheckLocalitySpecs(const parser::LoopControl::Concurrent &concurrent,
      const parser::Block &block) const {
    const auto &header{std::get<parser::ConcurrentHeader>(concurrent.t)};
    const auto &controls{
        std::get<std::list<parser::ConcurrentControl>>(header.t)};
    const auto &localitySpecs{
        std::get<std::list<parser::LocalitySpec>>(concurrent.t)};
    if (!localitySpecs.empty()) {
      const SymbolSet &localVars{GatherLocals(localitySpecs)};
      for (const auto &c : controls) {
        CheckExprDoesNotReferenceLocal(std::get<1>(c.t), localVars);
        CheckExprDoesNotReferenceLocal(std::get<2>(c.t), localVars);
        if (const auto &expr{
                std::get<std::optional<parser::ScalarIntExpr>>(c.t)}) {
          CheckExprDoesNotReferenceLocal(*expr, localVars);
        }
      }
      if (const auto &mask{
              std::get<std::optional<parser::ScalarLogicalExpr>>(header.t)}) {
        CheckMaskDoesNotReferenceLocal(*mask, localVars);
      }
      CheckDefaultNoneImpliesExplicitLocality(localitySpecs, block);
    }
  }

  // check constraints [C1121 .. C1130]
  void CheckConcurrentLoopControl(
      const parser::LoopControl::Concurrent &concurrent,
      const parser::Block &block) const {

    const auto &header{std::get<parser::ConcurrentHeader>(concurrent.t)};
    const auto &mask{
        std::get<std::optional<parser::ScalarLogicalExpr>>(header.t)};
    if (mask) {
      CheckMaskIsPure(*mask);
    }
    CheckConcurrentHeader(header);
    CheckLocalitySpecs(concurrent, block);
  }

  SemanticsContext &context_;
  parser::CharBlock currentStatementSourcePosition_;
};  // class DoContext

void DoChecker::Enter(const parser::DoConstruct &doConstruct) {
  DoContext doContext{context_};
  doContext.DefineDoVariables(doConstruct);
}

void DoChecker::Leave(const parser::DoConstruct &doConstruct) {
  DoContext doContext{context_};
  doContext.Check(doConstruct);
  doContext.ResetDoVariables(doConstruct);
}

// Return the (possibly null) name of the ConstructNode
static const parser::Name *MaybeGetNodeName(const ConstructNode &construct) {
  return std::visit(
      [&](const auto &x) { return MaybeGetConstructName(*x); }, construct);
}

template<typename A> static parser::CharBlock GetConstructPosition(const A &a) {
  return std::get<0>(a.t).source;
}

static parser::CharBlock GetNodePosition(const ConstructNode &construct) {
  return std::visit(
      [&](const auto &x) { return GetConstructPosition(*x); }, construct);
}

void DoChecker::SayBadLeave(StmtType stmtType, const char *enclosingStmtName,
    const ConstructNode &construct) const {
  context_
      .Say("%s must not leave a %s statement"_err_en_US, EnumToString(stmtType),
          enclosingStmtName)
      .Attach(GetNodePosition(construct), "The construct that was left"_en_US);
}

static const parser::DoConstruct *MaybeGetDoConstruct(
    const ConstructNode &construct) {
  if (const auto *doNode{
          std::get_if<const parser::DoConstruct *>(&construct)}) {
    return *doNode;
  } else {
    return nullptr;
  }
}

static bool ConstructIsDoConcurrent(const ConstructNode &construct) {
  const parser::DoConstruct *doConstruct{MaybeGetDoConstruct(construct)};
  return doConstruct && doConstruct->IsDoConcurrent();
}

// Check that CYCLE and EXIT statements do not cause flow of control to
// leave DO CONCURRENT, CRITICAL, or CHANGE TEAM constructs.
void DoChecker::CheckForBadLeave(
    StmtType stmtType, const ConstructNode &construct) const {
  std::visit(
      common::visitors{
          [&](const parser::DoConstruct *doConstructPtr) {
            if (doConstructPtr->IsDoConcurrent()) {
              // C1135 and C1167 -- CYCLE and EXIT statements can't leave a
              // DO CONCURRENT
              SayBadLeave(stmtType, "DO CONCURRENT", construct);
            }
          },
          [&](const parser::CriticalConstruct *) {
            // C1135 and C1168 -- similarly, for CRITICAL
            SayBadLeave(stmtType, "CRITICAL", construct);
          },
          [&](const parser::ChangeTeamConstruct *) {
            // C1135 and C1168 -- similarly, for CHANGE TEAM
            SayBadLeave(stmtType, "CHANGE TEAM", construct);
          },
          [](const auto *) {},
      },
      construct);
}

static bool StmtMatchesConstruct(const parser::Name *stmtName,
    StmtType stmtType, const parser::Name *constructName,
    const ConstructNode &construct) {
  bool inDoConstruct{MaybeGetDoConstruct(construct)};
  if (!stmtName) {
    return inDoConstruct;  // Unlabeled statements match all DO constructs
  } else if (constructName && constructName->source == stmtName->source) {
    return stmtType == StmtType::EXIT || inDoConstruct;
  } else {
    return false;
  }
}

// C1167 Can't EXIT from a DO CONCURRENT
void DoChecker::CheckDoConcurrentExit(
    StmtType stmtType, const ConstructNode &construct) const {
  if (stmtType == StmtType::EXIT && ConstructIsDoConcurrent(construct)) {
    SayBadLeave(StmtType::EXIT, "DO CONCURRENT", construct);
  }
}

// Check nesting violations for a CYCLE or EXIT statement.  Loop up the
// nesting levels looking for a construct that matches the CYCLE or EXIT
// statment.  At every construct, check for a violation.  If we find a match
// without finding a violation, the check is complete.
void DoChecker::CheckNesting(
    StmtType stmtType, const parser::Name *stmtName) const {
  const ConstructStack &stack{context_.constructStack()};
  for (auto iter{stack.cend()}; iter-- != stack.cbegin();) {
    const ConstructNode &construct{*iter};
    const parser::Name *constructName{MaybeGetNodeName(construct)};
    if (StmtMatchesConstruct(stmtName, stmtType, constructName, construct)) {
      CheckDoConcurrentExit(stmtType, construct);
      return;  // We got a match, so we're finished checking
    }
    CheckForBadLeave(stmtType, construct);
  }

  // We haven't found a match in the enclosing constructs
  if (stmtType == StmtType::EXIT) {
    context_.Say("No matching construct for EXIT statement"_err_en_US);
  } else {
    context_.Say("No matching DO construct for CYCLE statement"_err_en_US);
  }
}

// C1135 -- Nesting for CYCLE statements
void DoChecker::Enter(const parser::CycleStmt &cycleStmt) {
  CheckNesting(StmtType::CYCLE, common::GetPtrFromOptional(cycleStmt.v));
}

// C1167 and C1168 -- Nesting for EXIT statements
void DoChecker::Enter(const parser::ExitStmt &exitStmt) {
  CheckNesting(StmtType::EXIT, common::GetPtrFromOptional(exitStmt.v));
}

void DoChecker::Leave(const parser::AssignmentStmt &stmt) {
  const auto &variable{std::get<parser::Variable>(stmt.t)};
  context_.CheckDoVarRedefine(variable);
}

static void CheckIfArgIsDoVar(const evaluate::ActualArgument &arg,
    const parser::CharBlock location, SemanticsContext &context) {
  common::Intent intent{arg.dummyIntent()};
  if (intent == common::Intent::Out || intent == common::Intent::InOut) {
    if (const SomeExpr * argExpr{arg.UnwrapExpr()}) {
      if (const Symbol * var{evaluate::UnwrapWholeSymbolDataRef(*argExpr)}) {
        if (intent == common::Intent::Out) {
          context.CheckDoVarRedefine(location, *var);
        } else {
          context.WarnDoVarRedefine(location, *var);  // INTENT(INOUT)
        }
      }
    }
  }
}

// Check to see if a DO variable is being passed as an actual argument to a
// dummy argument whose intent is OUT or INOUT.  To do this, we need to find
// the expressions for actual arguments which contain DO variables.  We get the
// intents of the dummy arguments from the ProcedureRef in the "typedCall"
// field of the CallStmt which was filled in during expression checking.  At
// the same time, we need to iterate over the parser::Expr versions of the
// actual arguments to get their source locations of the arguments for the
// messages.
void DoChecker::Leave(const parser::CallStmt &callStmt) {
  if (const auto &typedCall{callStmt.typedCall}) {
    const auto &parsedArgs{
        std::get<std::list<parser::ActualArgSpec>>(callStmt.v.t)};
    auto parsedArgIter{parsedArgs.begin()};
    const evaluate::ActualArguments &checkedArgs{typedCall->arguments()};
    for (const auto &checkedOptionalArg : checkedArgs) {
      if (parsedArgIter == parsedArgs.end()) {
        break;  // No more parsed arguments, we're done.
      }
      const auto &parsedArg{std::get<parser::ActualArg>(parsedArgIter->t)};
      ++parsedArgIter;
      if (checkedOptionalArg) {
        const evaluate::ActualArgument &checkedArg{*checkedOptionalArg};
        if (const auto *parsedExpr{
                std::get_if<common::Indirection<parser::Expr>>(&parsedArg.u)}) {
          CheckIfArgIsDoVar(checkedArg, parsedExpr->value().source, context_);
        }
      }
    }
  }
}

void DoChecker::Leave(const parser::ConnectSpec &connectSpec) {
  const auto *newunit{
      std::get_if<parser::ConnectSpec::Newunit>(&connectSpec.u)};
  if (newunit) {
    context_.CheckDoVarRedefine(newunit->v.thing.thing);
  }
}

using ActualArgumentSet = std::set<evaluate::ActualArgumentRef>;

struct CollectActualArgumentsHelper
  : public evaluate::SetTraverse<CollectActualArgumentsHelper,
        ActualArgumentSet> {
  using Base = SetTraverse<CollectActualArgumentsHelper, ActualArgumentSet>;
  CollectActualArgumentsHelper() : Base{*this} {}
  using Base::operator();
  ActualArgumentSet operator()(const evaluate::ActualArgument &arg) const {
    return ActualArgumentSet{arg};
  }
};

template<typename A> ActualArgumentSet CollectActualArguments(const A &x) {
  return CollectActualArgumentsHelper{}(x);
}

template ActualArgumentSet CollectActualArguments(const SomeExpr &);

void DoChecker::Leave(const parser::Expr &parsedExpr) {
  if (const SomeExpr * expr{GetExpr(parsedExpr)}) {
    ActualArgumentSet argSet{CollectActualArguments(*expr)};
    for (const evaluate::ActualArgumentRef &argRef : argSet) {
      CheckIfArgIsDoVar(*argRef, parsedExpr.source, context_);
    }
  }
}

void DoChecker::Leave(const parser::InquireSpec &inquireSpec) {
  const auto *intVar{std::get_if<parser::InquireSpec::IntVar>(&inquireSpec.u)};
  if (intVar) {
    const auto &scalar{std::get<parser::ScalarIntVariable>(intVar->t)};
    context_.CheckDoVarRedefine(scalar.thing.thing);
  }
}

void DoChecker::Leave(const parser::IoControlSpec &ioControlSpec) {
  const auto *size{std::get_if<parser::IoControlSpec::Size>(&ioControlSpec.u)};
  if (size) {
    context_.CheckDoVarRedefine(size->v.thing.thing);
  }
}

void DoChecker::Leave(const parser::OutputImpliedDo &outputImpliedDo) {
  const auto &control{std::get<parser::IoImpliedDoControl>(outputImpliedDo.t)};
  const parser::Name &name{control.name.thing.thing};
  context_.CheckDoVarRedefine(name.source, *name.symbol);
}

void DoChecker::Leave(const parser::StatVariable &statVariable) {
  context_.CheckDoVarRedefine(statVariable.v.thing.thing);
}

}  // namespace Fortran::semantics
