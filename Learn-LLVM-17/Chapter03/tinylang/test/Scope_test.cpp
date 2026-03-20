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

// Represents an action to perform on the Scope hierarchy
enum class ActionKind { Insert, Lookup };

struct ScopeAction {
  ActionKind Action;
  int ScopeLevel;         // 0 = Global, 1 = Local, 2 = Nested Local
  std::string Name;       // Name of the identifier
  bool ExpectedSuccess;   // Insert: true if successful. Lookup: true if found.
  int DeclID;             // ID to link a specific Decl across inserts and lookups
};

struct ScopeTestParam {
  std::string TestName;
  std::vector<ScopeAction> Actions;
};

// Test fixture to manage Scopes, Decls, and Memory
class ScopeTest : public ::testing::TestWithParam<ScopeTestParam> {
protected:
  std::vector<std::unique_ptr<Scope>> Scopes;
  std::map<int, std::unique_ptr<Decl>> Decls;

  // std::list guarantees memory locations for strings don't change
  // when pushing new elements, keeping StringRefs valid.
  std::list<std::string> StringStorage;

  // Retrieves a scope, creating it and its parents if they don't exist
  Scope *getOrCreateScope(int Level) {
    while (Scopes.size() <= static_cast<size_t>(Level)) {
      Scope *Parent = Scopes.empty() ? nullptr : Scopes.back().get();
      Scopes.push_back(std::make_unique<Scope>(Parent));
    }
    return Scopes[Level].get();
  }

  // Retrieves or creates a mock declaration for testing
  Decl *getOrCreateDecl(int ID, const std::string &Name) {
    if (Decls.find(ID) == Decls.end()) {
      StringStorage.push_back(Name);
      llvm::StringRef SR(StringStorage.back());
      // Instantiate a generic Decl using the public constructor
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
      bool Result = S->insert(D); //

      EXPECT_EQ(Result, Act.ExpectedSuccess)
          << "Action " << i << ": Insert '" << Act.Name
          << "' at level " << Act.ScopeLevel << " failed expectation.";

    } else if (Act.Action == ActionKind::Lookup) {
      Decl *Result = S->lookup(Act.Name); //

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
        // 1. Basic insertion and lookup in the same scope
        ScopeTestParam{
            "BasicInsertAndLookup",
            {
                {ActionKind::Insert, 0, "var1", true, 1},
                {ActionKind::Lookup, 0, "var1", true, 1}
            }
        },

        // 2. Ensuring the map prevents duplicate names in the SAME scope
        ScopeTestParam{
            "DuplicateInsertFails",
            {
                {ActionKind::Insert, 0, "var1", true, 1},
                {ActionKind::Insert, 0, "var1", false, 2}, // Insertion rejected
                {ActionKind::Lookup, 0, "var1", true, 1}   // Should still yield the first one
            }
        },

        // 3. Ensuring child scopes walk the parent chain to find symbols
        ScopeTestParam{
            "ParentScopeLookup",
            {
                {ActionKind::Insert, 0, "globalVar", true, 1},
                {ActionKind::Lookup, 1, "globalVar", true, 1}, // Level 1 finds Level 0
                {ActionKind::Lookup, 2, "globalVar", true, 1}  // Level 2 finds Level 0
            }
        },

        // 4. Ensuring local variables shadow global variables with the same name
        ScopeTestParam{
            "ShadowingVariables",
            {
                {ActionKind::Insert, 0, "shadowVar", true, 1},
                {ActionKind::Insert, 1, "shadowVar", true, 2}, // Allowed! Different scope.
                {ActionKind::Lookup, 1, "shadowVar", true, 2}, // Resolves to local (ID 2)
                {ActionKind::Lookup, 0, "shadowVar", true, 1}  // Resolves to global (ID 1)
            }
        },

        // 5. Parent scope cannot peek into child scopes (Visibility rule)
        ScopeTestParam{
            "ChildCannotBeSeenByParent",
            {
                {ActionKind::Insert, 1, "localVar", true, 1},
                {ActionKind::Lookup, 0, "localVar", false, 0} // Fails
            }
        },

        // 6. Looking up names that were never inserted
        ScopeTestParam{
            "NotFound",
            {
                {ActionKind::Lookup, 0, "nonExistent", false, 0},
                {ActionKind::Lookup, 1, "nonExistent", false, 0}
            }
        },

        // 7. Case Sensitivity: "Var" and "var" are distinct symbols
        ScopeTestParam{
            "CaseSensitivity",
            {
                {ActionKind::Insert, 0, "MyVar", true, 1},
                {ActionKind::Lookup, 0, "myvar", false, 0}, // Lowercase fails
                {ActionKind::Lookup, 0, "MyVar", true, 1}   // Exact match succeeds
            }
        },

        // 8. Deep Nesting: Ensuring the `while(S)` loop can traverse many levels
        ScopeTestParam{
            "DeepNestingLookup",
            {
                {ActionKind::Insert, 0, "deepVar", true, 1},
                // Look up from 5 levels deep
                {ActionKind::Lookup, 5, "deepVar", true, 1}
            }
        },

        // 9. Multi-Shadowing: A variable shadowed multiple times down the chain
        ScopeTestParam{
            "MultiLevelShadowing",
            {
                {ActionKind::Insert, 0, "x", true, 1}, // Global 'x'
                {ActionKind::Insert, 1, "x", true, 2}, // Local 'x'
                {ActionKind::Insert, 2, "x", true, 3}, // Nested 'x'

                {ActionKind::Lookup, 2, "x", true, 3}, // Level 2 sees its own
                {ActionKind::Lookup, 1, "x", true, 2}, // Level 1 sees its own
                {ActionKind::Lookup, 0, "x", true, 1}  // Level 0 sees its own
            }
        },

        // 10. Multiple Distinct Variables in the same scope
        ScopeTestParam{
            "MultipleVariablesSameScope",
            {
                {ActionKind::Insert, 1, "varA", true, 1},
                {ActionKind::Insert, 1, "varB", true, 2},
                {ActionKind::Insert, 1, "varC", true, 3},

                {ActionKind::Lookup, 1, "varB", true, 2},
                {ActionKind::Lookup, 2, "varA", true, 1}, // Child sees parent's varA
                {ActionKind::Lookup, 2, "varC", true, 3}  // Child sees parent's varC
            }
        }
    ),
    [](const ::testing::TestParamInfo<ScopeTestParam> &info) { return info.param.TestName; }
);

} // namespace