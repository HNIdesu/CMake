/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmGeneratorExpressionDAGChecker.h"

#include <sstream>
#include <utility>

#include <cm/optional>
#include <cm/string_view>
#include <cmext/string_view>

#include "cmGeneratorExpressionContext.h"
#include "cmGeneratorExpressionEvaluator.h"
#include "cmGeneratorTarget.h"
#include "cmLocalGenerator.h"
#include "cmMessageType.h"
#include "cmStringAlgorithms.h"
#include "cmake.h"

cmGeneratorExpressionDAGChecker::cmGeneratorExpressionDAGChecker(
  cmGeneratorTarget const* target, std::string property,
  GeneratorExpressionContent const* content,
  cmGeneratorExpressionDAGChecker* parent, cmLocalGenerator const* contextLG,
  std::string const& contextConfig, cmListFileBacktrace backtrace,
  ComputingLinkLibraries computingLinkLibraries)
  : Parent(parent)
  , Top(parent ? parent->Top : this)
  , Target(target)
  , Property(std::move(property))
  , Content(content)
  , Backtrace(std::move(backtrace))
  , ComputingLinkLibraries_(computingLinkLibraries)
{
  if (parent) {
    this->TopIsTransitiveProperty = parent->TopIsTransitiveProperty;
  } else {
    this->TopIsTransitiveProperty =
      this->Target
        ->IsTransitiveProperty(this->Property, contextLG, contextConfig, this)
        .has_value();
  }

  this->CheckResult = this->CheckGraph();

  if (this->CheckResult == DAG && this->EvaluatingTransitiveProperty()) {
    auto const* top = this->Top;
    auto it = top->Seen.find(this->Target);
    if (it != top->Seen.end()) {
      std::set<std::string> const& propSet = it->second;
      if (propSet.find(this->Property) != propSet.end()) {
        this->CheckResult = ALREADY_SEEN;
        return;
      }
    }
    top->Seen[this->Target].insert(this->Property);
  }
}

cmGeneratorExpressionDAGChecker::Result
cmGeneratorExpressionDAGChecker::Check() const
{
  return this->CheckResult;
}

void cmGeneratorExpressionDAGChecker::ReportError(
  cmGeneratorExpressionContext* context, std::string const& expr)
{
  if (this->CheckResult == DAG) {
    return;
  }

  context->HadError = true;
  if (context->Quiet) {
    return;
  }

  cmGeneratorExpressionDAGChecker const* parent = this->Parent;

  if (parent && !parent->Parent) {
    std::ostringstream e;
    e << "Error evaluating generator expression:\n"
      << "  " << expr << "\n"
      << "Self reference on target \"" << context->HeadTarget->GetName()
      << "\".\n";
    context->LG->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                                  e.str(), parent->Backtrace);
    return;
  }

  {
    std::ostringstream e;
    /* clang-format off */
  e << "Error evaluating generator expression:\n"
    << "  " << expr << "\n"
    << "Dependency loop found.";
    /* clang-format on */
    context->LG->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                                  e.str(), context->Backtrace);
  }

  int loopStep = 1;
  while (parent) {
    std::ostringstream e;
    e << "Loop step " << loopStep << "\n"
      << "  "
      << (parent->Content ? parent->Content->GetOriginalExpression() : expr)
      << "\n";
    context->LG->GetCMakeInstance()->IssueMessage(MessageType::FATAL_ERROR,
                                                  e.str(), parent->Backtrace);
    parent = parent->Parent;
    ++loopStep;
  }
}

cmGeneratorExpressionDAGChecker::Result
cmGeneratorExpressionDAGChecker::CheckGraph() const
{
  cmGeneratorExpressionDAGChecker const* parent = this->Parent;
  while (parent) {
    if (this->Target == parent->Target && this->Property == parent->Property) {
      return (parent == this->Parent) ? SELF_REFERENCE : CYCLIC_REFERENCE;
    }
    parent = parent->Parent;
  }
  return DAG;
}

bool cmGeneratorExpressionDAGChecker::GetTransitivePropertiesOnly() const
{
  return this->Top->TransitivePropertiesOnly;
}

bool cmGeneratorExpressionDAGChecker::GetTransitivePropertiesOnlyCMP0131()
  const
{
  return this->Top->CMP0131;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingTransitiveProperty() const
{
  return this->TopIsTransitiveProperty;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingGenexExpression() const
{
  // Corresponds to GenexEvaluator::EvaluateExpression.
  return cmHasLiteralPrefix(this->Property, "TARGET_GENEX_EVAL:") ||
    cmHasLiteralPrefix(this->Property, "GENEX_EVAL:");
}

bool cmGeneratorExpressionDAGChecker::EvaluatingPICExpression() const
{
  // Corresponds to checkInterfacePropertyCompatibility's special case
  // that evaluates the value of POSITION_INDEPENDENT_CODE as a genex.
  return this->Top->Property == "INTERFACE_POSITION_INDEPENDENT_CODE";
}

bool cmGeneratorExpressionDAGChecker::EvaluatingCompileExpression() const
{
  cm::string_view property(this->Top->Property);

  return property == "INCLUDE_DIRECTORIES"_s ||
    property == "COMPILE_DEFINITIONS"_s || property == "COMPILE_OPTIONS"_s;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingSources() const
{
  return this->Property == "SOURCES"_s ||
    this->Property == "INTERFACE_SOURCES"_s;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingLinkExpression() const
{
  cm::string_view property(this->Top->Property);

  return property == "LINK_DIRECTORIES"_s || property == "LINK_OPTIONS"_s ||
    property == "LINK_DEPENDS"_s || property == "LINK_LIBRARY_OVERRIDE"_s ||
    property == "LINKER_TYPE"_s;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingLinkOptionsExpression() const
{
  cm::string_view property(this->Top->Property);

  return property == "LINK_OPTIONS"_s || property == "LINKER_TYPE"_s;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingLinkerLauncher() const
{
  cm::string_view property(this->Top->Property);

  return property.length() > cmStrLen("_LINKER_LAUNCHER") &&
    property.substr(property.length() - cmStrLen("_LINKER_LAUNCHER")) ==
    "_LINKER_LAUNCHER"_s;
}

bool cmGeneratorExpressionDAGChecker::IsComputingLinkLibraries() const
{
  return this->Top->ComputingLinkLibraries_ == ComputingLinkLibraries::Yes;
}

bool cmGeneratorExpressionDAGChecker::EvaluatingLinkLibraries(
  cmGeneratorTarget const* tgt, ForGenex genex) const
{
  auto const* top = this->Top;

  cm::string_view prop(top->Property);

  if (tgt) {
    return top->Target == tgt && prop == "LINK_LIBRARIES"_s;
  }

  auto result = prop == "LINK_LIBRARIES"_s ||
    prop == "INTERFACE_LINK_LIBRARIES"_s ||
    prop == "INTERFACE_LINK_LIBRARIES_DIRECT"_s ||
    prop == "LINK_INTERFACE_LIBRARIES"_s ||
    prop == "IMPORTED_LINK_INTERFACE_LIBRARIES"_s ||
    cmHasLiteralPrefix(prop, "LINK_INTERFACE_LIBRARIES_") ||
    cmHasLiteralPrefix(prop, "IMPORTED_LINK_INTERFACE_LIBRARIES_");

  return genex == ForGenex::LINK_LIBRARY || genex == ForGenex::LINK_GROUP
    ? result
    : (result || prop == "INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE"_s);
}

cmGeneratorTarget const* cmGeneratorExpressionDAGChecker::TopTarget() const
{
  return this->Top->Target;
}
