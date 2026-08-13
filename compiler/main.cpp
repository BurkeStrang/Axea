#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "interpreter/Interpreter.hpp"
#include "ir/IrGenerator.hpp"
#include "lexer/Lexer.hpp"
#include "llvmir/LlvmIrEmitter.hpp"
#include "parser/Parser.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/RegionChecker.hpp"
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

        if (const auto* loopExpr = dynamic_cast<const LoopExpr*>(&expr))
        {
            std::cout << pad << "Loop\n";
            printExpr(*loopExpr->body, indent + 2);
            return;
        }

        if (const auto* arrayLiteral = dynamic_cast<const ArrayLiteralExpr*>(&expr))
        {
            std::cout << pad << "ArrayLiteral\n";
            for (const auto& element : arrayLiteral->elements)
            {
                printExpr(*element, indent + 2);
            }
            return;
        }

        if (const auto* index = dynamic_cast<const IndexExpr*>(&expr))
        {
            std::cout << pad << "Index\n";
            printExpr(*index->object, indent + 2);
            printExpr(*index->index, indent + 2);
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

        if (const auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt))
        {
            std::cout << pad << "IndexAssign\n";
            printExpr(*indexAssign->object, indent + 2);
            printExpr(*indexAssign->index, indent + 2);
            printExpr(*indexAssign->value, indent + 2);
            return;
        }

        if (const auto* incDec = dynamic_cast<const IncDecStmt*>(&stmt))
        {
            std::cout << pad << (incDec->increment ? "Increment\n" : "Decrement\n");
            printExpr(*incDec->target, indent + 2);
            return;
        }

        if (const auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt))
        {
            std::cout << pad << "While\n";
            printExpr(*whileStmt->condition, indent + 2);
            printExpr(*whileStmt->body, indent + 2);
            return;
        }

        if (const auto* breakStmt = dynamic_cast<const BreakStmt*>(&stmt))
        {
            std::cout << pad << "Break\n";
            if (breakStmt->value)
            {
                printExpr(*breakStmt->value, indent + 2);
            }
            return;
        }

        if (dynamic_cast<const ContinueStmt*>(&stmt))
        {
            std::cout << pad << "Continue\n";
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

    void printIrInst(const IrInst& inst, int indent)
    {
        const std::string pad(static_cast<std::size_t>(indent), ' ');

        if (const auto* constInt = dynamic_cast<const IrConstInt*>(&inst))
        {
            std::cout << pad << "%" << constInt->dest << " = const.i32 " << constInt->value << "\n";
            return;
        }

        if (const auto* constBool = dynamic_cast<const IrConstBool*>(&inst))
        {
            std::cout << pad << "%" << constBool->dest << " = const.bool "
                      << (constBool->value ? "true" : "false") << "\n";
            return;
        }

        if (const auto* constString = dynamic_cast<const IrConstString*>(&inst))
        {
            std::cout << pad << "%" << constString->dest << " = const.str \"" << constString->value
                      << "\"\n";
            return;
        }

        if (const auto* binOp = dynamic_cast<const IrBinOp*>(&inst))
        {
            std::cout << pad << "%" << binOp->dest << " = binop " << tokenKindName(binOp->op)
                      << " %" << binOp->lhs << ", %" << binOp->rhs << "\n";
            return;
        }

        if (const auto* call = dynamic_cast<const IrCall*>(&inst))
        {
            std::cout << pad << "%" << call->dest << " = call " << call->callee << "(";
            for (std::size_t i = 0; i < call->args.size(); ++i)
            {
                std::cout << (i > 0 ? ", " : "") << "%" << call->args[i];
            }
            std::cout << ")\n";
            return;
        }

        if (const auto* structNew = dynamic_cast<const IrStructNew*>(&inst))
        {
            std::cout << pad << "%" << structNew->dest << " = struct.new " << structNew->typeName
                      << " {";
            for (std::size_t i = 0; i < structNew->fields.size(); ++i)
            {
                std::cout << (i > 0 ? "," : "") << " " << structNew->fields[i].first << ": %"
                          << structNew->fields[i].second;
            }
            std::cout << " }\n";
            return;
        }

        if (const auto* fieldGet = dynamic_cast<const IrFieldGet*>(&inst))
        {
            std::cout << pad << "%" << fieldGet->dest << " = field.get %" << fieldGet->object << "."
                      << fieldGet->field << "\n";
            return;
        }

        if (const auto* fieldSet = dynamic_cast<const IrFieldSet*>(&inst))
        {
            std::cout << pad << "field.set %" << fieldSet->object << "." << fieldSet->field
                      << " = %" << fieldSet->value << "\n";
            return;
        }

        if (const auto* arrayNew = dynamic_cast<const IrArrayNew*>(&inst))
        {
            std::cout << pad << "%" << arrayNew->dest << " = array.new [";
            for (std::size_t i = 0; i < arrayNew->elements.size(); ++i)
            {
                std::cout << (i > 0 ? ", " : "") << "%" << arrayNew->elements[i];
            }
            std::cout << "]\n";
            return;
        }

        if (const auto* indexGet = dynamic_cast<const IrIndexGet*>(&inst))
        {
            std::cout << pad << "%" << indexGet->dest << " = index.get %" << indexGet->object << "["
                      << "%" << indexGet->index << "]\n";
            return;
        }

        if (const auto* indexSet = dynamic_cast<const IrIndexSet*>(&inst))
        {
            std::cout << pad << "index.set %" << indexSet->object << "[%" << indexSet->index
                      << "] = %" << indexSet->value << "\n";
            return;
        }

        if (const auto* branch = dynamic_cast<const IrBranch*>(&inst))
        {
            std::cout << pad << "%" << branch->dest << " = br %" << branch->condition << " {\n";
            for (const auto& thenInst : branch->thenBlock)
            {
                printIrInst(*thenInst, indent + 2);
            }
            std::cout << pad << "} (-> %" << branch->thenValue << ") else {\n";
            for (const auto& elseInst : branch->elseBlock)
            {
                printIrInst(*elseInst, indent + 2);
            }
            std::cout << pad << "} (-> %" << branch->elseValue << ")\n";
            return;
        }

        if (const auto* loop = dynamic_cast<const IrLoop*>(&inst))
        {
            std::cout << pad << "%" << loop->dest << " = loop";
            if (!loop->conditionBlock.empty() || loop->conditionValue != -1)
            {
                std::cout << " while {\n";
                for (const auto& condInst : loop->conditionBlock)
                {
                    printIrInst(*condInst, indent + 2);
                }
                std::cout << pad << "} (-> %" << loop->conditionValue << ") {\n";
            }
            else
            {
                std::cout << " {\n";
            }
            for (const auto& bodyInst : loop->body)
            {
                printIrInst(*bodyInst, indent + 2);
            }
            std::cout << pad << "}";
            if (!loop->carried.empty())
            {
                std::cout << " carried:";
                for (const auto& [before, after] : loop->carried)
                {
                    std::cout << " (%" << before << " -> %" << after << ")";
                }
            }
            std::cout << "\n";
            return;
        }

        if (const auto* breakInst = dynamic_cast<const IrBreak*>(&inst))
        {
            std::cout << pad << "break";
            if (breakInst->value != -1)
            {
                std::cout << " %" << breakInst->value;
            }
            for (const auto& [before, after] : breakInst->carried)
            {
                std::cout << " (%" << before << " -> %" << after << ")";
            }
            std::cout << "\n";
            return;
        }

        if (const auto* continueInst = dynamic_cast<const IrContinue*>(&inst))
        {
            std::cout << pad << "continue";
            for (const auto& [before, after] : continueInst->carried)
            {
                std::cout << " (%" << before << " -> %" << after << ")";
            }
            std::cout << "\n";
            return;
        }

        if (const auto* returnInst = dynamic_cast<const IrReturn*>(&inst))
        {
            if (returnInst->value == -1)
            {
                std::cout << pad << "return\n";
            }
            else
            {
                std::cout << pad << "return %" << returnInst->value << "\n";
            }
            return;
        }

        if (const auto* borrowRead = dynamic_cast<const IrBorrowRead*>(&inst))
        {
            std::cout << pad << "borrow.read %" << borrowRead->value << "\n";
            return;
        }

        if (const auto* borrowWrite = dynamic_cast<const IrBorrowWrite*>(&inst))
        {
            std::cout << pad << "borrow.write %" << borrowWrite->value << "\n";
            return;
        }

        if (const auto* move = dynamic_cast<const IrMove*>(&inst))
        {
            std::cout << pad << "move %" << move->value << "\n";
            return;
        }

        if (dynamic_cast<const IrRegionEnter*>(&inst))
        {
            std::cout << pad << "region.enter\n";
            return;
        }

        if (dynamic_cast<const IrRegionExit*>(&inst))
        {
            std::cout << pad << "region.exit\n";
            return;
        }

        if (const auto* drop = dynamic_cast<const IrDrop*>(&inst))
        {
            std::cout << pad << "drop %" << drop->value << "\n";
            return;
        }
    }

    void printIrFunction(const IrFunction& function)
    {
        std::cout << "Function(" << function.name << ")\n";
        std::cout << "  Params:";
        for (std::size_t i = 0; i < function.paramNames.size(); ++i)
        {
            std::cout << " %" << i << "=" << function.paramNames[i];
        }
        std::cout << "\n";
        for (const auto& inst : function.body)
        {
            printIrInst(*inst, 2);
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
        std::cerr << "usage: ax <tokens|ast|run|capabilities|regions|ir|llvm-ir> <file.ax>\n";
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

            RegionChecker regionChecker;
            regionChecker.check(program, capabilityChecker.effectiveCapabilities());

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

        if (command == "regions")
        {
            Parser parser(std::move(tokens));
            auto program = parser.parseProgram();

            TypeChecker typeChecker;
            typeChecker.check(program);

            CapabilityChecker capabilityChecker;
            capabilityChecker.check(program);

            RegionChecker regionChecker;
            regionChecker.check(program, capabilityChecker.effectiveCapabilities());

            auto isStructType = [&program](const std::string& typeName)
            {
                for (const auto& item : program.items)
                {
                    if (const auto* structDecl = dynamic_cast<const StructDecl*>(item.get()))
                    {
                        if (structDecl->name == typeName)
                        {
                            return true;
                        }
                    }
                }
                return false;
            };

            for (const auto& item : program.items)
            {
                const auto* function = dynamic_cast<const FunctionDecl*>(item.get());
                if (!function || !function->returnType || !isStructType(*function->returnType))
                {
                    continue;
                }

                std::cout << "Function(" << function->name << ")\n";
                const auto& regions = regionChecker.regions().at(function->name);
                for (std::size_t i = 0; i < function->params.size(); ++i)
                {
                    std::cout << "  Param(" << function->params[i].name << ": "
                              << regionName(regions[i]) << ")\n";
                }
            }
            return 0;
        }

        if (command == "ir")
        {
            Parser parser(std::move(tokens));
            auto program = parser.parseProgram();

            TypeChecker typeChecker;
            typeChecker.check(program);

            CapabilityChecker capabilityChecker;
            capabilityChecker.check(program);

            RegionChecker regionChecker;
            regionChecker.check(program, capabilityChecker.effectiveCapabilities());

            IrGenerator irGenerator;
            auto irProgram = irGenerator.generate(
                program, capabilityChecker.effectiveCapabilities(), regionChecker.regions());

            for (const auto& function : irProgram.functions)
            {
                printIrFunction(function);
            }

            if (!irProgram.topLevel.empty())
            {
                std::cout << "TopLevel\n";
                for (const auto& inst : irProgram.topLevel)
                {
                    printIrInst(*inst, 2);
                }
            }
            return 0;
        }

        if (command == "llvm-ir")
        {
            Parser parser(std::move(tokens));
            auto program = parser.parseProgram();

            TypeChecker typeChecker;
            typeChecker.check(program);

            CapabilityChecker capabilityChecker;
            capabilityChecker.check(program);

            RegionChecker regionChecker;
            regionChecker.check(program, capabilityChecker.effectiveCapabilities());

            IrGenerator irGenerator;
            auto irProgram = irGenerator.generate(
                program, capabilityChecker.effectiveCapabilities(), regionChecker.regions());

            LlvmIrEmitter emitter;
            std::cout << emitter.emit(irProgram);
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
