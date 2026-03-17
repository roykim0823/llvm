#include "Sema.h"
#include "Parser.h"
#include "Lexer.h"
#include "AST.h"
#include <gtest/gtest.h>
#include <string>

// Visitor that records the exact order nodes are visited
class TraversalLogger : public ASTVisitor {
public:
    std::string Log;

    void visit(Factor &Node) override {
        Log += (Node.getKind() == Factor::Ident ? "Id(" : "Num(");
        Log += Node.getVal().str() + ") ";
    }

    void visit(BinaryOp &Node) override {
        Log += "BinOp ";
        if (Node.getLeft()) Node.getLeft()->accept(*this);
        if (Node.getRight()) Node.getRight()->accept(*this);
    }

    void visit(WithDecl &Node) override {
        Log += "With ";
        if (Node.getExpr()) Node.getExpr()->accept(*this);
    }
};
//---------------------------------------------

struct ASTTraversalCase {
    std::string input;
    std::string expectedTraversal;
};

class ASTTraversalTest : public ::testing::TestWithParam<ASTTraversalCase> {};

TEST_P(ASTTraversalTest, VerifiesTreeVisitationOrder) {
    const auto& param = GetParam();

    llvm::StringRef buffer(param.input);
    Lexer lexer(buffer);
    Parser parser(lexer);
    auto tree = parser.parse();

    ASSERT_FALSE(parser.hasError()) << "Parser failed on valid input: " << param.input;
    ASSERT_NE(tree, nullptr);

    // 1. Traverse and log
    TraversalLogger logger;
    tree->accept(logger);

    // 2. Assert the traversal path matches
    EXPECT_EQ(logger.Log, param.expectedTraversal);
}

INSTANTIATE_TEST_SUITE_P(
    ASTStructure,
    ASTTraversalTest,
    ::testing::Values(
        // Pre-order traversal checks: Root -> Left -> Right
        ASTTraversalCase{"a + 5", "BinOp Id(a) Num(5) "},
        ASTTraversalCase{"1 * 2 + 3", "BinOp BinOp Num(1) Num(2) Num(3) "},
        ASTTraversalCase{"with x : x - 1", "With BinOp Id(x) Num(1) "},
        ASTTraversalCase{"with a, b : a * b", "With BinOp Id(a) Id(b) "}
    )
);