#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "interpreter/Interpreter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/TypeChecker.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    void printExpr(const Expr& expr, int indent = 0);
    void printStmt(const Stmt& stmt, int indent = 0);

    void printExpr(const Expr& expr, int indent)
    {
        const std::string pad(static_cast<std::size_t>(indent), ' ');

        if (const auto* integer = dynamic_cast<const IntegerExpr*>(&expr))
        {
            std::cout << pad << "Integer(" << integer->value << ")\n";
            return;
        }

        if (const auto* name = dynamic_cast<const NameExpr*>(&expr))
        {
            std::cout << pad << "Name(" << name->name << ")\n";
            return;
        }

        if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr))
        {
            std::cout << pad << "Binary(" << tokenKindName(binary->op) << ")\n";
            printExpr(*binary->left, indent + 2);
            printExpr(*binary->right, indent + 2);
            return;
        }

        if (const auto* boolean = dynamic_cast<const BoolExpr*>(&expr))
        {
            std::cout << pad << "Bool(" << (boolean->value ? "true" : "false") << ")\n";
            return;
        }

        if (const auto* string = dynamic_cast<const StringExpr*>(&expr))
        {
            std::cout << pad << "String(" << string->value << ")\n";
            return;
        }

        if (const auto* ifExpr = dynamic_cast<const IfExpr*>(&expr))
        {
            std::cout << pad << "If\n";
            printExpr(*ifExpr->condition, indent + 2);
            std::cout << pad << "Then\n";
            printExpr(*ifExpr->thenBranch, indent + 2);
            std::cout << pad << "Else\n";
            printExpr(*ifExpr->elseBranch, indent + 2);
            return;
        }

        if (const auto* block = dynamic_cast<const BlockExpr*>(&expr))
        {
            std::cout << pad << "Block\n";
            for (const auto& statement : block->statements)
            {
                printStmt(*statement, indent + 2);
            }
            if (block->result)
            {
                printExpr(*block->result, indent + 2);
            }
            return;
        }

        if (const auto* call = dynamic_cast<const CallExpr*>(&expr))
        {
            std::cout << pad << "Call(" << call->callee << ")\n";
            for (const auto& argument : call->arguments)
            {
                printExpr(*argument, indent + 2);
            }
            return;
        }

        if (const auto* field = dynamic_cast<const FieldExpr*>(&expr))
        {
            std::cout << pad << "Field(" << field->field << ")\n";
            printExpr(*field->object, indent + 2);
            return;
        }

        if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(&expr))
        {
            std::cout << pad << "StructLiteral(" << literal->typeName << ")\n";
            for (const auto& [fieldName, fieldExpr] : literal->fields)
            {
                std::cout << pad << "  " << fieldName << ":\n";
                printExpr(*fieldExpr, indent + 4);
            }
            return;
        }
    }

    void printStmt(const Stmt& stmt, int indent)
    {
        const std::string pad(static_cast<std::size_t>(indent), ' ');

        if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(&stmt))
        {
            std::cout << pad << "Assignment(" << assignment->name << ")\n";
            printExpr(*assignment->value, indent + 2);
            return;
        }

        if (const auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt))
        {
            std::cout << pad << "Return\n";
            if (returnStmt->value)
            {
                printExpr(*returnStmt->value, indent + 2);
            }
            return;
        }

        if (const auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt))
        {
            std::cout << pad << "ExprStmt\n";
            printExpr(*exprStmt->expr, indent + 2);
            return;
        }

        if (const auto* fieldAssign = dynamic_cast<const FieldAssignStmt*>(&stmt))
        {
            std::cout << pad << "FieldAssign(" << fieldAssign->field << ")\n";
            printExpr(*fieldAssign->object, indent + 2);
            printExpr(*fieldAssign->value, indent + 2);
            return;
        }

        if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
        {
            std::cout << pad << (incDec->increment ? "Increment\n" : "Decrement\n");
            printExpr(*incDec->target, indent + 2);
            return;
        }

        if (const auto* function = dynamic_cast<const FunctionDecl*>(&stmt))
        {
            std::cout << pad << "Function(" << function->name << ")\n";
            for (const auto& param : function->params)
            {
                std::cout << pad << "  Param(";
                if (param.declaredCapability)
                {
                    std::cout << capabilityName(*param.declaredCapability) << " ";
                }
                std::cout << param.name << ": " << param.type << ")\n";
            }
            printExpr(*function->body, indent + 2);
            return;
        }

        if (const auto* structDecl = dynamic_cast<const StructDecl*>(&stmt))
        {
            std::cout << pad << "Struct(" << structDecl->name << ")\n";
            for (const auto& field : structDecl->fields)
            {
                std::cout << pad << "  Field(" << field.name << ": " << field.type << ")\n";
            }
            return;
        }
    }

    std::string readFile(const std::string& path)
    {
        std::ifstream input(path);
        if (!input)
        {
            throw std::runtime_error("could not open file: " + path);
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: ax <tokens|ast|run|capabilities> <file.ax>\n";
        return 1;
    }

    try
    {
        const std::string command = argv[1];
        const std::string source = readFile(argv[2]);

        Lexer lexer(source);
        auto tokens = lexer.lex();

        if (command == "tokens")
        {
            for (const auto& token : tokens)
            {
                std::cout << token.line << ':' << token.column << ' ' << tokenKindName(token.kind)
                          << "  " << token.text << '\n';
            }
            return 0;
        }

        if (command == "ast")
        {
            Parser parser(std::move(tokens));
            auto program = parser.parseProgram();

            for (const auto& item : program.items)
            {
                printStmt(*item);
            }
            return 0;
        }

        if (command == "run")
        {
            Parser parser(std::move(tokens));
            auto program = parser.parseProgram();

            TypeChecker typeChecker;
            typeChecker.check(program);

            CapabilityChecker capabilityChecker;
            capabilityChecker.check(program);

            Interpreter interpreter;
            interpreter.run(program);

            for (const auto& item : program.items)
            {
                if (const auto* assignment = dynamic_cast<const AssignmentStmt*>(item.get()))
                {
                    std::cout << assignment->name << " = "
                              << toString(interpreter.variables().at(assignment->name)) << '\n';
                }
            }
            return 0;
        }

        if (command == "capabilities")
        {
            Parser parser(std::move(tokens));
            auto program = parser.parseProgram();

            TypeChecker typeChecker;
            typeChecker.check(program);

            CapabilityChecker capabilityChecker;
            capabilityChecker.check(program);

            for (const auto& item : program.items)
            {
                const auto* function = dynamic_cast<const FunctionDecl*>(item.get());
                if (!function)
                {
                    continue;
                }

                std::cout << "Function(" << function->name << ")\n";
                const auto& capabilities =
                    capabilityChecker.effectiveCapabilities().at(function->name);
                for (std::size_t i = 0; i < function->params.size(); ++i)
                {
                    std::cout << "  Param(" << function->params[i].name << ": "
                              << capabilityName(capabilities[i]) << ")\n";
                }
            }
            return 0;
        }

        std::cerr << "unknown command: " << command << '\n';
        return 1;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
