#include "TestFramework.hpp"

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

namespace
{
    Program parseOne(const std::string& source)
    {
        Lexer lexer(source);
        Parser parser(lexer.lex());
        return parser.parseProgram();
    }
} // namespace

TEST("Parser builds assignment AST with correct operator precedence")
{
    auto program = parseOne("x = 1 + 2 * 3");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    EXPECT_TRUE(assignment != nullptr);
    EXPECT_EQ(assignment->name, "x");

    auto* add = dynamic_cast<BinaryExpr*>(assignment->value.get());
    EXPECT_TRUE(add != nullptr);
    EXPECT_EQ(add->op, TokenKind::Plus);

    auto* multiply = dynamic_cast<BinaryExpr*>(add->right.get());
    EXPECT_TRUE(multiply != nullptr);
    EXPECT_EQ(multiply->op, TokenKind::Star);
}

TEST("Parser builds if-expression AST with condition and both branches as blocks")
{
    auto program = parseOne("x = if 1 < 2 { 10 } else { 20 }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    EXPECT_TRUE(assignment != nullptr);

    auto* ifExpr = dynamic_cast<IfExpr*>(assignment->value.get());
    EXPECT_TRUE(ifExpr != nullptr);

    auto* condition = dynamic_cast<BinaryExpr*>(ifExpr->condition.get());
    EXPECT_TRUE(condition != nullptr);
    EXPECT_EQ(condition->op, TokenKind::Less);

    auto* thenBlock = dynamic_cast<BlockExpr*>(ifExpr->thenBranch.get());
    EXPECT_TRUE(thenBlock != nullptr);
    auto* thenValue = dynamic_cast<IntegerExpr*>(thenBlock->result.get());
    EXPECT_TRUE(thenValue != nullptr);
    EXPECT_EQ(thenValue->value, 10);

    auto* elseBlock = dynamic_cast<BlockExpr*>(ifExpr->elseBranch.get());
    EXPECT_TRUE(elseBlock != nullptr);
    auto* elseValue = dynamic_cast<IntegerExpr*>(elseBlock->result.get());
    EXPECT_TRUE(elseValue != nullptr);
    EXPECT_EQ(elseValue->value, 20);
}

TEST("Parser desugars an if with no else into an empty unit block")
{
    auto program = parseOne("x = if true { 1 }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* ifExpr = dynamic_cast<IfExpr*>(assignment->value.get());
    EXPECT_TRUE(ifExpr != nullptr);

    auto* elseBlock = dynamic_cast<BlockExpr*>(ifExpr->elseBranch.get());
    EXPECT_TRUE(elseBlock != nullptr);
    EXPECT_TRUE(elseBlock->statements.empty());
    EXPECT_TRUE(elseBlock->result == nullptr);
}

TEST("Parser builds else-if chains as nested if-expressions")
{
    auto program = parseOne("x = if 1 == 2 { 1 } else if 3 == 4 { 2 } else { 3 }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* outerIf = dynamic_cast<IfExpr*>(assignment->value.get());
    EXPECT_TRUE(outerIf != nullptr);

    auto* innerIf = dynamic_cast<IfExpr*>(outerIf->elseBranch.get());
    EXPECT_TRUE(innerIf != nullptr);

    auto* innerElseBlock = dynamic_cast<BlockExpr*>(innerIf->elseBranch.get());
    EXPECT_TRUE(innerElseBlock != nullptr);
    auto* innerElseValue = dynamic_cast<IntegerExpr*>(innerElseBlock->result.get());
    EXPECT_TRUE(innerElseValue != nullptr);
    EXPECT_EQ(innerElseValue->value, 3);
}

TEST("Parser treats a bare name in an if-condition as a name, not a struct literal")
{
    auto program = parseOne("x = if flag { 1 } else { 2 }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* ifExpr = dynamic_cast<IfExpr*>(assignment->value.get());
    EXPECT_TRUE(ifExpr != nullptr);

    auto* condition = dynamic_cast<NameExpr*>(ifExpr->condition.get());
    EXPECT_TRUE(condition != nullptr);
    EXPECT_EQ(condition->name, "flag");
}

TEST("Parser builds string and boolean literal AST nodes")
{
    auto program = parseOne(R"(x = "hi")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    EXPECT_TRUE(assignment != nullptr);

    auto* string = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(string != nullptr);
    EXPECT_EQ(string->value, "hi");
}

TEST("Parser builds a function declaration with a block body")
{
    auto program = parseOne("square(x: i32) -> i32 { x * x }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->name, "square");
    EXPECT_EQ(function->params.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(function->params[0].name, "x");
    EXPECT_EQ(function->params[0].type, "i32");
    EXPECT_TRUE(function->returnType.has_value());
    EXPECT_EQ(*function->returnType, "i32");

    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    EXPECT_TRUE(body != nullptr);
    auto* multiply = dynamic_cast<BinaryExpr*>(body->result.get());
    EXPECT_TRUE(multiply != nullptr);
    EXPECT_EQ(multiply->op, TokenKind::Star);
}

TEST("Parser normalizes a fat-arrow function body into an explicit return")
{
    // `=>` is sugar for `{ return expr }`, not for "the block's result" -
    // functions require an explicit return (docs/language/0027-explicit-return.md).
    auto program = parseOne("square(x: i32) -> i32 => x * x");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);

    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    EXPECT_TRUE(body != nullptr);
    EXPECT_TRUE(body->result == nullptr);
    EXPECT_EQ(body->statements.size(), static_cast<std::size_t>(1));

    auto* returnStmt = dynamic_cast<ReturnStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(returnStmt != nullptr);
    auto* multiply = dynamic_cast<BinaryExpr*>(returnStmt->value.get());
    EXPECT_TRUE(multiply != nullptr);
}

TEST("Parser allows an omitted return type")
{
    auto program = parseOne("noop() { 1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_TRUE(!function->returnType.has_value());
}

TEST("Parser parses pub (discarded) and captures a capability-prefixed parameter")
{
    auto program = parseOne("pub update(write user: i32) -> i32 { user }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->name, "update");
    EXPECT_EQ(function->params[0].name, "user");
    EXPECT_EQ(function->params[0].type, "i32");
    EXPECT_TRUE(function->params[0].declaredCapability.has_value());
    EXPECT_TRUE(*function->params[0].declaredCapability == Capability::Write);
}

TEST("Parser leaves a parameter's capability unset when omitted")
{
    auto program = parseOne("f(x: i32) -> i32 { x }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(!function->params[0].declaredCapability.has_value());
}

TEST("Parser builds a struct declaration with newline-separated fields")
{
    auto program = parseOne("struct Point { x: i32  y: i32 }");

    auto* structDecl = dynamic_cast<StructDecl*>(program.items.at(0).get());
    EXPECT_TRUE(structDecl != nullptr);
    EXPECT_EQ(structDecl->name, "Point");
    EXPECT_EQ(structDecl->fields.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(structDecl->fields[0].name, "x");
    EXPECT_EQ(structDecl->fields[0].type, "i32");
    EXPECT_EQ(structDecl->fields[1].name, "y");
}

TEST("Parser builds a struct literal with named fields")
{
    auto program = parseOne("p = Point { x: 1  y: 2 }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* literal = dynamic_cast<StructLiteralExpr*>(assignment->value.get());
    EXPECT_TRUE(literal != nullptr);
    EXPECT_EQ(literal->typeName, "Point");
    EXPECT_EQ(literal->fields.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(literal->fields[0].first, "x");
    auto* xValue = dynamic_cast<IntegerExpr*>(literal->fields[0].second.get());
    EXPECT_TRUE(xValue != nullptr);
    EXPECT_EQ(xValue->value, 1);
}

TEST("Parser builds a struct literal with optional commas")
{
    auto program = parseOne("p = Point { x: 1, y: 2 }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* literal = dynamic_cast<StructLiteralExpr*>(assignment->value.get());
    EXPECT_TRUE(literal != nullptr);
    EXPECT_EQ(literal->fields.size(), static_cast<std::size_t>(2));
}

TEST("Parser builds shorthand struct literal fields as name expressions")
{
    auto program = parseOne("p = Point { x, y }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* literal = dynamic_cast<StructLiteralExpr*>(assignment->value.get());
    EXPECT_TRUE(literal != nullptr);
    EXPECT_EQ(literal->fields[0].first, "x");
    auto* xValue = dynamic_cast<NameExpr*>(literal->fields[0].second.get());
    EXPECT_TRUE(xValue != nullptr);
    EXPECT_EQ(xValue->name, "x");
}

TEST("Parser builds a call expression with arguments")
{
    auto program = parseOne("y = add(1, 2)");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<CallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "add");
    EXPECT_EQ(call->arguments.size(), static_cast<std::size_t>(2));
}

TEST("Parser builds a field access chain")
{
    auto program = parseOne("z = p.x");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* field = dynamic_cast<FieldExpr*>(assignment->value.get());
    EXPECT_TRUE(field != nullptr);
    EXPECT_EQ(field->field, "x");
    auto* object = dynamic_cast<NameExpr*>(field->object.get());
    EXPECT_TRUE(object != nullptr);
    EXPECT_EQ(object->name, "p");
}

TEST("Parser builds a return statement inside a block")
{
    auto program = parseOne("f() -> i32 { if true { return 1 } 2 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    EXPECT_EQ(body->statements.size(), static_cast<std::size_t>(1));

    auto* exprStmt = dynamic_cast<ExprStmt*>(body->statements[0].get());
    EXPECT_TRUE(exprStmt != nullptr);
    auto* ifExpr = dynamic_cast<IfExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(ifExpr != nullptr);

    auto* thenBlock = dynamic_cast<BlockExpr*>(ifExpr->thenBranch.get());
    auto* returnStmt = dynamic_cast<ReturnStmt*>(thenBlock->statements.at(0).get());
    EXPECT_TRUE(returnStmt != nullptr);
    auto* returnValue = dynamic_cast<IntegerExpr*>(returnStmt->value.get());
    EXPECT_TRUE(returnValue != nullptr);
    EXPECT_EQ(returnValue->value, 1);
}

TEST("Parser parses an optional type annotation on a local assignment")
{
    auto program = parseOne("port: i32 = 443");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    EXPECT_TRUE(assignment != nullptr);
    EXPECT_TRUE(assignment->declaredType.has_value());
    EXPECT_EQ(*assignment->declaredType, "i32");
}

TEST("Parser builds a field-assignment statement")
{
    auto program = parseOne("f(p: Point) -> i32 { p.x = 5  1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* fieldAssign = dynamic_cast<FieldAssignStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(fieldAssign != nullptr);
    EXPECT_EQ(fieldAssign->field, "x");

    auto* object = dynamic_cast<NameExpr*>(fieldAssign->object.get());
    EXPECT_TRUE(object != nullptr);
    EXPECT_EQ(object->name, "p");

    auto* value = dynamic_cast<IntegerExpr*>(fieldAssign->value.get());
    EXPECT_TRUE(value != nullptr);
    EXPECT_EQ(value->value, 5);
}

TEST("Parser builds a field-assignment statement for a nested field chain")
{
    auto program = parseOne("f(a: A) -> i32 { a.b.c = 1  1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* fieldAssign = dynamic_cast<FieldAssignStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(fieldAssign != nullptr);
    EXPECT_EQ(fieldAssign->field, "c");

    auto* nestedObject = dynamic_cast<FieldExpr*>(fieldAssign->object.get());
    EXPECT_TRUE(nestedObject != nullptr);
    EXPECT_EQ(nestedObject->field, "b");
}

TEST("Parser builds increment/decrement statements for a name and a field target")
{
    auto program = parseOne("f(p: Point, n: i32) -> i32 { p.x++  n--  1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    EXPECT_EQ(body->statements.size(), static_cast<std::size_t>(2));

    auto* fieldInc = dynamic_cast<IncDecStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(fieldInc != nullptr);
    EXPECT_TRUE(fieldInc->increment);
    EXPECT_TRUE(dynamic_cast<FieldExpr*>(fieldInc->target.get()) != nullptr);

    auto* nameDec = dynamic_cast<IncDecStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(nameDec != nullptr);
    EXPECT_TRUE(!nameDec->increment);
    EXPECT_TRUE(dynamic_cast<NameExpr*>(nameDec->target.get()) != nullptr);
}

TEST("Parser rejects an invalid assignment target")
{
    EXPECT_THROWS(parseOne("f() -> i32 { 1 + 2 = 5 }"));
}

TEST("Parser rejects an invalid increment target")
{
    EXPECT_THROWS(parseOne("f() -> i32 { (1 + 2)++  3 }"));
}
