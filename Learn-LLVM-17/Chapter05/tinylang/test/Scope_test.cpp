#include "tinylang/Sema/Scope.h"
#include "tinylang/AST/AST.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include "gtest/gtest.h"

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace tinylang;

namespace {

enum class ActionKind { Insert, Lookup };

struct ScopeAction {
  ActionKind Action;
  int ScopeLevel;
  std::string Name;
  bool ExpectedSuccess;
  int DeclID;
};

struct ScopeTestParam {
  std::string TestName;
  std::vector<ScopeAction> Actions;
};

class ScopeTest : public ::testing::TestWithParam<ScopeTestParam> {
protected:
  std::vector<std::unique_ptr<Scope>> Scopes;
  std::map<int, std::unique_ptr<Decl>> Decls;
  std::list<std::string> StringStorage;

  Scope *getOrCreateScope(int Level) {
    while (Scopes.size() <= static_cast<size_t>(Level)) {
      Scope *Parent = Scopes.empty() ? nullptr : Scopes.back().get();
      Scopes.push_back(std::make_unique<Scope>(Parent));
    }
    return Scopes[Level].get();
  }

  Decl *getOrCreateDecl(int ID, const std::string &Name) {
    if (Decls.find(ID) == Decls.end()) {
      StringStorage.push_back(Name);
      llvm::StringRef SR(StringStorage.back());
      Decls[ID] = std::make_unique<Decl>(Decl::DK_Var, nullptr, llvm::SMLoc(), SR);
    }
    return Decls[ID].get();
  }
};

TEST_P(ScopeTest, ExecuteActions) {
  const auto &Param = GetParam();

  for (size_t i = 0; i < Param.Actions.size(); ++i) {
    const auto &Act = Param.Actions[i];
    Scope *S = getOrCreateScope(Act.ScopeLevel);

    if (Act.Action == ActionKind::Insert) {
      Decl *D = getOrCreateDecl(Act.DeclID, Act.Name);
      bool Result = S->insert(D);

      EXPECT_EQ(Result, Act.ExpectedSuccess)
          << "Action " << i << ": Insert '" << Act.Name
          << "' at level " << Act.ScopeLevel << " failed expectation.";

    } else if (Act.Action == ActionKind::Lookup) {
      Decl *Result = S->lookup(Act.Name);

      if (Act.ExpectedSuccess) {
        ASSERT_NE(Result, nullptr)
            << "Action " << i << ": Lookup '" << Act.Name
            << "' at level " << Act.ScopeLevel << " unexpectedly returned null.";

        Decl *ExpectedDecl = getOrCreateDecl(Act.DeclID, Act.Name);
        EXPECT_EQ(Result, ExpectedDecl)
            << "Action " << i << ": Lookup '" << Act.Name
            << "' returned the wrong Decl instance.";

      } else {
        EXPECT_EQ(Result, nullptr)
            << "Action " << i << ": Lookup '" << Act.Name
            << "' unexpectedly found a Decl.";
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    ScopeBehaviors, ScopeTest,
    ::testing::Values(
        ScopeTestParam{
            "BasicInsertAndLookup",
            {
                {ActionKind::Insert, 0, "var1", true, 1},
                {ActionKind::Lookup, 0, "var1", true, 1}
            }
        },
        ScopeTestParam{
            "DuplicateInsertFails",
            {
                {ActionKind::Insert, 0, "var1", true, 1},
                {ActionKind::Insert, 0, "var1", false, 2},
                {ActionKind::Lookup, 0, "var1", true, 1}
            }
        },
        ScopeTestParam{
            "ParentScopeLookup",
            {
                {ActionKind::Insert, 0, "globalVar", true, 1},
                {ActionKind::Lookup, 1, "globalVar", true, 1},
                {ActionKind::Lookup, 2, "globalVar", true, 1}
            }
        },
        ScopeTestParam{
            "ShadowingVariables",
            {
                {ActionKind::Insert, 0, "shadowVar", true, 1},
                {ActionKind::Insert, 1, "shadowVar", true, 2},
                {ActionKind::Lookup, 1, "shadowVar", true, 2},
                {ActionKind::Lookup, 0, "shadowVar", true, 1}
            }
        },
        ScopeTestParam{
            "ChildCannotBeSeenByParent",
            {
                {ActionKind::Insert, 1, "localVar", true, 1},
                {ActionKind::Lookup, 0, "localVar", false, 0}
            }
        },
        ScopeTestParam{
            "NotFound",
            {
                {ActionKind::Lookup, 0, "nonExistent", false, 0},
                {ActionKind::Lookup, 1, "nonExistent", false, 0}
            }
        },
        ScopeTestParam{
            "CaseSensitivity",
            {
                {ActionKind::Insert, 0, "MyVar", true, 1},
                {ActionKind::Lookup, 0, "myvar", false, 0},
                {ActionKind::Lookup, 0, "MyVar", true, 1}
            }
        },
        ScopeTestParam{
            "DeepNestingLookup",
            {
                {ActionKind::Insert, 0, "deepVar", true, 1},
                {ActionKind::Lookup, 5, "deepVar", true, 1}
            }
        },
        ScopeTestParam{
            "MultiLevelShadowing",
            {
                {ActionKind::Insert, 0, "x", true, 1},
                {ActionKind::Insert, 1, "x", true, 2},
                {ActionKind::Insert, 2, "x", true, 3},
                {ActionKind::Lookup, 2, "x", true, 3},
                {ActionKind::Lookup, 1, "x", true, 2},
                {ActionKind::Lookup, 0, "x", true, 1}
            }
        },
        ScopeTestParam{
            "MultipleVariablesSameScope",
            {
                {ActionKind::Insert, 1, "varA", true, 1},
                {ActionKind::Insert, 1, "varB", true, 2},
                {ActionKind::Insert, 1, "varC", true, 3},
                {ActionKind::Lookup, 1, "varB", true, 2},
                {ActionKind::Lookup, 2, "varA", true, 1},
                {ActionKind::Lookup, 2, "varC", true, 3}
            }
        }
    ),
    [](const ::testing::TestParamInfo<ScopeTestParam> &info) { return info.param.TestName; }
);

} // namespace
