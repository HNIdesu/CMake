/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmCTestBuildHandler.h"

#include <cstdlib>
#include <memory>
#include <ratio>
#include <set>
#include <utility>

#include <cm/string_view>
#include <cmext/algorithm>

#include <cm3p/json/value.h>
#include <cm3p/uv.h>

#include "cmsys/Directory.hxx"
#include "cmsys/FStream.hxx"

#include "cmCTest.h"
#include "cmCTestLaunchReporter.h"
#include "cmDuration.h"
#include "cmFileTimeCache.h"
#include "cmGeneratedFileStream.h"
#include "cmInstrumentation.h"
#include "cmInstrumentationQuery.h"
#include "cmJSONState.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmProcessOutput.h"
#include "cmStringAlgorithms.h"
#include "cmStringReplaceHelper.h"
#include "cmSystemTools.h"
#include "cmUVHandlePtr.h"
#include "cmUVProcessChain.h"
#include "cmUVStream.h"
#include "cmValue.h"
#include "cmXMLWriter.h"

static char const* cmCTestErrorMatches[] = {
  "^[Bb]us [Ee]rror", // noqa: spellcheck disable-line
  "^[Ss]egmentation [Vv]iolation",
  "^[Ss]egmentation [Ff]ault",
  ":.*[Pp]ermission [Dd]enied",
  "([^ :]+):([0-9]+): ([^ \\t])",
  "([^:]+): error[ \\t]*[0-9]+[ \\t]*:",
  "^Error ([0-9]+):",
  "^Fatal",
  "^Error: ",
  "^Error ",
  "[0-9] ERROR: ",
  R"(^"[^"]+", line [0-9]+: [^Ww])",
  "^cc[^C]*CC: ERROR File = ([^,]+), Line = ([0-9]+)",
  "^ld([^:])*:([ \\t])*ERROR([^:])*:",
  R"(^ild:([ \t])*\(undefined symbol\))",
  "([^ :]+) : (error|fatal error|catastrophic error)",
  "([^:]+): (Error:|error|undefined reference|multiply defined)",
  R"(([^:]+)\(([^\)]+)\) ?: (error|fatal error|catastrophic error))",
  "^fatal error C[0-9]+:",
  ": syntax error ",
  "^collect2: ld returned 1 exit status",
  "ld terminated with signal",
  "Unsatisfied symbol",
  "^Unresolved:",
  "Undefined symbol",
  "^Undefined[ \\t]+first referenced",
  "^CMake Error.*:",
  ":[ \\t]cannot find",
  ":[ \\t]can't find",
  R"(: \*\*\* No rule to make target [`'].*\'.  Stop)",
  R"(: \*\*\* No targets specified and no makefile found)",
  ": Invalid loader fixup for symbol",
  ": Invalid fixups exist",
  ": Can't find library for",
  ": internal link edit command failed",
  ": Unrecognized option [`'].*\\'",
  R"(", line [0-9]+\.[0-9]+: [0-9]+-[0-9]+ \([^WI]\))",
  "ld: 0706-006 Cannot find or open library file: -l ",
  "ild: \\(argument error\\) can't find library argument ::",
  "^could not be found and will not be loaded.",
  "s:616 string too big",
  "make: Fatal error: ",
  "ld: 0711-993 Error occurred while writing to the output file:",
  "ld: fatal: ",
  "final link failed:",
  R"(make: \*\*\*.*Error)",
  R"(make\[.*\]: \*\*\*.*Error)",
  R"(\*\*\* Error code)",
  "nternal error:",
  R"(Makefile:[0-9]+: \*\*\* .*  Stop\.)",
  ": No such file or directory",
  ": Invalid argument",
  "^The project cannot be built\\.",
  "^\\[ERROR\\]",
  "^Command .* failed with exit code",
  "lcc: \"([^\"]+)\", (line|строка) ([0-9]+): (error|ошибка)",
  nullptr
};

static char const* cmCTestErrorExceptions[] = {
  "instantiated from ",
  "candidates are:",
  ": warning",
  ": WARNING",
  ": \\(Warning\\)",
  ": note",
  "Note:",
  "makefile:",
  "Makefile:",
  ":[ \\t]+Where:",
  "([^ :]+):([0-9]+): Warning",
  "------ Build started: .* ------",
  nullptr
};

static char const* cmCTestWarningMatches[] = {
  "([^ :]+):([0-9]+): warning:",
  "([^ :]+):([0-9]+): note:",
  "^cc[^C]*CC: WARNING File = ([^,]+), Line = ([0-9]+)",
  "^ld([^:])*:([ \\t])*WARNING([^:])*:",
  "([^:]+): warning ([0-9]+):",
  R"(^"[^"]+", line [0-9]+: [Ww](arning|arnung))",
  "([^:]+): warning[ \\t]*[0-9]+[ \\t]*:",
  "^(Warning|Warnung) ([0-9]+):",
  "^(Warning|Warnung)[ :]",
  "WARNING: ",
  "([^ :]+) : warning",
  "([^:]+): warning",
  R"(", line [0-9]+\.[0-9]+: [0-9]+-[0-9]+ \([WI]\))",
  "^cxx: Warning:",
  ".*file: .* has no symbols",
  "([^ :]+):([0-9]+): (Warning|Warnung)",
  "\\([0-9]*\\): remark #[0-9]*",
  R"(".*", line [0-9]+: remark\([0-9]*\):)",
  "cc-[0-9]* CC: REMARK File = .*, Line = [0-9]*",
  "^CMake Warning.*:",
  "^\\[WARNING\\]",
  "lcc: \"([^\"]+)\", (line|строка) ([0-9]+): (warning|предупреждение)",
  nullptr
};

static char const* cmCTestWarningExceptions[] = {
  R"(/usr/.*/X11/Xlib\.h:[0-9]+: war.*: ANSI C\+\+ forbids declaration)",
  R"(/usr/.*/X11/Xutil\.h:[0-9]+: war.*: ANSI C\+\+ forbids declaration)",
  R"(/usr/.*/X11/XResource\.h:[0-9]+: war.*: ANSI C\+\+ forbids declaration)",
  "WARNING 84 :",
  "WARNING 47 :",
  "makefile:",
  "Makefile:",
  "warning:  Clock skew detected.  Your build may be incomplete.",
  "/usr/openwin/include/GL/[^:]+:",
  "bind_at_load",
  "XrmQGetResource",
  "IceFlush",
  "warning LNK4089: all references to [^ \\t]+ discarded by .OPT:REF",
  "ld32: WARNING 85: definition of dataKey in",
  "cc: warning 422: Unknown option \"\\+b",
  "_with_warning_C",
  nullptr
};

struct cmCTestBuildCompileErrorWarningRex
{
  char const* RegularExpressionString;
  int FileIndex;
  int LineIndex;
};

static cmCTestBuildCompileErrorWarningRex cmCTestWarningErrorFileLine[] = {
  { "^Warning W[0-9]+ ([a-zA-Z.\\:/0-9_+ ~-]+) ([0-9]+):", 1, 2 },
  { "^([a-zA-Z./0-9_+ ~-]+):([0-9]+):", 1, 2 },
  { R"(^([a-zA-Z.\:/0-9_+ ~-]+)\(([0-9]+)\))", 1, 2 },
  { R"(^[0-9]+>([a-zA-Z.\:/0-9_+ ~-]+)\(([0-9]+)\))", 1, 2 },
  { "^([a-zA-Z./0-9_+ ~-]+)\\(([0-9]+)\\)", 1, 2 },
  { "\"([a-zA-Z./0-9_+ ~-]+)\", line ([0-9]+)", 1, 2 },
  { "File = ([a-zA-Z./0-9_+ ~-]+), Line = ([0-9]+)", 1, 2 },
  { "lcc: \"([^\"]+)\", (line|строка) ([0-9]+): "
    "(error|ошибка|warning|предупреждение)",
    1, 3 },
  { nullptr, 0, 0 }
};

cmCTestBuildHandler::cmCTestBuildHandler(cmCTest* ctest)
  : Superclass(ctest)
  , LastErrorOrWarning(this->ErrorsAndWarnings.end())
{
}

void cmCTestBuildHandler::PopulateCustomVectors(cmMakefile* mf)
{
  this->CTest->PopulateCustomVector(mf, "CTEST_CUSTOM_ERROR_MATCH",
                                    this->CustomErrorMatches);
  this->CTest->PopulateCustomVector(mf, "CTEST_CUSTOM_ERROR_EXCEPTION",
                                    this->CustomErrorExceptions);
  this->CTest->PopulateCustomVector(mf, "CTEST_CUSTOM_WARNING_MATCH",
                                    this->CustomWarningMatches);
  this->CTest->PopulateCustomVector(mf, "CTEST_CUSTOM_WARNING_EXCEPTION",
                                    this->CustomWarningExceptions);
  this->CTest->PopulateCustomInteger(
    mf, "CTEST_CUSTOM_MAXIMUM_NUMBER_OF_ERRORS", this->MaxErrors);
  this->CTest->PopulateCustomInteger(
    mf, "CTEST_CUSTOM_MAXIMUM_NUMBER_OF_WARNINGS", this->MaxWarnings);

  int n = -1;
  this->CTest->PopulateCustomInteger(mf, "CTEST_CUSTOM_ERROR_PRE_CONTEXT", n);
  if (n != -1) {
    this->MaxPreContext = static_cast<size_t>(n);
  }

  n = -1;
  this->CTest->PopulateCustomInteger(mf, "CTEST_CUSTOM_ERROR_POST_CONTEXT", n);
  if (n != -1) {
    this->MaxPostContext = static_cast<size_t>(n);
  }

  // Record the user-specified custom warning rules.
  if (cmValue customWarningMatchers =
        mf->GetDefinition("CTEST_CUSTOM_WARNING_MATCH")) {
    cmExpandList(*customWarningMatchers, this->ReallyCustomWarningMatches);
  }
  if (cmValue customWarningExceptions =
        mf->GetDefinition("CTEST_CUSTOM_WARNING_EXCEPTION")) {
    cmExpandList(*customWarningExceptions,
                 this->ReallyCustomWarningExceptions);
  }
}

std::string cmCTestBuildHandler::GetMakeCommand()
{
  std::string makeCommand = this->CTest->GetCTestConfiguration("MakeCommand");
  cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                     "MakeCommand:" << makeCommand << "\n", this->Quiet);

  std::string configType = this->CTest->GetConfigType();
  if (configType.empty()) {
    configType =
      this->CTest->GetCTestConfiguration("DefaultCTestConfigurationType");
  }
  if (configType.empty()) {
    configType = "Release";
  }

  cmSystemTools::ReplaceString(makeCommand, "${CTEST_CONFIGURATION_TYPE}",
                               configType.c_str());

  return makeCommand;
}

// clearly it would be nice if this were broken up into a few smaller
// functions and commented...
int cmCTestBuildHandler::ProcessHandler()
{
  cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT, "Build project" << std::endl,
                     this->Quiet);

  // do we have time for this
  if (this->CTest->GetRemainingTimeAllowed() < std::chrono::minutes(2)) {
    return 0;
  }

  int entry;
  for (entry = 0; cmCTestWarningErrorFileLine[entry].RegularExpressionString;
       ++entry) {
    cmCTestBuildHandler::cmCTestCompileErrorWarningRex r;
    if (r.RegularExpression.compile(
          cmCTestWarningErrorFileLine[entry].RegularExpressionString)) {
      r.FileIndex = cmCTestWarningErrorFileLine[entry].FileIndex;
      r.LineIndex = cmCTestWarningErrorFileLine[entry].LineIndex;
      this->ErrorWarningFileLineRegex.push_back(std::move(r));
    } else {
      cmCTestLog(
        this->CTest, ERROR_MESSAGE,
        "Problem Compiling regular expression: "
          << cmCTestWarningErrorFileLine[entry].RegularExpressionString
          << std::endl);
    }
  }

  // Determine build command and build directory
  std::string makeCommand = this->GetMakeCommand();
  if (makeCommand.empty()) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Cannot find MakeCommand key in the DartConfiguration.tcl"
                 << std::endl);
    return -1;
  }

  std::string const& buildDirectory =
    this->CTest->GetCTestConfiguration("BuildDirectory");
  if (buildDirectory.empty()) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Cannot find BuildDirectory  key in the DartConfiguration.tcl"
                 << std::endl);
    return -1;
  }

  std::string const& useLaunchers =
    this->CTest->GetCTestConfiguration("UseLaunchers");
  this->UseCTestLaunch = cmIsOn(useLaunchers);

  // Create a last build log
  cmGeneratedFileStream ofs;
  auto elapsed_time_start = std::chrono::steady_clock::now();
  if (!this->StartLogFile("Build", ofs)) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Cannot create build log file" << std::endl);
  }

  this->LogFileName = ofs.GetTempName();

  // Create lists of regular expression strings for errors, error exceptions,
  // warnings and warning exceptions.
  std::vector<std::string>::size_type cc;
  for (cc = 0; cmCTestErrorMatches[cc]; cc++) {
    this->CustomErrorMatches.emplace_back(cmCTestErrorMatches[cc]);
  }
  for (cc = 0; cmCTestErrorExceptions[cc]; cc++) {
    this->CustomErrorExceptions.emplace_back(cmCTestErrorExceptions[cc]);
  }
  for (cc = 0; cmCTestWarningMatches[cc]; cc++) {
    this->CustomWarningMatches.emplace_back(cmCTestWarningMatches[cc]);
  }

  for (cc = 0; cmCTestWarningExceptions[cc]; cc++) {
    this->CustomWarningExceptions.emplace_back(cmCTestWarningExceptions[cc]);
  }

  // Pre-compile regular expressions objects for all regular expressions

#define cmCTestBuildHandlerPopulateRegexVector(strings, regexes)              \
  do {                                                                        \
    regexes.clear();                                                          \
    cmCTestOptionalLog(this->CTest, DEBUG,                                    \
                       this << "Add " #regexes << std::endl, this->Quiet);    \
    for (std::string const& s : (strings)) {                                  \
      cmCTestOptionalLog(this->CTest, DEBUG,                                  \
                         "Add " #strings ": " << s << std::endl,              \
                         this->Quiet);                                        \
      (regexes).emplace_back(s);                                              \
    }                                                                         \
  } while (false)

  cmCTestBuildHandlerPopulateRegexVector(this->CustomErrorMatches,
                                         this->ErrorMatchRegex);
  cmCTestBuildHandlerPopulateRegexVector(this->CustomErrorExceptions,
                                         this->ErrorExceptionRegex);
  cmCTestBuildHandlerPopulateRegexVector(this->CustomWarningMatches,
                                         this->WarningMatchRegex);
  cmCTestBuildHandlerPopulateRegexVector(this->CustomWarningExceptions,
                                         this->WarningExceptionRegex);

  // Determine source and binary tree substitutions to simplify the output.
  this->SimplifySourceDir.clear();
  this->SimplifyBuildDir.clear();
  if (this->CTest->GetCTestConfiguration("SourceDirectory").size() > 20) {
    std::string srcdir =
      this->CTest->GetCTestConfiguration("SourceDirectory") + "/";
    cc = srcdir.rfind('/', srcdir.size() - 2);
    if (cc != std::string::npos) {
      srcdir.resize(cc + 1);
      this->SimplifySourceDir = std::move(srcdir);
    }
  }
  if (this->CTest->GetCTestConfiguration("BuildDirectory").size() > 20) {
    std::string bindir =
      this->CTest->GetCTestConfiguration("BuildDirectory") + "/";
    cc = bindir.rfind('/', bindir.size() - 2);
    if (cc != std::string::npos) {
      bindir.resize(cc + 1);
      this->SimplifyBuildDir = std::move(bindir);
    }
  }

  // Ok, let's do the build

  // Remember start build time
  this->StartBuild = this->CTest->CurrentTime();
  this->StartBuildTime = std::chrono::system_clock::now();

  cmStringReplaceHelper colorRemover("\x1b\\[[0-9;]*m", "", nullptr);
  this->ColorRemover = &colorRemover;
  int retVal = 0;
  bool res = true;
  if (!this->CTest->GetShowOnly()) {
    res = this->RunMakeCommand(makeCommand, &retVal, buildDirectory.c_str(), 0,
                               ofs);
  } else {
    cmCTestOptionalLog(this->CTest, DEBUG,
                       "Build with command: " << makeCommand << std::endl,
                       this->Quiet);
  }

  // Remember end build time and calculate elapsed time
  this->EndBuild = this->CTest->CurrentTime();
  this->EndBuildTime = std::chrono::system_clock::now();
  auto elapsed_build_time =
    std::chrono::steady_clock::now() - elapsed_time_start;

  // Cleanups strings in the errors and warnings list.
  if (!this->SimplifySourceDir.empty()) {
    for (cmCTestBuildErrorWarning& evit : this->ErrorsAndWarnings) {
      cmSystemTools::ReplaceString(evit.Text, this->SimplifySourceDir.c_str(),
                                   "/.../");
      cmSystemTools::ReplaceString(evit.PreContext,
                                   this->SimplifySourceDir.c_str(), "/.../");
      cmSystemTools::ReplaceString(evit.PostContext,
                                   this->SimplifySourceDir.c_str(), "/.../");
    }
  }

  if (!this->SimplifyBuildDir.empty()) {
    for (cmCTestBuildErrorWarning& evit : this->ErrorsAndWarnings) {
      cmSystemTools::ReplaceString(evit.Text, this->SimplifyBuildDir.c_str(),
                                   "/.../");
      cmSystemTools::ReplaceString(evit.PreContext,
                                   this->SimplifyBuildDir.c_str(), "/.../");
      cmSystemTools::ReplaceString(evit.PostContext,
                                   this->SimplifyBuildDir.c_str(), "/.../");
    }
  }

  // Generate XML output
  cmGeneratedFileStream xofs;
  if (!this->StartResultingXML(cmCTest::PartBuild, "Build", xofs)) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Cannot create build XML file" << std::endl);
    return -1;
  }
  cmXMLWriter xml(xofs);
  this->GenerateXMLHeader(xml);
  if (this->UseCTestLaunch) {
    this->GenerateXMLLaunched(xml);
  } else {
    this->GenerateXMLLogScraped(xml);
  }

  this->CTest->GetInstrumentation().CollectTimingData(
    cmInstrumentationQuery::Hook::PrepareForCDash);
  this->GenerateInstrumentationXML(xml);

  this->GenerateXMLFooter(xml, elapsed_build_time);

  if (!res || retVal || this->TotalErrors > 0) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Error(s) when building project" << std::endl);
  }

  // Display message about number of errors and warnings
  cmCTestLog(this->CTest, HANDLER_OUTPUT,
             "   " << this->TotalErrors
                   << (this->TotalErrors >= this->MaxErrors ? " or more" : "")
                   << " Compiler errors" << std::endl);
  cmCTestLog(
    this->CTest, HANDLER_OUTPUT,
    "   " << this->TotalWarnings
          << (this->TotalWarnings >= this->MaxWarnings ? " or more" : "")
          << " Compiler warnings" << std::endl);

  return retVal;
}

void cmCTestBuildHandler::GenerateXMLHeader(cmXMLWriter& xml)
{
  this->CTest->StartXML(xml, this->CMake, this->AppendXML);
  this->CTest->GenerateSubprojectsOutput(xml);
  xml.StartElement("Build");
  xml.Element("StartDateTime", this->StartBuild);
  xml.Element("StartBuildTime", this->StartBuildTime);
  xml.Element("BuildCommand", this->GetMakeCommand());
}

class cmCTestBuildHandler::FragmentCompare
{
public:
  FragmentCompare(cmFileTimeCache* ftc)
    : FTC(ftc)
  {
  }
  FragmentCompare() = default;
  bool operator()(std::string const& l, std::string const& r) const
  {
    // Order files by modification time.  Use lexicographic order
    // among files with the same time.
    int result;
    if (this->FTC->Compare(l, r, &result) && result != 0) {
      return result < 0;
    }
    return l < r;
  }

private:
  cmFileTimeCache* FTC = nullptr;
};

void cmCTestBuildHandler::GenerateXMLLaunched(cmXMLWriter& xml)
{
  if (this->CTestLaunchDir.empty()) {
    return;
  }

  // Sort XML fragments in chronological order.
  cmFileTimeCache ftc;
  FragmentCompare fragmentCompare(&ftc);
  using Fragments = std::set<std::string, FragmentCompare>;
  Fragments fragments(fragmentCompare);

  // only report the first 50 warnings and first 50 errors
  int numErrorsAllowed = this->MaxErrors;
  int numWarningsAllowed = this->MaxWarnings;
  // Identify fragments on disk.
  cmsys::Directory launchDir;
  launchDir.Load(this->CTestLaunchDir);
  unsigned long n = launchDir.GetNumberOfFiles();
  for (unsigned long i = 0; i < n; ++i) {
    char const* fname = launchDir.GetFile(i);
    if (this->IsLaunchedErrorFile(fname) && numErrorsAllowed) {
      numErrorsAllowed--;
      fragments.insert(this->CTestLaunchDir + '/' + fname);
      ++this->TotalErrors;
    } else if (this->IsLaunchedWarningFile(fname) && numWarningsAllowed) {
      numWarningsAllowed--;
      fragments.insert(this->CTestLaunchDir + '/' + fname);
      ++this->TotalWarnings;
    }
  }

  // Copy the fragments into the final XML file.
  for (std::string const& f : fragments) {
    xml.FragmentFile(f.c_str());
  }
}

void cmCTestBuildHandler::GenerateXMLLogScraped(cmXMLWriter& xml)
{
  std::vector<cmCTestBuildErrorWarning>& ew = this->ErrorsAndWarnings;
  std::vector<cmCTestBuildErrorWarning>::iterator it;

  // only report the first 50 warnings and first 50 errors
  int numErrorsAllowed = this->MaxErrors;
  int numWarningsAllowed = this->MaxWarnings;
  std::string srcdir = this->CTest->GetCTestConfiguration("SourceDirectory");
  for (it = ew.begin();
       it != ew.end() && (numErrorsAllowed || numWarningsAllowed); it++) {
    cmCTestBuildErrorWarning* cm = &(*it);
    if ((cm->Error && numErrorsAllowed) ||
        (!cm->Error && numWarningsAllowed)) {
      if (cm->Error) {
        numErrorsAllowed--;
      } else {
        numWarningsAllowed--;
      }
      xml.StartElement(cm->Error ? "Error" : "Warning");
      xml.Element("BuildLogLine", cm->LogLine);
      xml.Element("Text", cm->Text);
      for (cmCTestCompileErrorWarningRex& rit :
           this->ErrorWarningFileLineRegex) {
        cmsys::RegularExpression* re = &rit.RegularExpression;
        if (re->find(cm->Text)) {
          cm->SourceFile = re->match(rit.FileIndex);
          // At this point we need to make this->SourceFile relative to
          // the source root of the project, so cvs links will work
          cmSystemTools::ConvertToUnixSlashes(cm->SourceFile);
          if (cm->SourceFile.find("/.../") != std::string::npos) {
            cmSystemTools::ReplaceString(cm->SourceFile, "/.../", "");
            std::string::size_type p = cm->SourceFile.find('/');
            if (p != std::string::npos) {
              cm->SourceFile =
                cm->SourceFile.substr(p + 1, cm->SourceFile.size() - p);
            }
          } else {
            // make sure it is a full path with the correct case
            cm->SourceFile =
              cmSystemTools::ToNormalizedPathOnDisk(cm->SourceFile);
            cmSystemTools::ReplaceString(cm->SourceFile, srcdir, "");
          }
          cm->LineNumber = atoi(re->match(rit.LineIndex).c_str());
          break;
        }
      }
      if (!cm->SourceFile.empty() && cm->LineNumber >= 0) {
        if (!cm->SourceFile.empty()) {
          xml.Element("SourceFile", cm->SourceFile);
        }
        if (!cm->SourceFileTail.empty()) {
          xml.Element("SourceFileTail", cm->SourceFileTail);
        }
        if (cm->LineNumber >= 0) {
          xml.Element("SourceLineNumber", cm->LineNumber);
        }
      }
      xml.Element("PreContext", cm->PreContext);
      xml.StartElement("PostContext");
      xml.Content(cm->PostContext);
      // is this the last warning or error, if so notify
      if ((cm->Error && !numErrorsAllowed) ||
          (!cm->Error && !numWarningsAllowed)) {
        xml.Content("\nThe maximum number of reported warnings or errors "
                    "has been reached!!!\n");
      }
      xml.EndElement(); // PostContext
      xml.Element("RepeatCount", "0");
      xml.EndElement(); // "Error" / "Warning"
    }
  }
}

void cmCTestBuildHandler::GenerateInstrumentationXML(cmXMLWriter& xml)
{
  // Record instrumentation data on a per-target basis.
  cmsys::Directory targets_dir;
  std::string targets_snippet_dir = cmStrCat(
    this->CTest->GetInstrumentation().GetCDashDir(), "/build/targets");
  if (targets_dir.Load(targets_snippet_dir) &&
      targets_dir.GetNumberOfFiles() > 0) {
    xml.StartElement("Targets");
    for (unsigned int i = 0; i < targets_dir.GetNumberOfFiles(); i++) {
      if (!targets_dir.FileIsDirectory(i)) {
        continue;
      }
      std::string target_name = targets_dir.GetFile(i);
      if (target_name == "." || target_name == "..") {
        continue;
      }
      std::string target_type = "UNKNOWN";

      xml.StartElement("Target");
      xml.Attribute("name", target_name);

      // Check if we have a link snippet for this target.
      cmsys::Directory target_dir;
      if (!target_dir.Load(targets_dir.GetFilePath(i))) {
        cmSystemTools::Error(
          cmStrCat("Error loading directory ", targets_dir.GetFilePath(i)));
      }
      Json::Value link_item;
      for (unsigned int j = 0; j < target_dir.GetNumberOfFiles(); j++) {
        std::string fname = target_dir.GetFile(j);
        if (fname.rfind("link-", 0) == 0) {
          std::string fpath = target_dir.GetFilePath(j);
          cmJSONState parseState = cmJSONState(fpath, &link_item);
          if (!parseState.errors.empty()) {
            cmSystemTools::Error(parseState.GetErrorMessage(true));
            break;
          }

          if (!link_item.isObject()) {
            std::string error_msg =
              cmStrCat("Expected snippet ", fpath, " to contain an object");
            cmSystemTools::Error(error_msg);
            break;
          }
          break;
        }
      }

      // If so, parse targetType and targetLabels (optional) from it.
      if (link_item.isMember("targetType")) {
        target_type = link_item["targetType"].asString();
      }

      xml.Attribute("type", target_type);

      if (link_item.isMember("targetLabels") &&
          !link_item["targetLabels"].empty()) {
        xml.StartElement("Labels");
        for (auto const& json_label_item : link_item["targetLabels"]) {
          xml.Element("Label", json_label_item.asString());
        }
        xml.EndElement(); // Labels
      }

      // Write instrumendation data for this target.
      std::string target_subdir = cmStrCat("build/targets/", target_name);
      this->CTest->ConvertInstrumentationSnippetsToXML(xml, target_subdir);
      std::string target_dir_fullpath = cmStrCat(
        this->CTest->GetInstrumentation().GetCDashDir(), '/', target_subdir);
      if (cmSystemTools::FileIsDirectory(target_dir_fullpath)) {
        cmSystemTools::RemoveADirectory(target_dir_fullpath);
      }
      xml.EndElement(); // Target
    }
    xml.EndElement(); // Targets
  }

  // Also record instrumentation data for custom commands (no target).
  this->CTest->ConvertInstrumentationSnippetsToXML(xml, "build/commands");
}

void cmCTestBuildHandler::GenerateXMLFooter(cmXMLWriter& xml,
                                            cmDuration elapsed_build_time)
{
  xml.StartElement("Log");
  xml.Attribute("Encoding", "base64");
  xml.Attribute("Compression", "bin/gzip");
  xml.EndElement(); // Log

  xml.Element("EndDateTime", this->EndBuild);
  xml.Element("EndBuildTime", this->EndBuildTime);
  xml.Element(
    "ElapsedMinutes",
    std::chrono::duration_cast<std::chrono::minutes>(elapsed_build_time)
      .count());
  xml.EndElement(); // Build
  this->CTest->EndXML(xml);
}

bool cmCTestBuildHandler::IsLaunchedErrorFile(char const* fname)
{
  // error-{hash}.xml
  return (cmHasLiteralPrefix(fname, "error-") &&
          cmHasLiteralSuffix(fname, ".xml"));
}

bool cmCTestBuildHandler::IsLaunchedWarningFile(char const* fname)
{
  // warning-{hash}.xml
  return (cmHasLiteralPrefix(fname, "warning-") &&
          cmHasLiteralSuffix(fname, ".xml"));
}

// ######################################################################
// ######################################################################
// ######################################################################
// ######################################################################

class cmCTestBuildHandler::LaunchHelper
{
public:
  LaunchHelper(cmCTestBuildHandler* handler);
  ~LaunchHelper();
  LaunchHelper(LaunchHelper const&) = delete;
  LaunchHelper& operator=(LaunchHelper const&) = delete;

private:
  cmCTestBuildHandler* Handler;
  cmCTest* CTest;

  void WriteLauncherConfig();
  void WriteScrapeMatchers(char const* purpose,
                           std::vector<std::string> const& matchers);
};

cmCTestBuildHandler::LaunchHelper::LaunchHelper(cmCTestBuildHandler* handler)
  : Handler(handler)
  , CTest(handler->CTest)
{
  std::string tag = this->CTest->GetCurrentTag();
  if (tag.empty()) {
    // This is not for a dashboard submission, so there is no XML.
    // Skip enabling the launchers.
    this->Handler->UseCTestLaunch = false;
  } else {
    // Compute a directory in which to store launcher fragments.
    std::string& launchDir = this->Handler->CTestLaunchDir;
    launchDir =
      cmStrCat(this->CTest->GetBinaryDir(), "/Testing/", tag, "/Build");

    // Clean out any existing launcher fragments.
    cmSystemTools::RemoveADirectory(launchDir);

    if (this->Handler->UseCTestLaunch) {
      // Enable launcher fragments.
      cmSystemTools::MakeDirectory(launchDir);
      this->WriteLauncherConfig();
      std::string launchEnv = cmStrCat("CTEST_LAUNCH_LOGS=", launchDir);
      cmSystemTools::PutEnv(launchEnv);
    }
  }

  // If not using launchers, make sure they passthru.
  if (!this->Handler->UseCTestLaunch) {
    cmSystemTools::UnsetEnv("CTEST_LAUNCH_LOGS");
  }
}

cmCTestBuildHandler::LaunchHelper::~LaunchHelper()
{
  if (this->Handler->UseCTestLaunch) {
    cmSystemTools::UnsetEnv("CTEST_LAUNCH_LOGS");
  }
}

void cmCTestBuildHandler::LaunchHelper::WriteLauncherConfig()
{
  this->WriteScrapeMatchers("Warning",
                            this->Handler->ReallyCustomWarningMatches);
  this->WriteScrapeMatchers("WarningSuppress",
                            this->Handler->ReallyCustomWarningExceptions);

  // Give some testing configuration information to the launcher.
  std::string fname =
    cmStrCat(this->Handler->CTestLaunchDir, "/CTestLaunchConfig.cmake");
  cmGeneratedFileStream fout(fname);
  std::string srcdir = this->CTest->GetCTestConfiguration("SourceDirectory");
  fout << "set(CTEST_SOURCE_DIRECTORY \"" << srcdir << "\")\n";
}

void cmCTestBuildHandler::LaunchHelper::WriteScrapeMatchers(
  char const* purpose, std::vector<std::string> const& matchers)
{
  if (matchers.empty()) {
    return;
  }
  std::string fname =
    cmStrCat(this->Handler->CTestLaunchDir, "/Custom", purpose, ".txt");
  cmGeneratedFileStream fout(fname);
  for (std::string const& m : matchers) {
    fout << m << "\n";
  }
}

bool cmCTestBuildHandler::RunMakeCommand(std::string const& command,
                                         int* retVal, char const* dir,
                                         int timeout, std::ostream& ofs,
                                         Encoding encoding)
{
  // First generate the command and arguments
  std::vector<std::string> args = cmSystemTools::ParseArguments(command);

  if (args.empty()) {
    return false;
  }

  cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                     "Run command:", this->Quiet);
  for (auto const& arg : args) {
    cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                       " \"" << arg << "\"", this->Quiet);
  }
  cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT, std::endl,
                     this->Quiet);

  // Optionally use make rule launchers to record errors and warnings.
  LaunchHelper launchHelper(this);
  static_cast<void>(launchHelper);

  // Now create process object
  cmUVProcessChainBuilder builder;
  builder.AddCommand(args)
    .SetBuiltinStream(cmUVProcessChainBuilder::Stream_OUTPUT)
    .SetBuiltinStream(cmUVProcessChainBuilder::Stream_ERROR);
  if (dir) {
    builder.SetWorkingDirectory(dir);
  }
  auto chain = builder.Start();

  // Initialize tick's
  std::string::size_type tick = 0;
  static constexpr std::string::size_type tick_len = 1024;

  cmProcessOutput processOutput(encoding);
  cmCTestOptionalLog(
    this->CTest, HANDLER_PROGRESS_OUTPUT,
    "   Each symbol represents "
      << tick_len << " bytes of output." << std::endl
      << (this->UseCTestLaunch
            ? ""
            : "   '!' represents an error and '*' a warning.\n")
      << "    " << std::flush,
    this->Quiet);

  // Initialize building structures
  this->BuildProcessingQueue.clear();
  this->OutputLineCounter = 0;
  this->ErrorsAndWarnings.clear();
  this->TotalErrors = 0;
  this->TotalWarnings = 0;
  this->BuildOutputLogSize = 0;
  this->LastTickChar = '.';
  this->WarningQuotaReached = false;
  this->ErrorQuotaReached = false;

  cm::uv_timer_ptr timer;
  bool timedOut = false;
  timer.init(chain.GetLoop(), &timedOut);
  if (timeout > 0) {
    timer.start(
      [](uv_timer_t* t) {
        auto* timedOutPtr = static_cast<bool*>(t->data);
        *timedOutPtr = true;
      },
      timeout * 1000, 0);
  }

  // For every chunk of data
  cm::uv_pipe_ptr outputStream;
  bool outFinished = false;
  cm::uv_pipe_ptr errorStream;
  bool errFinished = false;
  auto startRead = [this, &chain, &processOutput, &tick,
                    &ofs](cm::uv_pipe_ptr& pipe, int stream,
                          t_BuildProcessingQueueType& queue, bool& finished,
                          int id) -> std::unique_ptr<cmUVStreamReadHandle> {
    pipe.init(chain.GetLoop(), 0);
    uv_pipe_open(pipe, stream);
    return cmUVStreamRead(
      pipe,
      [this, &processOutput, &queue, id, &tick, &ofs](std::vector<char> data) {
        // Replace '\0' with '\n', since '\0' does not really make sense. This
        // is for Visual Studio output
        for (auto& c : data) {
          if (c == 0) {
            c = '\n';
          }
        }

        // Process the chunk of data
        std::string strdata;
        processOutput.DecodeText(data.data(), data.size(), strdata, id);
        this->ProcessBuffer(strdata.c_str(), strdata.size(), tick, tick_len,
                            ofs, &queue);
      },
      [this, &processOutput, &queue, id, &tick, &ofs, &finished]() {
        std::string strdata;
        processOutput.DecodeText(std::string(), strdata, id);
        if (!strdata.empty()) {
          this->ProcessBuffer(strdata.c_str(), strdata.size(), tick, tick_len,
                              ofs, &queue);
        }
        finished = true;
      });
  };
  auto outputHandle = startRead(outputStream, chain.OutputStream(),
                                this->BuildProcessingQueue, outFinished, 1);
  auto errorHandle =
    startRead(errorStream, chain.ErrorStream(),
              this->BuildProcessingErrorQueue, errFinished, 2);

  while (!timedOut && !(outFinished && errFinished && chain.Finished())) {
    uv_run(&chain.GetLoop(), UV_RUN_ONCE);
  }
  this->ProcessBuffer(nullptr, 0, tick, tick_len, ofs,
                      &this->BuildProcessingQueue);
  this->ProcessBuffer(nullptr, 0, tick, tick_len, ofs,
                      &this->BuildProcessingErrorQueue);
  cmCTestOptionalLog(this->CTest, HANDLER_PROGRESS_OUTPUT,
                     " Size of output: "
                       << ((this->BuildOutputLogSize + 512) / 1024) << "K"
                       << std::endl,
                     this->Quiet);

  if (chain.Finished()) {
    auto const& status = chain.GetStatus(0);
    auto exception = status.GetException();
    switch (exception.first) {
      case cmUVProcessChain::ExceptionCode::None:
        if (retVal) {
          *retVal = static_cast<int>(status.ExitStatus);
          cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                             "Command exited with the value: " << *retVal
                                                               << std::endl,
                             this->Quiet);
          // if a non zero return value
          if (*retVal) {
            // If there was an error running command, report that on the
            // dashboard.
            if (this->UseCTestLaunch) {
              // For launchers, do not record this top-level error if other
              // more granular build errors have already been captured.
              bool launcherXMLFound = false;
              cmsys::Directory launchDir;
              launchDir.Load(this->CTestLaunchDir);
              unsigned long n = launchDir.GetNumberOfFiles();
              for (unsigned long i = 0; i < n; ++i) {
                char const* fname = launchDir.GetFile(i);
                if (cmHasLiteralSuffix(fname, ".xml")) {
                  launcherXMLFound = true;
                  break;
                }
              }
              if (!launcherXMLFound) {
                cmCTestLaunchReporter reporter;
                reporter.RealArgs = args;
                reporter.ComputeFileNames();
                reporter.ExitCode = *retVal;
                reporter.Status = status;
                // Use temporary BuildLog file to populate this error for
                // CDash.
                ofs.flush();
                reporter.LogOut = this->LogFileName;
                reporter.WriteXML();
              }
            } else {
              cmCTestBuildErrorWarning errorwarning;
              errorwarning.LineNumber = 0;
              errorwarning.LogLine = 1;
              errorwarning.Text = cmStrCat(
                "*** WARNING non-zero return value in ctest from: ", args[0]);
              errorwarning.PreContext.clear();
              errorwarning.PostContext.clear();
              errorwarning.Error = false;
              this->ErrorsAndWarnings.push_back(std::move(errorwarning));
              this->TotalWarnings++;
            }
          }
        }
        break;
      case cmUVProcessChain::ExceptionCode::Spawn: {
        // If there was an error running command, report that on the dashboard.
        cmCTestBuildErrorWarning errorwarning;
        errorwarning.LineNumber = 0;
        errorwarning.LogLine = 1;
        errorwarning.Text =
          cmStrCat("*** ERROR executing: ", exception.second);
        errorwarning.PreContext.clear();
        errorwarning.PostContext.clear();
        errorwarning.Error = true;
        this->ErrorsAndWarnings.push_back(std::move(errorwarning));
        this->TotalErrors++;
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "There was an error: " << exception.second << std::endl);
      } break;
      default:
        if (retVal) {
          *retVal = status.TermSignal;
          cmCTestOptionalLog(
            this->CTest, WARNING,
            "There was an exception: " << *retVal << std::endl, this->Quiet);
        }
        break;
    }
  } else {
    cmCTestOptionalLog(this->CTest, WARNING,
                       "There was a timeout" << std::endl, this->Quiet);
  }

  return true;
}

// ######################################################################
// ######################################################################
// ######################################################################
// ######################################################################

void cmCTestBuildHandler::ProcessBuffer(char const* data, size_t length,
                                        size_t& tick, size_t tick_len,
                                        std::ostream& ofs,
                                        t_BuildProcessingQueueType* queue)
{
  std::string::size_type const tick_line_len = 50;
  char const* ptr;
  for (ptr = data; ptr < data + length; ptr++) {
    queue->push_back(*ptr);
  }
  this->BuildOutputLogSize += length;

  // until there are any lines left in the buffer
  while (true) {
    // Find the end of line
    t_BuildProcessingQueueType::iterator it;
    for (it = queue->begin(); it != queue->end(); ++it) {
      if (*it == '\n') {
        break;
      }
    }

    // Once certain number of errors or warnings reached, ignore future errors
    // or warnings.
    if (this->TotalWarnings >= this->MaxWarnings) {
      this->WarningQuotaReached = true;
    }
    if (this->TotalErrors >= this->MaxErrors) {
      this->ErrorQuotaReached = true;
    }

    // If the end of line was found
    if (it != queue->end()) {
      // Create a contiguous array for the line
      this->CurrentProcessingLine.clear();
      cm::append(this->CurrentProcessingLine, queue->begin(), it);
      this->CurrentProcessingLine.push_back(0);
      char const* line = this->CurrentProcessingLine.data();

      // Process the line
      int lineType = this->ProcessSingleLine(line);

      // Erase the line from the queue
      queue->erase(queue->begin(), it + 1);

      // Depending on the line type, produce error or warning, or nothing
      cmCTestBuildErrorWarning errorwarning;
      bool found = false;
      switch (lineType) {
        case b_WARNING_LINE:
          this->LastTickChar = '*';
          errorwarning.Error = false;
          found = true;
          this->TotalWarnings++;
          break;
        case b_ERROR_LINE:
          this->LastTickChar = '!';
          errorwarning.Error = true;
          found = true;
          this->TotalErrors++;
          break;
      }
      if (found) {
        // This is an error or warning, so generate report
        errorwarning.LogLine = static_cast<int>(this->OutputLineCounter + 1);
        errorwarning.Text = line;
        errorwarning.PreContext.clear();
        errorwarning.PostContext.clear();

        // Copy pre-context to report
        for (std::string const& pc : this->PreContext) {
          errorwarning.PreContext += pc + "\n";
        }
        this->PreContext.clear();

        // Store report
        this->ErrorsAndWarnings.push_back(std::move(errorwarning));
        this->LastErrorOrWarning = this->ErrorsAndWarnings.end() - 1;
        this->PostContextCount = 0;
      } else {
        // This is not an error or warning.
        // So, figure out if this is a post-context line
        if (!this->ErrorsAndWarnings.empty() &&
            this->LastErrorOrWarning != this->ErrorsAndWarnings.end() &&
            this->PostContextCount < this->MaxPostContext) {
          this->PostContextCount++;
          this->LastErrorOrWarning->PostContext += line;
          if (this->PostContextCount < this->MaxPostContext) {
            this->LastErrorOrWarning->PostContext += "\n";
          }
        } else {
          // Otherwise store pre-context for the next error
          this->PreContext.emplace_back(line);
          if (this->PreContext.size() > this->MaxPreContext) {
            this->PreContext.erase(this->PreContext.begin(),
                                   this->PreContext.end() -
                                     this->MaxPreContext);
          }
        }
      }
      this->OutputLineCounter++;
    } else {
      break;
    }
  }

  // Now that the buffer is processed, display missing ticks
  int tickDisplayed = false;
  while (this->BuildOutputLogSize > (tick * tick_len)) {
    tick++;
    cmCTestOptionalLog(this->CTest, HANDLER_PROGRESS_OUTPUT,
                       this->LastTickChar, this->Quiet);
    tickDisplayed = true;
    if (tick % tick_line_len == 0 && tick > 0) {
      cmCTestOptionalLog(this->CTest, HANDLER_PROGRESS_OUTPUT,
                         "  Size: "
                           << ((this->BuildOutputLogSize + 512) / 1024) << "K"
                           << std::endl
                           << "    ",
                         this->Quiet);
    }
  }
  if (tickDisplayed) {
    this->LastTickChar = '.';
  }

  // And if this is verbose output, display the content of the chunk
  cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
             cm::string_view(data, length));

  // Always store the chunk to the file
  ofs << cm::string_view(data, length);
}

int cmCTestBuildHandler::ProcessSingleLine(char const* data)
{
  if (this->UseCTestLaunch) {
    // No log scraping when using launchers.
    return b_REGULAR_LINE;
  }

  // Ignore ANSI color codes when checking for errors and warnings.
  std::string input(data);
  std::string line;
  this->ColorRemover->Replace(input, line);

  cmCTestOptionalLog(this->CTest, DEBUG, "Line: [" << line << "]" << std::endl,
                     this->Quiet);

  int warningLine = 0;
  int errorLine = 0;

  // Check for regular expressions

  if (!this->ErrorQuotaReached) {
    // Errors
    int wrxCnt = 0;
    for (cmsys::RegularExpression& rx : this->ErrorMatchRegex) {
      if (rx.find(line.c_str())) {
        errorLine = 1;
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "  Error Line: " << line << " (matches: "
                                            << this->CustomErrorMatches[wrxCnt]
                                            << ")" << std::endl,
                           this->Quiet);
        break;
      }
      wrxCnt++;
    }
    // Error exceptions
    wrxCnt = 0;
    for (cmsys::RegularExpression& rx : this->ErrorExceptionRegex) {
      if (rx.find(line.c_str())) {
        errorLine = 0;
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "  Not an error Line: "
                             << line << " (matches: "
                             << this->CustomErrorExceptions[wrxCnt] << ")"
                             << std::endl,
                           this->Quiet);
        break;
      }
      wrxCnt++;
    }
  }
  if (!this->WarningQuotaReached) {
    // Warnings
    int wrxCnt = 0;
    for (cmsys::RegularExpression& rx : this->WarningMatchRegex) {
      if (rx.find(line.c_str())) {
        warningLine = 1;
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "  Warning Line: "
                             << line << " (matches: "
                             << this->CustomWarningMatches[wrxCnt] << ")"
                             << std::endl,
                           this->Quiet);
        break;
      }
      wrxCnt++;
    }

    wrxCnt = 0;
    // Warning exceptions
    for (cmsys::RegularExpression& rx : this->WarningExceptionRegex) {
      if (rx.find(line.c_str())) {
        warningLine = 0;
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "  Not a warning Line: "
                             << line << " (matches: "
                             << this->CustomWarningExceptions[wrxCnt] << ")"
                             << std::endl,
                           this->Quiet);
        break;
      }
      wrxCnt++;
    }
  }
  if (errorLine) {
    return b_ERROR_LINE;
  }
  if (warningLine) {
    return b_WARNING_LINE;
  }
  return b_REGULAR_LINE;
}
