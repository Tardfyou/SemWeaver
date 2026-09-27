Reference skeleton for a Clang 18 CSA plugin. It is provided only on the first
generation turn. It defines structure, not a vulnerability mechanism; replace
every placeholder.

```cpp
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Version.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"
#include "llvm/ADT/StringRef.h"
#include <memory>

using namespace clang;
using namespace ento;

namespace {

class PatchGuidedChecker : public Checker<check::PreCall> {
  mutable std::unique_ptr<BugType> BT;

public:
  PatchGuidedChecker()
      : BT(std::make_unique<BugType>(this, "Patch-guided bug", "Custom")) {}

  void checkPreCall(const CallEvent &Call, CheckerContext &C) const {
    const IdentifierInfo *II = Call.getCalleeIdentifier();
    if (!II)
      return;

    StringRef FuncName = II->getName();
    // Bind the patch-evidenced trigger, guard, barrier, or state here.
    // Never report from the API name alone; check a semantic condition.

    if (false) { // Replace with a real condition.
      if (ExplodedNode *N = C.generateNonFatalErrorNode()) {
        auto R = std::make_unique<PathSensitiveBugReport>(
            *BT, "Describe the bug pattern here.", N);
        C.emitReport(std::move(R));
      }
    }
  }
};

} // namespace

extern "C" void clang_registerCheckers(CheckerRegistry &Registry) {
  Registry.addChecker<PatchGuidedChecker>(
      "custom.PatchGuidedChecker",
      "Patch-guided checker.",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] = CLANG_ANALYZER_API_VERSION_STRING;
```
