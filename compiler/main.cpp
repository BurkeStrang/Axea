#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "interpreter/Interpreter.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    void printExpr(const Expr& expr, int indent = 0)
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
        std::cerr << "usage: ax <tokens|ast|run> <file.ax>\n";
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
            auto stmt = parser.parseStatement();

            const auto* assignment = dynamic_cast<const AssignmentStmt*>(stmt.get());
            if (!assignment)
            {
                throw std::runtime_error("unsupported statement");
            }

            std::cout << "Assignment(" << assignment->name << ")\n";
            printExpr(*assignment->value, 2);
            return 0;
        }

        if (command == "run")
        {
            Parser parser(std::move(tokens));
            auto stmt = parser.parseStatement();

            const auto* assignment = dynamic_cast<const AssignmentStmt*>(stmt.get());
            if (!assignment)
            {
                throw std::runtime_error("unsupported statement");
            }

            Interpreter interpreter;
            interpreter.execute(*stmt);
            std::cout << assignment->name << " = "
                      << toString(interpreter.variables().at(assignment->name)) << '\n';
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
