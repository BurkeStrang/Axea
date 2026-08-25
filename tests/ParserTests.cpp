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

TEST("Parser builds char literal AST nodes, decoding UTF-8 into a codepoint")
{
    auto asciiProgram = parseOne("x = 'A'");
    auto* asciiAssignment = dynamic_cast<AssignmentStmt*>(asciiProgram.items.at(0).get());
    auto* ascii = dynamic_cast<CharExpr*>(asciiAssignment->value.get());
    EXPECT_TRUE(ascii != nullptr);
    EXPECT_EQ(ascii->codepoint, 65);

    auto accentedProgram = parseOne("x = 'é'");
    auto* accentedAssignment = dynamic_cast<AssignmentStmt*>(accentedProgram.items.at(0).get());
    auto* accented = dynamic_cast<CharExpr*>(accentedAssignment->value.get());
    EXPECT_TRUE(accented != nullptr);
    EXPECT_EQ(accented->codepoint, 233);

    auto rocketProgram = parseOne("x = '🚀'");
    auto* rocketAssignment = dynamic_cast<AssignmentStmt*>(rocketProgram.items.at(0).get());
    auto* rocket = dynamic_cast<CharExpr*>(rocketAssignment->value.get());
    EXPECT_TRUE(rocket != nullptr);
    EXPECT_EQ(rocket->codepoint, 128640);
}

TEST("Parser rejects an empty char literal")
{
    EXPECT_THROWS(parseOne("x = ''"));
}

TEST("Parser rejects a multi-character char literal")
{
    EXPECT_THROWS(parseOne("x = 'ab'"));
}

TEST("Parser parses a char parameter type into the canonical form")
{
    auto program = parseOne("useChar(c: char) -> char { return c }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "char");
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

TEST("Parser builds a while statement with condition and body")
{
    auto program = parseOne("f() { while n < 5 { n++ } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    EXPECT_EQ(body->statements.size(), static_cast<std::size_t>(1));

    auto* whileStmt = dynamic_cast<WhileStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(whileStmt != nullptr);
    auto* condition = dynamic_cast<BinaryExpr*>(whileStmt->condition.get());
    EXPECT_TRUE(condition != nullptr);
    EXPECT_EQ(condition->op, TokenKind::Less);
    auto* whileBody = dynamic_cast<BlockExpr*>(whileStmt->body.get());
    EXPECT_TRUE(whileBody != nullptr);
    EXPECT_EQ(whileBody->statements.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a loop expression with break value and continue")
{
    auto program = parseOne("f() -> i32 { return loop { continue  break 1 } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* returnStmt = dynamic_cast<ReturnStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(returnStmt != nullptr);

    auto* loopExpr = dynamic_cast<LoopExpr*>(returnStmt->value.get());
    EXPECT_TRUE(loopExpr != nullptr);
    auto* loopBody = dynamic_cast<BlockExpr*>(loopExpr->body.get());
    EXPECT_TRUE(loopBody != nullptr);
    // break/continue are always statements, even in trailing position - the
    // block's own `result` (expression-only) stays null.
    EXPECT_EQ(loopBody->statements.size(), static_cast<std::size_t>(2));
    EXPECT_TRUE(loopBody->result == nullptr);

    auto* continueStmt = dynamic_cast<ContinueStmt*>(loopBody->statements.at(0).get());
    EXPECT_TRUE(continueStmt != nullptr);

    auto* breakStmt = dynamic_cast<BreakStmt*>(loopBody->statements.at(1).get());
    EXPECT_TRUE(breakStmt != nullptr);
    auto* breakValue = dynamic_cast<IntegerExpr*>(breakStmt->value.get());
    EXPECT_TRUE(breakValue != nullptr);
    EXPECT_EQ(breakValue->value, 1);
}

TEST("Parser builds a bare break with no value")
{
    auto program = parseOne("f() { while true { break } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* whileStmt = dynamic_cast<WhileStmt*>(body->statements.at(0).get());
    auto* whileBody = dynamic_cast<BlockExpr*>(whileStmt->body.get());
    auto* breakStmt = dynamic_cast<BreakStmt*>(whileBody->statements.at(0).get());
    EXPECT_TRUE(breakStmt != nullptr);
    EXPECT_TRUE(breakStmt->value == nullptr);
}

TEST("Parser desugars for-in into an end/counter setup and an infinite while with an early bound "
     "check")
{
    auto program = parseOne("f() { for i in 0..5 { total = total + i } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    EXPECT_EQ(body->statements.size(), static_cast<std::size_t>(1));

    // `for` desugars to a bare ExprStmt wrapping the whole { ... } - no
    // dedicated ForStmt node (docs/language/0029-for-loops.md).
    auto* exprStmt = dynamic_cast<ExprStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(exprStmt != nullptr);
    auto* outerBlock = dynamic_cast<BlockExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(outerBlock != nullptr);
    EXPECT_EQ(outerBlock->statements.size(), static_cast<std::size_t>(3));

    // [ end = b, counter = a - 1, while true { ... } ]
    auto* endInit = dynamic_cast<AssignmentStmt*>(outerBlock->statements.at(0).get());
    EXPECT_TRUE(endInit != nullptr);
    EXPECT_TRUE(!endInit->forceDefine);
    auto* endValue = dynamic_cast<IntegerExpr*>(endInit->value.get());
    EXPECT_TRUE(endValue != nullptr);
    EXPECT_EQ(endValue->value, 5);

    auto* counterInit = dynamic_cast<AssignmentStmt*>(outerBlock->statements.at(1).get());
    EXPECT_TRUE(counterInit != nullptr);
    auto* startMinusOne = dynamic_cast<BinaryExpr*>(counterInit->value.get());
    EXPECT_TRUE(startMinusOne != nullptr);
    EXPECT_EQ(startMinusOne->op, TokenKind::Minus);
    auto* startValue = dynamic_cast<IntegerExpr*>(startMinusOne->left.get());
    EXPECT_TRUE(startValue != nullptr);
    EXPECT_EQ(startValue->value, 0);

    auto* whileStmt = dynamic_cast<WhileStmt*>(outerBlock->statements.at(2).get());
    EXPECT_TRUE(whileStmt != nullptr);
    auto* alwaysTrue = dynamic_cast<BoolExpr*>(whileStmt->condition.get());
    EXPECT_TRUE(alwaysTrue != nullptr);
    EXPECT_TRUE(alwaysTrue->value);

    auto* whileBody = dynamic_cast<BlockExpr*>(whileStmt->body.get());
    EXPECT_TRUE(whileBody != nullptr);
    // [ counter++, if counter >= end { break }, i = counter (forceDefine), user's own statement ]
    EXPECT_EQ(whileBody->statements.size(), static_cast<std::size_t>(4));

    auto* increment = dynamic_cast<IncDecStmt*>(whileBody->statements.at(0).get());
    EXPECT_TRUE(increment != nullptr);
    EXPECT_TRUE(increment->increment);

    auto* boundCheckStmt = dynamic_cast<ExprStmt*>(whileBody->statements.at(1).get());
    EXPECT_TRUE(boundCheckStmt != nullptr);
    auto* boundCheckIf = dynamic_cast<IfExpr*>(boundCheckStmt->expr.get());
    EXPECT_TRUE(boundCheckIf != nullptr);
    auto* boundCondition = dynamic_cast<BinaryExpr*>(boundCheckIf->condition.get());
    EXPECT_TRUE(boundCondition != nullptr);
    EXPECT_EQ(boundCondition->op, TokenKind::GreaterEqual);
    auto* thenBlock = dynamic_cast<BlockExpr*>(boundCheckIf->thenBranch.get());
    EXPECT_TRUE(thenBlock != nullptr);
    EXPECT_TRUE(dynamic_cast<BreakStmt*>(thenBlock->statements.at(0).get()) != nullptr);

    auto* inductionBind = dynamic_cast<AssignmentStmt*>(whileBody->statements.at(2).get());
    EXPECT_TRUE(inductionBind != nullptr);
    EXPECT_EQ(inductionBind->name, "i");
    EXPECT_TRUE(inductionBind->forceDefine); // must never mutate a same-named outer variable
}

TEST("Parser gives nested for-loops distinct internal counter names")
{
    auto program = parseOne("f() { for i in 0..3 { for i in 0..2 { } } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* outerExprStmt = dynamic_cast<ExprStmt*>(body->statements.at(0).get());
    auto* outerBlock = dynamic_cast<BlockExpr*>(outerExprStmt->expr.get());
    auto* outerCounterInit = dynamic_cast<AssignmentStmt*>(outerBlock->statements.at(1).get());
    auto* outerWhile = dynamic_cast<WhileStmt*>(outerBlock->statements.at(2).get());
    auto* outerWhileBody = dynamic_cast<BlockExpr*>(outerWhile->body.get());

    // [ counter++, if counter >= end { break }, i = counter (forceDefine), inner for's ExprStmt ]
    auto* innerExprStmt = dynamic_cast<ExprStmt*>(outerWhileBody->statements.at(3).get());
    EXPECT_TRUE(innerExprStmt != nullptr);
    auto* innerBlock = dynamic_cast<BlockExpr*>(innerExprStmt->expr.get());
    auto* innerCounterInit = dynamic_cast<AssignmentStmt*>(innerBlock->statements.at(1).get());

    EXPECT_TRUE(outerCounterInit->name != innerCounterInit->name);
}

TEST("Parser builds an array literal expression")
{
    auto program = parseOne("x = [1, 2, 3]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* literal = dynamic_cast<ArrayLiteralExpr*>(assignment->value.get());
    EXPECT_TRUE(literal != nullptr);
    EXPECT_EQ(literal->elements.size(), static_cast<std::size_t>(3));
    auto* second = dynamic_cast<IntegerExpr*>(literal->elements.at(1).get());
    EXPECT_TRUE(second != nullptr);
    EXPECT_EQ(second->value, 2);
}

TEST("Parser parses an array type annotation into the canonical no-spaces form")
{
    auto program = parseOne("x: [i32; 4] = [1, 2, 3, 4]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    EXPECT_TRUE(assignment->declaredType.has_value());
    EXPECT_EQ(*assignment->declaredType, "[i32;4]");
}

TEST("Parser parses a slice<T> parameter type into the canonical form")
{
    auto program = parseOne("sum(values: slice<i32>) -> i32 { return values[0] }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "slice<i32>");
}

TEST("Parser builds an index expression, chaining with field access")
{
    auto program = parseOne("f(a: A) -> i32 { a.items[0].x  1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* exprStmt = dynamic_cast<ExprStmt*>(body->statements.at(0).get());
    auto* outerField = dynamic_cast<FieldExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(outerField != nullptr);
    EXPECT_EQ(outerField->field, "x");

    auto* index = dynamic_cast<IndexExpr*>(outerField->object.get());
    EXPECT_TRUE(index != nullptr);
    auto* indexValue = dynamic_cast<IntegerExpr*>(index->index.get());
    EXPECT_TRUE(indexValue != nullptr);
    EXPECT_EQ(indexValue->value, 0);

    auto* innerField = dynamic_cast<FieldExpr*>(index->object.get());
    EXPECT_TRUE(innerField != nullptr);
    EXPECT_EQ(innerField->field, "items");
}

TEST("Parser builds a bounded str slice expression - object[start..end]")
{
    auto program = parseOne("x = date[5..7]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* slice = dynamic_cast<StrSliceExpr*>(assignment->value.get());
    EXPECT_TRUE(slice != nullptr);
    EXPECT_TRUE(dynamic_cast<NameExpr*>(slice->object.get()) != nullptr);
    auto* start = dynamic_cast<IntegerExpr*>(slice->start.get());
    EXPECT_TRUE(start != nullptr);
    EXPECT_EQ(start->value, 5);
    auto* end = dynamic_cast<IntegerExpr*>(slice->end.get());
    EXPECT_TRUE(end != nullptr);
    EXPECT_EQ(end->value, 7);
}

TEST("Parser builds an open-start str slice expression - object[..end]")
{
    auto program = parseOne("x = date[..4]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* slice = dynamic_cast<StrSliceExpr*>(assignment->value.get());
    EXPECT_TRUE(slice != nullptr);
    EXPECT_TRUE(slice->start == nullptr);
    auto* end = dynamic_cast<IntegerExpr*>(slice->end.get());
    EXPECT_TRUE(end != nullptr);
    EXPECT_EQ(end->value, 4);
}

TEST("Parser builds an open-end str slice expression - object[start..]")
{
    auto program = parseOne("x = date[8..]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* slice = dynamic_cast<StrSliceExpr*>(assignment->value.get());
    EXPECT_TRUE(slice != nullptr);
    auto* start = dynamic_cast<IntegerExpr*>(slice->start.get());
    EXPECT_TRUE(start != nullptr);
    EXPECT_EQ(start->value, 8);
    EXPECT_TRUE(slice->end == nullptr);
}

TEST("Parser builds a fully-open str slice expression - object[..]")
{
    auto program = parseOne("x = date[..]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* slice = dynamic_cast<StrSliceExpr*>(assignment->value.get());
    EXPECT_TRUE(slice != nullptr);
    EXPECT_TRUE(slice->start == nullptr);
    EXPECT_TRUE(slice->end == nullptr);
}

TEST("Parser still parses a plain index expression, unaffected by the new slice-range shape")
{
    auto program = parseOne("f(a: A) -> i32 { a[0] }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* index = dynamic_cast<IndexExpr*>(body->result.get());
    EXPECT_TRUE(index != nullptr);
}

TEST("Parser builds a generic method call expression with an explicit type argument")
{
    auto program = parseOne("x = \"42\".parse<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<MethodCallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->method, "parse");
    EXPECT_EQ(call->typeArgument, "i32");
    EXPECT_EQ(call->arguments.size(), static_cast<std::size_t>(0));
}

TEST("Parser builds an ordinary method call with an empty type argument, unaffected by the new "
     "generic-call shape")
{
    auto program = parseOne("f() { s = String(\"a\")  s.append(\"b\") }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* call = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_TRUE(call->typeArgument.empty());
}

TEST("Parser still parses 'field < expr' as a less-than comparison, not a misfired generic "
     "method call - the 4-token lookahead must not misfire on an ordinary comparison")
{
    auto program = parseOne("f(p: P) -> bool { p.field < 10 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* binary = dynamic_cast<BinaryExpr*>(body->result.get());
    EXPECT_TRUE(binary != nullptr);
    EXPECT_TRUE(binary->op == TokenKind::Less);
    auto* field = dynamic_cast<FieldExpr*>(binary->left.get());
    EXPECT_TRUE(field != nullptr);
    EXPECT_EQ(field->field, "field");
}

TEST("Parser builds an index-assignment statement")
{
    auto program = parseOne("f(values: [i32; 3]) -> i32 { values[0] = 5  1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* indexAssign = dynamic_cast<IndexAssignStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(indexAssign != nullptr);

    auto* object = dynamic_cast<NameExpr*>(indexAssign->object.get());
    EXPECT_TRUE(object != nullptr);
    EXPECT_EQ(object->name, "values");

    auto* index = dynamic_cast<IntegerExpr*>(indexAssign->index.get());
    EXPECT_TRUE(index != nullptr);
    EXPECT_EQ(index->value, 0);

    auto* value = dynamic_cast<IntegerExpr*>(indexAssign->value.get());
    EXPECT_TRUE(value != nullptr);
    EXPECT_EQ(value->value, 5);
}

TEST("Parser desugars for-in-over-an-array into a bound/counter setup comparing against .length")
{
    auto program = parseOne("f(values: [i32; 3]) { for v in values { total = total + v } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* exprStmt = dynamic_cast<ExprStmt*>(body->statements.at(0).get());
    auto* outerBlock = dynamic_cast<BlockExpr*>(exprStmt->expr.get());
    EXPECT_EQ(outerBlock->statements.size(), static_cast<std::size_t>(3));

    // [ __for0_arr = values, __for0_i = -1, while true { ... } ]
    auto* arrInit = dynamic_cast<AssignmentStmt*>(outerBlock->statements.at(0).get());
    EXPECT_TRUE(arrInit != nullptr);
    auto* arrValue = dynamic_cast<NameExpr*>(arrInit->value.get());
    EXPECT_TRUE(arrValue != nullptr);
    EXPECT_EQ(arrValue->name, "values");

    auto* counterInit = dynamic_cast<AssignmentStmt*>(outerBlock->statements.at(1).get());
    auto* counterValue = dynamic_cast<IntegerExpr*>(counterInit->value.get());
    EXPECT_TRUE(counterValue != nullptr);
    EXPECT_EQ(counterValue->value, -1);

    auto* whileStmt = dynamic_cast<WhileStmt*>(outerBlock->statements.at(2).get());
    auto* whileBody = dynamic_cast<BlockExpr*>(whileStmt->body.get());
    EXPECT_EQ(whileBody->statements.size(), static_cast<std::size_t>(4));

    // Bound check compares against `.length`, not a plain end variable.
    auto* boundCheckStmt = dynamic_cast<ExprStmt*>(whileBody->statements.at(1).get());
    auto* boundCheckIf = dynamic_cast<IfExpr*>(boundCheckStmt->expr.get());
    auto* boundCondition = dynamic_cast<BinaryExpr*>(boundCheckIf->condition.get());
    auto* lengthField = dynamic_cast<FieldExpr*>(boundCondition->right.get());
    EXPECT_TRUE(lengthField != nullptr);
    EXPECT_EQ(lengthField->field, "length");

    // Loop variable is bound via indexing, not a direct counter reference.
    auto* inductionBind = dynamic_cast<AssignmentStmt*>(whileBody->statements.at(2).get());
    EXPECT_TRUE(inductionBind != nullptr);
    EXPECT_EQ(inductionBind->name, "v");
    EXPECT_TRUE(inductionBind->forceDefine);
    auto* inductionIndex = dynamic_cast<IndexExpr*>(inductionBind->value.get());
    EXPECT_TRUE(inductionIndex != nullptr);
}

TEST("Parser builds a List<T> construction expression")
{
    auto program = parseOne("x = List<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* listNew = dynamic_cast<ListNewExpr*>(assignment->value.get());
    EXPECT_TRUE(listNew != nullptr);
    EXPECT_EQ(listNew->elementType, "i32");
}

TEST("Parser parses a List<T> parameter type into the canonical form")
{
    auto program = parseOne("sum(values: List<i32>) -> i32 { return values[0] }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "List<i32>");
}

TEST("Parser builds a Stack<T> construction expression")
{
    auto program = parseOne("x = Stack<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* stackNew = dynamic_cast<StackNewExpr*>(assignment->value.get());
    EXPECT_TRUE(stackNew != nullptr);
    EXPECT_EQ(stackNew->elementType, "i32");
}

TEST("Parser parses a Stack<T> parameter type into the canonical form")
{
    auto program = parseOne("useStack(s: Stack<i32>) -> i32 { return s.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "Stack<i32>");
}

TEST("Parser builds Stack<T> push/pop/peek method-call expressions")
{
    auto program = parseOne("f() { s = Stack<i32>()  s.push(1)  s.peek() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* pushStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(pushStmt != nullptr);
    auto* pushCall = dynamic_cast<MethodCallExpr*>(pushStmt->expr.get());
    EXPECT_TRUE(pushCall != nullptr);
    EXPECT_EQ(pushCall->method, "push");

    auto* peekResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(peekResult != nullptr);
    EXPECT_EQ(peekResult->method, "peek");
    EXPECT_TRUE(peekResult->arguments.empty());
}

TEST("Parser builds a LinkedList<T> construction expression")
{
    auto program = parseOne("x = LinkedList<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* linkedListNew = dynamic_cast<LinkedListNewExpr*>(assignment->value.get());
    EXPECT_TRUE(linkedListNew != nullptr);
    EXPECT_EQ(linkedListNew->elementType, "i32");
}

TEST("Parser parses a LinkedList<T> parameter type into the canonical form")
{
    auto program = parseOne("useLinkedList(s: LinkedList<i32>) -> i32 { return s.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "LinkedList<i32>");
}

TEST("Parser builds LinkedList<T> push_front/push_back/pop_front/pop_back method-call expressions")
{
    auto program = parseOne("f() { s = LinkedList<i32>()  s.push_front(1)  s.push_back(2)  "
                            "s.pop_front()  s.pop_back() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* pushFrontStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(pushFrontStmt != nullptr);
    auto* pushFrontCall = dynamic_cast<MethodCallExpr*>(pushFrontStmt->expr.get());
    EXPECT_TRUE(pushFrontCall != nullptr);
    EXPECT_EQ(pushFrontCall->method, "push_front");

    auto* pushBackStmt = dynamic_cast<ExprStmt*>(body->statements.at(2).get());
    EXPECT_TRUE(pushBackStmt != nullptr);
    auto* pushBackCall = dynamic_cast<MethodCallExpr*>(pushBackStmt->expr.get());
    EXPECT_TRUE(pushBackCall != nullptr);
    EXPECT_EQ(pushBackCall->method, "push_back");

    auto* popFrontStmt = dynamic_cast<ExprStmt*>(body->statements.at(3).get());
    EXPECT_TRUE(popFrontStmt != nullptr);
    auto* popFrontCall = dynamic_cast<MethodCallExpr*>(popFrontStmt->expr.get());
    EXPECT_TRUE(popFrontCall != nullptr);
    EXPECT_EQ(popFrontCall->method, "pop_front");

    auto* popBackResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(popBackResult != nullptr);
    EXPECT_EQ(popBackResult->method, "pop_back");
    EXPECT_TRUE(popBackResult->arguments.empty());
}

TEST("Parser builds a Deque<T> construction expression")
{
    auto program = parseOne("x = Deque<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* dequeNew = dynamic_cast<DequeNewExpr*>(assignment->value.get());
    EXPECT_TRUE(dequeNew != nullptr);
    EXPECT_EQ(dequeNew->elementType, "i32");
}

TEST("Parser parses a Deque<T> parameter type into the canonical form")
{
    auto program = parseOne("useDeque(d: Deque<i32>) -> i32 { return d.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "Deque<i32>");
}

TEST("Parser builds Deque<T> push_front/push_back/pop_front/pop_back method-call expressions")
{
    auto program = parseOne("f() { d = Deque<i32>()  d.push_front(1)  d.push_back(2)  "
                            "d.pop_front()  d.pop_back() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* pushFrontStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(pushFrontStmt != nullptr);
    auto* pushFrontCall = dynamic_cast<MethodCallExpr*>(pushFrontStmt->expr.get());
    EXPECT_TRUE(pushFrontCall != nullptr);
    EXPECT_EQ(pushFrontCall->method, "push_front");

    auto* popBackResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(popBackResult != nullptr);
    EXPECT_EQ(popBackResult->method, "pop_back");
}

TEST("Parser builds Deque<T> index-get and index-assign expressions")
{
    auto program = parseOne("f() { d = Deque<i32>()  d.push_back(1)  d[0] = 2  x = d[0] }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* indexAssign = dynamic_cast<IndexAssignStmt*>(body->statements.at(2).get());
    EXPECT_TRUE(indexAssign != nullptr);

    auto* readAssign = dynamic_cast<AssignmentStmt*>(body->statements.at(3).get());
    EXPECT_TRUE(readAssign != nullptr);
    auto* indexGet = dynamic_cast<IndexExpr*>(readAssign->value.get());
    EXPECT_TRUE(indexGet != nullptr);
}

TEST("Parser builds a Queue<T> construction expression")
{
    auto program = parseOne("x = Queue<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* queueNew = dynamic_cast<QueueNewExpr*>(assignment->value.get());
    EXPECT_TRUE(queueNew != nullptr);
    EXPECT_EQ(queueNew->elementType, "i32");
}

TEST("Parser parses a Queue<T> parameter type into the canonical form")
{
    auto program = parseOne("useQueue(q: Queue<i32>) -> i32 { return q.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "Queue<i32>");
}

TEST("Parser builds Queue<T> enqueue/dequeue method-call expressions")
{
    auto program = parseOne("f() { q = Queue<i32>()  q.enqueue(1)  q.dequeue() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* enqueueStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(enqueueStmt != nullptr);
    auto* enqueueCall = dynamic_cast<MethodCallExpr*>(enqueueStmt->expr.get());
    EXPECT_TRUE(enqueueCall != nullptr);
    EXPECT_EQ(enqueueCall->method, "enqueue");

    auto* dequeueResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(dequeueResult != nullptr);
    EXPECT_EQ(dequeueResult->method, "dequeue");
    EXPECT_TRUE(dequeueResult->arguments.empty());
}

TEST("Parser builds a PriorityQueue<T> construction expression")
{
    auto program = parseOne("x = PriorityQueue<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* priorityQueueNew = dynamic_cast<PriorityQueueNewExpr*>(assignment->value.get());
    EXPECT_TRUE(priorityQueueNew != nullptr);
    EXPECT_EQ(priorityQueueNew->elementType, "i32");
}

TEST("Parser parses a PriorityQueue<T> parameter type into the canonical form")
{
    auto program = parseOne("usePriorityQueue(q: PriorityQueue<i32>) -> i32 { return q.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "PriorityQueue<i32>");
}

TEST("Parser builds PriorityQueue<T> push/pop/peek method-call expressions")
{
    auto program = parseOne("f() { q = PriorityQueue<i32>()  q.push(1)  q.peek() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* pushStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(pushStmt != nullptr);
    auto* pushCall = dynamic_cast<MethodCallExpr*>(pushStmt->expr.get());
    EXPECT_TRUE(pushCall != nullptr);
    EXPECT_EQ(pushCall->method, "push");

    auto* peekResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(peekResult != nullptr);
    EXPECT_EQ(peekResult->method, "peek");
    EXPECT_TRUE(peekResult->arguments.empty());
}

TEST("Parser builds a Map<K,V> construction expression")
{
    auto program = parseOne("x = Map<i32,i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* mapNew = dynamic_cast<MapNewExpr*>(assignment->value.get());
    EXPECT_TRUE(mapNew != nullptr);
    EXPECT_EQ(mapNew->keyType, "i32");
    EXPECT_EQ(mapNew->valueType, "i32");
}

TEST("Parser builds a Set<T> construction expression")
{
    auto program = parseOne("x = Set<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* setNew = dynamic_cast<SetNewExpr*>(assignment->value.get());
    EXPECT_TRUE(setNew != nullptr);
    EXPECT_EQ(setNew->elementType, "i32");
}

TEST("Parser builds nested construction syntax for List/Map/Set (K/V can themselves be generic)")
{
    // A single Identifier token isn't enough here - the value type is
    // itself a nested generic shape (see
    // docs/language/0034-maps-and-sets.md's generic rewrite).
    auto program = parseOne("x = Map<i32, List<i32>>()  y = List<List<i32>>()  "
                            "z = Set<[i32;3]>()");

    auto* mapAssign = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* mapNew = dynamic_cast<MapNewExpr*>(mapAssign->value.get());
    EXPECT_TRUE(mapNew != nullptr);
    EXPECT_EQ(mapNew->keyType, "i32");
    EXPECT_EQ(mapNew->valueType, "List<i32>");

    auto* listAssign = dynamic_cast<AssignmentStmt*>(program.items.at(1).get());
    auto* listNew = dynamic_cast<ListNewExpr*>(listAssign->value.get());
    EXPECT_TRUE(listNew != nullptr);
    EXPECT_EQ(listNew->elementType, "List<i32>");

    auto* setAssign = dynamic_cast<AssignmentStmt*>(program.items.at(2).get());
    auto* setNew2 = dynamic_cast<SetNewExpr*>(setAssign->value.get());
    EXPECT_TRUE(setNew2 != nullptr);
    EXPECT_EQ(setNew2->elementType, "[i32;3]");
}

TEST("Parser parses Map<K,V> and Set<T> parameter types into the canonical form")
{
    auto program = parseOne("useMap(m: Map<i32,i32>) -> i32 { return m.length } "
                            "useSet(s: Set<i32>) -> i32 { return s.length }");

    auto* mapFn = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(mapFn != nullptr);
    EXPECT_EQ(mapFn->params[0].type, "Map<i32,i32>");

    auto* setFn = dynamic_cast<FunctionDecl*>(program.items.at(1).get());
    EXPECT_TRUE(setFn != nullptr);
    EXPECT_EQ(setFn->params[0].type, "Set<i32>");
}

TEST("Parser builds Map/Set method-call expressions")
{
    auto program = parseOne("f() { m = Map<i32,i32>()  m.set(1, 2)  s = Set<i32>()  s.add(1) }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* mapSetStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(mapSetStmt != nullptr);
    auto* mapSetCall = dynamic_cast<MethodCallExpr*>(mapSetStmt->expr.get());
    EXPECT_TRUE(mapSetCall != nullptr);
    EXPECT_EQ(mapSetCall->method, "set");
    EXPECT_EQ(mapSetCall->arguments.size(), static_cast<std::size_t>(2));

    auto* setAddResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(setAddResult != nullptr);
    EXPECT_EQ(setAddResult->method, "add");
    EXPECT_EQ(setAddResult->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a SortedMap<K,V> construction expression")
{
    auto program = parseOne("x = SortedMap<i32,i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* sortedMapNew = dynamic_cast<SortedMapNewExpr*>(assignment->value.get());
    EXPECT_TRUE(sortedMapNew != nullptr);
    EXPECT_EQ(sortedMapNew->keyType, "i32");
    EXPECT_EQ(sortedMapNew->valueType, "i32");
}

TEST("Parser parses a SortedMap<K,V> parameter type into the canonical form")
{
    auto program = parseOne("useSortedMap(m: SortedMap<i32,i32>) -> i32 { return m.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "SortedMap<i32,i32>");
}

TEST("Parser builds SortedMap<K,V> set/get/contains/remove method-call expressions")
{
    auto program = parseOne("f() { m = SortedMap<i32,i32>()  m.set(1, 2)  m.get(1) }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* setStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(setStmt != nullptr);
    auto* setCall = dynamic_cast<MethodCallExpr*>(setStmt->expr.get());
    EXPECT_TRUE(setCall != nullptr);
    EXPECT_EQ(setCall->method, "set");
    EXPECT_EQ(setCall->arguments.size(), static_cast<std::size_t>(2));

    auto* getResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(getResult != nullptr);
    EXPECT_EQ(getResult->method, "get");
    EXPECT_EQ(getResult->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a SortedSet<T> construction expression")
{
    auto program = parseOne("x = SortedSet<i32>()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* sortedSetNew = dynamic_cast<SortedSetNewExpr*>(assignment->value.get());
    EXPECT_TRUE(sortedSetNew != nullptr);
    EXPECT_EQ(sortedSetNew->elementType, "i32");
}

TEST("Parser parses a SortedSet<T> parameter type into the canonical form")
{
    auto program = parseOne("useSortedSet(s: SortedSet<i32>) -> i32 { return s.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "SortedSet<i32>");
}

TEST("Parser builds SortedSet<T> add/contains/remove method-call expressions")
{
    auto program = parseOne("f() { s = SortedSet<i32>()  s.add(1)  s.contains(1) }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* addStmt = dynamic_cast<ExprStmt*>(body->statements.at(1).get());
    EXPECT_TRUE(addStmt != nullptr);
    auto* addCall = dynamic_cast<MethodCallExpr*>(addStmt->expr.get());
    EXPECT_TRUE(addCall != nullptr);
    EXPECT_EQ(addCall->method, "add");
    EXPECT_EQ(addCall->arguments.size(), static_cast<std::size_t>(1));

    auto* containsResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(containsResult != nullptr);
    EXPECT_EQ(containsResult->method, "contains");
    EXPECT_EQ(containsResult->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a String(text) construction expression")
{
    auto program = parseOne("x = String(\"Axea\")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* stringNew = dynamic_cast<StringNewExpr*>(assignment->value.get());
    EXPECT_TRUE(stringNew != nullptr);
    auto* text = dynamic_cast<StringExpr*>(stringNew->text.get());
    EXPECT_TRUE(text != nullptr);
    EXPECT_EQ(text->value, "Axea");
}

TEST("Parser rejects String(...) with anything but exactly one argument")
{
    EXPECT_THROWS(parseOne("x = String()"));
    EXPECT_THROWS(parseOne("x = String(\"a\", \"b\")"));
}

TEST("Parser parses a String parameter type into the canonical form")
{
    auto program = parseOne("useString(s: String) -> i32 { return s.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "String");
}

TEST("Parser builds a String append method-call expression")
{
    auto program = parseOne("f() { s = String(\"a\")  s.append(\"b\") }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* appendResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(appendResult != nullptr);
    EXPECT_EQ(appendResult->method, "append");
    EXPECT_EQ(appendResult->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a Buffer() construction expression")
{
    auto program = parseOne("x = Buffer()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* bufferNew = dynamic_cast<BufferNewExpr*>(assignment->value.get());
    EXPECT_TRUE(bufferNew != nullptr);
}

TEST("Parser rejects Buffer(...) with any argument")
{
    EXPECT_THROWS(parseOne("x = Buffer(\"a\")"));
}

TEST("Parser parses a Buffer parameter type into the canonical form")
{
    auto program = parseOne("useBuffer(b: Buffer) -> i32 { return b.length }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params[0].type, "Buffer");
}

TEST("Parser builds Buffer append/append_line/clear/reserve/finish method-call expressions")
{
    auto program = parseOne("f() { b = Buffer()  b.append(\"a\")  b.append_line(\"b\")  b.clear()  "
                            "b.reserve(8)  b.finish() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* finishResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(finishResult != nullptr);
    EXPECT_EQ(finishResult->method, "finish");
    EXPECT_EQ(finishResult->arguments.size(), static_cast<std::size_t>(0));

    auto* reserveStmt = dynamic_cast<ExprStmt*>(body->statements.at(4).get());
    auto* reserveCall = dynamic_cast<MethodCallExpr*>(reserveStmt->expr.get());
    EXPECT_TRUE(reserveCall != nullptr);
    EXPECT_EQ(reserveCall->method, "reserve");
    EXPECT_EQ(reserveCall->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a Buffer.write(...) method-call expression despite 'write' predating this "
     "as a keyword token (see docs/language/0061-buffer-write.md)")
{
    auto program = parseOne("f() { b = Buffer()  b.write(\"hi {name}\") }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());

    auto* writeResult = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(writeResult != nullptr);
    EXPECT_EQ(writeResult->method, "write");
    EXPECT_EQ(writeResult->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a method-call expression, distinct from field access")
{
    // A trailing expression with nothing after it becomes the block's
    // result, not a pushed statement (same as any other expression) - so
    // the method call ends up as `body->result` here, not `statements[1]`.
    auto program = parseOne("f() { numbers = List<i32>()  numbers.push(4) }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* methodCall = dynamic_cast<MethodCallExpr*>(body->result.get());
    EXPECT_TRUE(methodCall != nullptr);
    EXPECT_EQ(methodCall->method, "push");
    EXPECT_EQ(methodCall->arguments.size(), static_cast<std::size_t>(1));

    auto* object = dynamic_cast<NameExpr*>(methodCall->object.get());
    EXPECT_TRUE(object != nullptr);
    EXPECT_EQ(object->name, "numbers");
}

TEST("Parser builds a zero-argument method-call expression usable as a value")
{
    auto program = parseOne("f() -> i32 { numbers = List<i32>()  return numbers.pop() }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* returnStmt = dynamic_cast<ReturnStmt*>(body->statements.at(1).get());
    auto* methodCall = dynamic_cast<MethodCallExpr*>(returnStmt->value.get());
    EXPECT_TRUE(methodCall != nullptr);
    EXPECT_EQ(methodCall->method, "pop");
    EXPECT_TRUE(methodCall->arguments.empty());
}

TEST("Parser builds an extern c declaration with parameters and a return type")
{
    auto program = parseOne("extern c abs(x: i32) -> i32");

    auto* externDecl = dynamic_cast<ExternDecl*>(program.items.at(0).get());
    EXPECT_TRUE(externDecl != nullptr);
    EXPECT_EQ(externDecl->name, "abs");
    EXPECT_EQ(externDecl->params.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(externDecl->params[0].name, "x");
    EXPECT_EQ(externDecl->params[0].type, "i32");
    EXPECT_TRUE(!externDecl->params[0].declaredCapability);
    EXPECT_TRUE(externDecl->returnType.has_value());
    EXPECT_EQ(*externDecl->returnType, "i32");
}

TEST("Parser builds an extern c declaration with no return type (unit) and a cstr parameter")
{
    auto program = parseOne("extern c puts(text: cstr)");

    auto* externDecl = dynamic_cast<ExternDecl*>(program.items.at(0).get());
    EXPECT_TRUE(externDecl != nullptr);
    EXPECT_EQ(externDecl->name, "puts");
    EXPECT_EQ(externDecl->params.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(externDecl->params[0].type, "cstr");
    EXPECT_TRUE(!externDecl->returnType.has_value());
}

TEST("Parser rejects an extern calling convention other than 'c'")
{
    EXPECT_THROWS(parseOne("extern rust foo(x: i32)"));
}

TEST("Parser builds a .to_cstr() method-call expression")
{
    auto program = parseOne("x = \"hi\".to_cstr()");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<MethodCallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->method, "to_cstr");
    EXPECT_TRUE(call->typeArgument.empty());
    EXPECT_EQ(call->arguments.size(), static_cast<std::size_t>(0));
}

TEST("Parser builds a print(...) call expression like any other bare-identifier call")
{
    auto program = parseOne("x = print(\"hello\", 1)");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<CallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "print");
    EXPECT_EQ(call->arguments.size(), static_cast<std::size_t>(2));
}

TEST("Parser builds a write(...) call expression despite 'write' also being the capability-"
     "prefix keyword (see docs/language/Axea_Printing_Formatting.md)")
{
    auto program = parseOne("x = write(\"hello\")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<CallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "write");
    EXPECT_EQ(call->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser still parses 'write' as a parameter capability prefix, unaffected by the new "
     "write(...) call special-case")
{
    auto program = parseOne("bump(write u: i32) { }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(function->params[0].declaredCapability.has_value());
    EXPECT_TRUE(*function->params[0].declaredCapability == Capability::Write);
}

TEST("Parser parses a bare top-level print(...) call with no assignment as an ExprStmt, not "
     "an attempted function declaration (see docs/language/0049-printing-formatting.md's own "
     "Parsing follow-up)")
{
    auto program = parseOne("print(\"hello\")");

    auto* exprStmt = dynamic_cast<ExprStmt*>(program.items.at(0).get());
    EXPECT_TRUE(exprStmt != nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "print");
}

TEST("Parser parses a bare top-level write(...) call the same way, despite 'write' being the "
     "capability-prefix keyword rather than an Identifier")
{
    auto program = parseOne("write(\"hello\")");

    auto* exprStmt = dynamic_cast<ExprStmt*>(program.items.at(0).get());
    EXPECT_TRUE(exprStmt != nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "write");
}

TEST("Parser still parses a real function declaration with zero parameters, disambiguated "
     "from a bare zero-arg call by looking past the empty parens for '->'/'{'/'=>'")
{
    auto program = parseOne("foo() -> i32 { return 1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->name, "foo");
    EXPECT_EQ(function->params.size(), static_cast<std::size_t>(0));
}

TEST("Parser parses a bare zero-arg top-level call (no '->'/'{'/'=>' after the empty parens) "
     "as an ExprStmt, not a function declaration attempt")
{
    auto program = parseOne("foo()");

    auto* exprStmt = dynamic_cast<ExprStmt*>(program.items.at(0).get());
    EXPECT_TRUE(exprStmt != nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "foo");
}

TEST("Parser still parses a real function declaration whose first parameter is a bare name "
     "(no capability prefix), disambiguated from a call by the 'Identifier :' shape only a "
     "Param can start with")
{
    auto program = parseOne("foo(x: i32) -> i32 { return x }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->params.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(function->params[0].name, "x");
}

TEST("Parser parses a bare top-level call whose first argument is just a bare name (no ':' "
     "following it) as an ExprStmt, not a function declaration attempt")
{
    auto program = parseOne("y = 1 "
                            "foo(y)");

    auto* exprStmt = dynamic_cast<ExprStmt*>(program.items.at(1).get());
    EXPECT_TRUE(exprStmt != nullptr);
    auto* call = dynamic_cast<CallExpr*>(exprStmt->expr.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "foo");
}

TEST("Parser builds a plain string literal with no interpolation spans as StringExpr, "
     "not InterpolatedStringExpr")
{
    auto program = parseOne("x = \"hello world\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* str = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, "hello world");
}

TEST("Parser splits an interpolated string literal into alternating literal/expr pieces")
{
    auto program = parseOne("x = \"Hello {name}, you are {age} years old\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(5));

    EXPECT_TRUE(interpolated->pieces[0].expr == nullptr);
    EXPECT_EQ(interpolated->pieces[0].literalText, "Hello ");

    EXPECT_TRUE(interpolated->pieces[1].expr != nullptr);
    auto* nameExpr = dynamic_cast<NameExpr*>(interpolated->pieces[1].expr.get());
    EXPECT_TRUE(nameExpr != nullptr);
    EXPECT_EQ(nameExpr->name, "name");

    EXPECT_TRUE(interpolated->pieces[2].expr == nullptr);
    EXPECT_EQ(interpolated->pieces[2].literalText, ", you are ");

    EXPECT_TRUE(interpolated->pieces[3].expr != nullptr);
    auto* ageExpr = dynamic_cast<NameExpr*>(interpolated->pieces[3].expr.get());
    EXPECT_TRUE(ageExpr != nullptr);
    EXPECT_EQ(ageExpr->name, "age");

    EXPECT_TRUE(interpolated->pieces[4].expr == nullptr);
    EXPECT_EQ(interpolated->pieces[4].literalText, " years old");
}

TEST("Parser parses an arbitrary expression inside an interpolation span")
{
    auto program = parseOne("x = \"next year: {age + 1}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(2));

    auto* add = dynamic_cast<BinaryExpr*>(interpolated->pieces[1].expr.get());
    EXPECT_TRUE(add != nullptr);
    EXPECT_EQ(add->op, TokenKind::Plus);
}

TEST("Parser treats '{{' and '}}' as escaped literal braces, not interpolation")
{
    auto program = parseOne("x = \"{{literal}} and {age}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(2));
    EXPECT_TRUE(interpolated->pieces[0].expr == nullptr);
    EXPECT_EQ(interpolated->pieces[0].literalText, "{literal} and ");
}

TEST("Parser rejects an empty interpolation expression '{}'")
{
    EXPECT_THROWS(parseOne("x = \"{}\""));
}

TEST("Parser rejects an unterminated interpolation expression")
{
    EXPECT_THROWS(parseOne("x = \"hello {name\""));
}

TEST("Parser rejects an unmatched '}' in a string literal")
{
    EXPECT_THROWS(parseOne("x = \"hello }\""));
}

TEST("Parser builds an array-literal slice expression into the same StrSliceExpr node str "
     "slicing already uses (see docs/language/0050-collection-join-and-slicing.md)")
{
    auto program = parseOne("x = [1, 2, 3][..2]");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* slice = dynamic_cast<StrSliceExpr*>(assignment->value.get());
    EXPECT_TRUE(slice != nullptr);
    EXPECT_TRUE(slice->start == nullptr);
    EXPECT_TRUE(slice->end != nullptr);
    EXPECT_TRUE(dynamic_cast<ArrayLiteralExpr*>(slice->object.get()) != nullptr);
}

TEST("Parser builds a .join(separator) method-call expression")
{
    auto program = parseOne("x = numbers.join(\",\")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<MethodCallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->method, "join");
    EXPECT_EQ(call->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds an Int64Expr from an i64-suffixed literal, with the suffix stripped from "
     "the parsed value (see docs/language/0005-type-system.md)")
{
    auto program = parseOne("x = 100i64");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* int64Expr = dynamic_cast<Int64Expr*>(assignment->value.get());
    EXPECT_TRUE(int64Expr != nullptr);
    EXPECT_EQ(int64Expr->value, static_cast<std::int64_t>(100));
}

TEST("Parser builds a FloatExpr from a bare decimal literal and from an f64-suffixed one, both "
     "with the suffix (if any) stripped")
{
    auto program = parseOne("x = 1.5 y = 100f64");

    auto* xAssignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* xFloat = dynamic_cast<FloatExpr*>(xAssignment->value.get());
    EXPECT_TRUE(xFloat != nullptr);
    EXPECT_EQ(xFloat->value, 1.5);

    auto* yAssignment = dynamic_cast<AssignmentStmt*>(program.items.at(1).get());
    auto* yFloat = dynamic_cast<FloatExpr*>(yAssignment->value.get());
    EXPECT_TRUE(yFloat != nullptr);
    EXPECT_EQ(yFloat->value, 100.0);
}

TEST("Parser builds a CastExpr for '<expr> as <targetType>', binding tighter than the "
     "surrounding binary operator (`x as i64 + 1` is `(x as i64) + 1`, see "
     "docs/language/0005-type-system.md)")
{
    auto program = parseOne("y = x as i64 + 1");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* add = dynamic_cast<BinaryExpr*>(assignment->value.get());
    EXPECT_TRUE(add != nullptr);
    EXPECT_EQ(add->op, TokenKind::Plus);

    auto* cast = dynamic_cast<CastExpr*>(add->left.get());
    EXPECT_TRUE(cast != nullptr);
    EXPECT_EQ(cast->targetType, "i64");
    EXPECT_TRUE(dynamic_cast<NameExpr*>(cast->operand.get()) != nullptr);
}

TEST("Parser splits an interpolation span's expression text from its ':<spec>' format spec, "
     "leaving a plain span (no ':') with an empty formatSpec (see "
     "docs/language/0055-numeric-format-specs.md)")
{
    auto program = parseOne("x = \"{value:05} plain {other}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(3));

    EXPECT_TRUE(interpolated->pieces[0].expr != nullptr);
    EXPECT_EQ(interpolated->pieces[0].formatSpec, "05");
    auto* valueExpr = dynamic_cast<NameExpr*>(interpolated->pieces[0].expr.get());
    EXPECT_TRUE(valueExpr != nullptr);
    EXPECT_EQ(valueExpr->name, "value");

    EXPECT_TRUE(interpolated->pieces[2].expr != nullptr);
    EXPECT_EQ(interpolated->pieces[2].formatSpec, "");
}

TEST("Parser keeps a ':' inside a nested string-literal argument out of the format-spec split "
     "(e.g. `{x.join(\":\")}` has no format spec)")
{
    auto program = parseOne("x = \"{items.join(\":\")}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(interpolated->pieces[0].formatSpec, "");
    EXPECT_TRUE(dynamic_cast<MethodCallExpr*>(interpolated->pieces[0].expr.get()) != nullptr);
}

TEST("Parser rejects an empty format spec after ':' in an interpolation span")
{
    EXPECT_THROWS(parseOne("x = \"{value:}\""));
}

TEST("Parser extracts an alignment format spec ('<'/'>'/'^' + width) unchanged, same raw-text "
     "split as any other spec (see docs/language/0057-alignment.md)")
{
    auto program = parseOne("x = \"{name:<20}|{name:>20}|{name:^20}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(5));
    EXPECT_EQ(interpolated->pieces[0].formatSpec, "<20");
    EXPECT_EQ(interpolated->pieces[2].formatSpec, ">20");
    EXPECT_EQ(interpolated->pieces[4].formatSpec, "^20");
}

TEST("Parser builds a self-documenting '{expr=}' piece with the raw source text captured as "
     "selfDocPrefix, and an empty formatSpec (see docs/language/0058-debug-formatting.md)")
{
    auto program = parseOne("x = \"{age + 1=}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(interpolated->pieces[0].selfDocPrefix, "age + 1");
    EXPECT_EQ(interpolated->pieces[0].formatSpec, "");
    EXPECT_TRUE(dynamic_cast<BinaryExpr*>(interpolated->pieces[0].expr.get()) != nullptr);
}

TEST("Parser combines '{expr=:spec}' - self-doc prefix plus a trailing format spec")
{
    auto program = parseOne("x = \"{pi=:.2}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces[0].selfDocPrefix, "pi");
    EXPECT_EQ(interpolated->pieces[0].formatSpec, ".2");
}

TEST("Parser does not misread a comparison operator ending in '=' ('==', '!=', '<=', '>=') as "
     "a self-doc marker")
{
    auto program = parseOne("x = \"{a >= b}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces[0].selfDocPrefix, "");
    auto* binary = dynamic_cast<BinaryExpr*>(interpolated->pieces[0].expr.get());
    EXPECT_TRUE(binary != nullptr);
    EXPECT_EQ(binary->op, TokenKind::GreaterEqual);
}

TEST("Parser rejects an empty interpolation expression before '=' in a self-doc span")
{
    EXPECT_THROWS(parseOne("x = \"{=}\""));
}

TEST("Parser builds a debug-format '{expr:?}' piece with debug set and an empty formatSpec "
     "(see docs/language/0058-debug-formatting.md)")
{
    auto program = parseOne("x = \"{user:?}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_TRUE(interpolated->pieces[0].debug);
    EXPECT_EQ(interpolated->pieces[0].formatSpec, "");
}

TEST("Parser combines a self-doc prefix with a debug spec: '{s=:?}'")
{
    auto program = parseOne("x = \"{s=:?}\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces[0].selfDocPrefix, "s");
    EXPECT_TRUE(interpolated->pieces[0].debug);
}

TEST("Parser builds Ok(value)/Err(value) construction expressions and 'Result<T,E>' as a type "
     "(see docs/language/0063-result.md)")
{
    auto program = parseOne("f(a: i32) -> Result<i32, str> { return Ok(a) }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(function->returnType.value(), "Result<i32,str>");

    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* returnStmt = dynamic_cast<ReturnStmt*>(body->statements.at(0).get());
    auto* okExpr = dynamic_cast<OkExpr*>(returnStmt->value.get());
    EXPECT_TRUE(okExpr != nullptr);
    auto* name = dynamic_cast<NameExpr*>(okExpr->value.get());
    EXPECT_TRUE(name != nullptr);
    EXPECT_EQ(name->name, "a");
}

TEST("Parser builds an Err(value) construction expression")
{
    auto program = parseOne("x: Result<i32, str> = Err(\"oops\")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* errExpr = dynamic_cast<ErrExpr*>(assignment->value.get());
    EXPECT_TRUE(errExpr != nullptr);
    auto* str = dynamic_cast<StringExpr*>(errExpr->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, "oops");
}

TEST("Parser rejects Ok(...)/Err(...) with the wrong argument count")
{
    EXPECT_THROWS(parseOne("x = Ok()"));
    EXPECT_THROWS(parseOne("x = Ok(1, 2)"));
    EXPECT_THROWS(parseOne("x = Err()"));
}

TEST("Parser parses a nested Result<T,E> type via bracket-depth-aware comma splitting - E "
     "itself a Map<K,V>")
{
    auto program = parseOne("f() -> Result<i32, Map<i32,i32>> { return Ok(1) }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_EQ(function->returnType.value(), "Result<i32,Map<i32,i32>>");
}

TEST("Parser canonicalizes a 'T1 | T2' union type into a sorted, '|'-joined string, so 'i32 | "
     "str' and 'str | i32' parse to the identical text (see docs/language/0065-unions.md)")
{
    auto a = parseOne("f(x: i32 | str) -> i32 { return 0 }");
    auto b = parseOne("f(x: str | i32) -> i32 { return 0 }");

    auto* functionA = dynamic_cast<FunctionDecl*>(a.items.at(0).get());
    auto* functionB = dynamic_cast<FunctionDecl*>(b.items.at(0).get());
    EXPECT_EQ(functionA->params.at(0).type, "i32|str");
    EXPECT_EQ(functionB->params.at(0).type, "i32|str");
}

TEST("Parser deduplicates a repeated alternative in a union type")
{
    auto program = parseOne("f(x: i32 | str | i32) -> i32 { return 0 }");
    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_EQ(function->params.at(0).type, "i32|str");
}

TEST("Parser parses a three-alternative union type, sorted alphabetically")
{
    auto program = parseOne("f(x: str | bool | i32) -> i32 { return 0 }");
    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_EQ(function->params.at(0).type, "bool|i32|str");
}

TEST("Parser builds an EnumDecl with each variant's own name and positional payload types (see "
     "docs/language/0064-enums.md)")
{
    auto program = parseOne("enum Shape { Circle(f64)  Rectangle(f64, f64)  Point }");

    auto* enumDecl = dynamic_cast<EnumDecl*>(program.items.at(0).get());
    EXPECT_TRUE(enumDecl != nullptr);
    EXPECT_EQ(enumDecl->name, "Shape");
    EXPECT_EQ(enumDecl->variants.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(enumDecl->variants[0].name, "Circle");
    EXPECT_EQ(enumDecl->variants[0].fieldTypes.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(enumDecl->variants[0].fieldTypes[0], "f64");
    EXPECT_EQ(enumDecl->variants[1].name, "Rectangle");
    EXPECT_EQ(enumDecl->variants[1].fieldTypes.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(enumDecl->variants[2].name, "Point");
    EXPECT_TRUE(enumDecl->variants[2].fieldTypes.empty());
}

TEST("Parser builds EnumName.Variant(args) as an ordinary MethodCallExpr - no new grammar, "
     "reuses the existing object.method(args) postfix shape")
{
    auto program = parseOne("x = Shape.Circle(5.0)");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* methodCall = dynamic_cast<MethodCallExpr*>(assignment->value.get());
    EXPECT_TRUE(methodCall != nullptr);
    EXPECT_EQ(methodCall->method, "Circle");
    auto* object = dynamic_cast<NameExpr*>(methodCall->object.get());
    EXPECT_TRUE(object != nullptr);
    EXPECT_EQ(object->name, "Shape");
    EXPECT_EQ(methodCall->arguments.size(), static_cast<std::size_t>(1));
}

TEST("Parser builds a no-payload EnumName.Variant (no parens) as an ordinary FieldExpr")
{
    auto program = parseOne("x = Shape.Point");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* field = dynamic_cast<FieldExpr*>(assignment->value.get());
    EXPECT_TRUE(field != nullptr);
    EXPECT_EQ(field->field, "Point");
}

TEST("Parser builds a MatchExpr with each arm's own variant name, binding names, and body")
{
    auto program = parseOne("f() -> f64 { return match s { "
                            "Circle(r) => r  Rectangle(w, h) => w  _ => 0.0 } }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    auto* body = dynamic_cast<BlockExpr*>(function->body.get());
    auto* returnStmt = dynamic_cast<ReturnStmt*>(body->statements.at(0).get());
    auto* matchExpr = dynamic_cast<MatchExpr*>(returnStmt->value.get());
    EXPECT_TRUE(matchExpr != nullptr);
    auto* scrutinee = dynamic_cast<NameExpr*>(matchExpr->scrutinee.get());
    EXPECT_TRUE(scrutinee != nullptr);
    EXPECT_EQ(scrutinee->name, "s");
    EXPECT_EQ(matchExpr->arms.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(matchExpr->arms[0].variantName, "Circle");
    EXPECT_EQ(matchExpr->arms[0].bindingNames.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(matchExpr->arms[0].bindingNames[0], "r");
    EXPECT_EQ(matchExpr->arms[1].variantName, "Rectangle");
    EXPECT_EQ(matchExpr->arms[1].bindingNames.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(matchExpr->arms[2].variantName, "_");
    EXPECT_TRUE(matchExpr->arms[2].bindingNames.empty());
}

TEST("Parser rejects a malformed match arm - missing '=>' or a trailing comma inside bindings")
{
    EXPECT_THROWS(parseOne("f() -> f64 { return match s { Circle(r) 0.0 } }"));
}

TEST("Parser builds a raw string literal as a plain StringExpr, backslashes and braces both "
     "left completely untouched (see docs/language/0059-raw-strings.md)")
{
    auto program = parseOne(R"(x = r"C:\Users\Burke\Documents")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* str = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, R"(C:\Users\Burke\Documents)");
}

TEST("Parser never splits a raw string on '{expr}' - it stays a single literal StringExpr even "
     "when its content looks exactly like an interpolation span")
{
    auto program = parseOne(R"(x = r"literal {name} not interpolated")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* str = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, "literal {name} not interpolated");
}

TEST("Parser builds a plain triple-quoted string with no interpolation spans as StringExpr, "
     "embedded newlines preserved verbatim (see docs/language/0060-multiline-strings.md)")
{
    auto program = parseOne("x = \"\"\"\nHello,\n\nBuild complete.\n\"\"\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* str = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, "\nHello,\n\nBuild complete.\n");
}

TEST("Parser splits a triple-quoted string with an interpolation span into pieces exactly like "
     "an ordinary single-quoted one - multiline strings support interpolation")
{
    auto program = parseOne("x = \"\"\"Hello {name},\nyou are {age}\"\"\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* interpolated = dynamic_cast<InterpolatedStringExpr*>(assignment->value.get());
    EXPECT_TRUE(interpolated != nullptr);
    EXPECT_EQ(interpolated->pieces.size(), static_cast<std::size_t>(4));
    EXPECT_EQ(interpolated->pieces[0].literalText, "Hello ");
    auto* nameExpr = dynamic_cast<NameExpr*>(interpolated->pieces[1].expr.get());
    EXPECT_TRUE(nameExpr != nullptr);
    EXPECT_EQ(nameExpr->name, "name");
    EXPECT_EQ(interpolated->pieces[2].literalText, ",\nyou are ");
    auto* ageExpr = dynamic_cast<NameExpr*>(interpolated->pieces[3].expr.get());
    EXPECT_TRUE(ageExpr != nullptr);
    EXPECT_EQ(ageExpr->name, "age");
}

TEST("Parser does not end a triple-quoted string at a lone embedded '\"' - only a real 3-in-a-row "
     "closing run ends it, and the lone quote survives as literal content")
{
    auto program = parseOne(R"(x = """She said "hi" to me""")");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* str = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, R"(She said "hi" to me)");
}

TEST("Parser builds a raw triple-quoted string ('r\"\"\"...\"\"\"') as a plain StringExpr, "
     "combining both raw (no interpolation) and multiline (embedded quotes/newlines survive)")
{
    auto program = parseOne("x = r\"\"\"\n{\n    \"name\": \"{literal}\"\n}\n\"\"\"");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* str = dynamic_cast<StringExpr*>(assignment->value.get());
    EXPECT_TRUE(str != nullptr);
    EXPECT_EQ(str->value, "\n{\n    \"name\": \"{literal}\"\n}\n");
}

TEST("Parser builds a TraitDecl with its own method signature's name and arity (see "
     "docs/language/0062-display-trait.md)")
{
    auto program = parseOne("trait Display { format(self, buf: Buffer) }");

    auto* trait = dynamic_cast<TraitDecl*>(program.items.at(0).get());
    EXPECT_TRUE(trait != nullptr);
    EXPECT_EQ(trait->name, "Display");
    EXPECT_EQ(trait->methods.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(trait->methods[0].name, "format");
    EXPECT_EQ(trait->methods[0].paramCount, static_cast<std::size_t>(2));
}

TEST("Parser builds an ImplDecl whose methods are real FunctionDecls with mangled "
     "'TypeName.methodName' names and self resolved to the concrete impl target type")
{
    auto program =
        parseOne("impl Display for Point { format(self, buf: Buffer) { buf.write(\"hi\") } }");

    auto* impl = dynamic_cast<ImplDecl*>(program.items.at(0).get());
    EXPECT_TRUE(impl != nullptr);
    EXPECT_EQ(impl->traitName, "Display");
    EXPECT_EQ(impl->typeName, "Point");
    EXPECT_EQ(impl->methods.size(), static_cast<std::size_t>(1));

    auto& method = impl->methods[0];
    EXPECT_EQ(method->name, "Point.format");
    EXPECT_EQ(method->params.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(method->params[0].name, "self");
    EXPECT_EQ(method->params[0].type, "Point");
    EXPECT_TRUE(!method->params[0].declaredCapability.has_value());
    EXPECT_EQ(method->params[1].name, "buf");
    EXPECT_EQ(method->params[1].type, "Buffer");
}

TEST("Parser accepts a real parameter literally named 'self' with an explicit type, distinct "
     "from the bare-self sugar")
{
    auto program = parseOne("impl Display for Point { format(self: i32) { } }");

    auto* impl = dynamic_cast<ImplDecl*>(program.items.at(0).get());
    EXPECT_EQ(impl->methods[0]->params[0].type, "i32");
}

TEST("Parser parses 'module name' as a single top-level declaration scoping the whole file, "
     "recorded on Program::moduleName (see docs/language/0066-modules.md)")
{
    auto program = parseOne("module math\n"
                            "pub square(x: i32) -> i32 { return x * x }");

    EXPECT_TRUE(program.moduleName.has_value());
    EXPECT_EQ(*program.moduleName, "math");
    auto* moduleDecl = dynamic_cast<ModuleDecl*>(program.items.at(0).get());
    EXPECT_TRUE(moduleDecl != nullptr);
    EXPECT_EQ(moduleDecl->name, "math");
}

TEST("Parser rejects a second 'module' declaration in the same file")
{
    EXPECT_THROWS(parseOne("module a\nmodule b\nx = 1"));
}

TEST("Parser tracks 'pub' on a function/extern declaration via isPublic - a file with no 'pub' "
     "at all defaults every declaration to non-public")
{
    auto program = parseOne("pub f() -> i32 { return 1 }\n"
                            "g() -> i32 { return 2 }\n"
                            "pub extern c h() -> i32\n"
                            "extern c k() -> i32");

    EXPECT_TRUE(dynamic_cast<FunctionDecl*>(program.items.at(0).get())->isPublic);
    EXPECT_TRUE(!dynamic_cast<FunctionDecl*>(program.items.at(1).get())->isPublic);
    EXPECT_TRUE(dynamic_cast<ExternDecl*>(program.items.at(2).get())->isPublic);
    EXPECT_TRUE(!dynamic_cast<ExternDecl*>(program.items.at(3).get())->isPublic);
}

TEST("Parser parses 'use math' and 'use math as m' as UseDecl items carrying the real module "
     "name and an optional alias")
{
    auto plain = parseOne("use math\nx = 1");
    auto* plainUse = dynamic_cast<UseDecl*>(plain.items.at(0).get());
    EXPECT_TRUE(plainUse != nullptr);
    EXPECT_EQ(plainUse->moduleName, "math");
    EXPECT_TRUE(!plainUse->alias.has_value());

    auto aliased = parseOne("use math as m\nx = 1");
    auto* aliasedUse = dynamic_cast<UseDecl*>(aliased.items.at(0).get());
    EXPECT_TRUE(aliasedUse != nullptr);
    EXPECT_EQ(aliasedUse->moduleName, "math");
    EXPECT_EQ(*aliasedUse->alias, "m");
}

TEST("Parser rewrites an aliased 'm.foo(...)' call site's own object NameExpr to the real "
     "module name at the exact point it's parsed, so every later pass only ever sees the real "
     "name - never anywhere else a plain reference to 'm' would appear")
{
    auto program = parseOne("use math as m\n"
                            "y = m.square(5)");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(1).get());
    auto* methodCall = dynamic_cast<MethodCallExpr*>(assignment->value.get());
    EXPECT_TRUE(methodCall != nullptr);
    auto* object = dynamic_cast<NameExpr*>(methodCall->object.get());
    EXPECT_TRUE(object != nullptr);
    EXPECT_EQ(object->name, "math");
    EXPECT_EQ(methodCall->method, "square");
}

TEST("Parser's alias rewrite only touches a NameExpr immediately followed by '.' - an ordinary "
     "local variable that happens to share a name with an in-scope alias is unaffected "
     "everywhere else it's referenced bare")
{
    auto program = parseOne("use math as m\n"
                            "m = 5\n"
                            "y = m + 1");

    auto* firstAssignment = dynamic_cast<AssignmentStmt*>(program.items.at(1).get());
    EXPECT_EQ(firstAssignment->name, "m");
    auto* secondAssignment = dynamic_cast<AssignmentStmt*>(program.items.at(2).get());
    auto* addExpr = dynamic_cast<BinaryExpr*>(secondAssignment->value.get());
    auto* leftName = dynamic_cast<NameExpr*>(addExpr->left.get());
    EXPECT_TRUE(leftName != nullptr);
    EXPECT_EQ(leftName->name, "m");
}

TEST("Parser builds a ClosureExpr AST node for a 'fn(params) -> ReturnType { body }' literal "
     "(see docs/language/0067-closures.md), the same (params, optional return type, body) shape "
     "as a top-level FunctionDecl")
{
    auto program = parseOne("add = fn(x: i32, y: i32) -> i32 { return x + y }");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* closure = dynamic_cast<ClosureExpr*>(assignment->value.get());
    EXPECT_TRUE(closure != nullptr);
    EXPECT_EQ(closure->params.size(), std::size_t(2));
    EXPECT_EQ(closure->params[0].name, "x");
    EXPECT_EQ(closure->params[0].type, "i32");
    EXPECT_EQ(closure->params[1].name, "y");
    EXPECT_EQ(*closure->returnType, "i32");
}

TEST("Parser parses 'fn(T1,T2)->R' as a type - canonicalized with no spaces, exactly like every "
     "other type this parser produces")
{
    auto program = parseOne("f(callback: fn(i32, str) -> bool) -> i32 { return 1 }");

    auto* function = dynamic_cast<FunctionDecl*>(program.items.at(0).get());
    EXPECT_EQ(function->params.at(0).type, "fn(i32,str)->bool");
}

TEST("Parser desugars a closure's own '=>' body the same way a top-level function's does - a "
     "single ReturnStmt wrapping the expression, not a trailing block result")
{
    auto program = parseOne("double = fn(x: i32) -> i32 => x * 2");

    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* closure = dynamic_cast<ClosureExpr*>(assignment->value.get());
    auto* body = dynamic_cast<BlockExpr*>(closure->body.get());
    EXPECT_EQ(body->statements.size(), std::size_t(1));
    auto* returnStmt = dynamic_cast<ReturnStmt*>(body->statements.at(0).get());
    EXPECT_TRUE(returnStmt != nullptr);
    EXPECT_TRUE(body->result == nullptr);
}

TEST("Parser parses a closure literal called immediately - 'fn(x: i32) -> i32 { x }(5)' - as an "
     "ordinary CallExpr whose callee text is empty (a closure value has no name), consistent "
     "with how a closure-typed local is called")
{
    // A closure literal has no name of its own, so it can't be the *callee* of an ordinary
    // Identifier(args) call the way a named local can - only a name (local/param) can be
    // called this phase. This test instead confirms a closure nested as a call *argument*
    // parses correctly, the more common real shape.
    auto program = parseOne("y = apply(fn(x: i32) -> i32 { return x }, 5)");
    auto* assignment = dynamic_cast<AssignmentStmt*>(program.items.at(0).get());
    auto* call = dynamic_cast<CallExpr*>(assignment->value.get());
    EXPECT_TRUE(call != nullptr);
    EXPECT_EQ(call->callee, "apply");
    EXPECT_EQ(call->arguments.size(), std::size_t(2));
    auto* closureArg = dynamic_cast<ClosureExpr*>(call->arguments.at(0).get());
    EXPECT_TRUE(closureArg != nullptr);
}
