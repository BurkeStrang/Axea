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

        if (const auto* character = dynamic_cast<const CharExpr*>(&expr))
        {
            std::cout << pad << "Char(" << character->codepoint << ")\n";
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

        if (const auto* listNew = dynamic_cast<const ListNewExpr*>(&expr))
        {
            std::cout << pad << "ListNew(" << listNew->elementType << ")\n";
            return;
        }

        if (const auto* mapNew = dynamic_cast<const MapNewExpr*>(&expr))
        {
            std::cout << pad << "MapNew(" << mapNew->keyType << ", " << mapNew->valueType << ")\n";
            return;
        }

        if (const auto* setNew = dynamic_cast<const SetNewExpr*>(&expr))
        {
            std::cout << pad << "SetNew(" << setNew->elementType << ")\n";
            return;
        }

        if (const auto* sortedMapNew = dynamic_cast<const SortedMapNewExpr*>(&expr))
        {
            std::cout << pad << "SortedMapNew(" << sortedMapNew->keyType << ", "
                      << sortedMapNew->valueType << ")\n";
            return;
        }

        if (const auto* sortedSetNew = dynamic_cast<const SortedSetNewExpr*>(&expr))
        {
            std::cout << pad << "SortedSetNew(" << sortedSetNew->elementType << ")\n";
            return;
        }

        if (const auto* stringNew = dynamic_cast<const StringNewExpr*>(&expr))
        {
            // Unlike every other *New node above, `text` is a real
            // sub-expression, not a type-name string, so it recurses via
            // printExpr the same way MethodCallExpr's own object/arguments
            // do, rather than being inlined into the header line.
            std::cout << pad << "StringNew\n";
            printExpr(*stringNew->text, indent + 2);
            return;
        }

        if (dynamic_cast<const BufferNewExpr*>(&expr))
        {
            std::cout << pad << "BufferNew\n";
            return;
        }

        if (const auto* stackNew = dynamic_cast<const StackNewExpr*>(&expr))
        {
            std::cout << pad << "StackNew(" << stackNew->elementType << ")\n";
            return;
        }

        if (const auto* linkedListNew = dynamic_cast<const LinkedListNewExpr*>(&expr))
        {
            std::cout << pad << "LinkedListNew(" << linkedListNew->elementType << ")\n";
            return;
        }

        if (const auto* dequeNew = dynamic_cast<const DequeNewExpr*>(&expr))
        {
            std::cout << pad << "DequeNew(" << dequeNew->elementType << ")\n";
            return;
        }

        if (const auto* queueNew = dynamic_cast<const QueueNewExpr*>(&expr))
        {
            std::cout << pad << "QueueNew(" << queueNew->elementType << ")\n";
            return;
        }

        if (const auto* priorityQueueNew = dynamic_cast<const PriorityQueueNewExpr*>(&expr))
        {
            std::cout << pad << "PriorityQueueNew(" << priorityQueueNew->elementType << ")\n";
            return;
        }

        if (const auto* methodCall = dynamic_cast<const MethodCallExpr*>(&expr))
        {
            std::cout << pad << "MethodCall(" << methodCall->method << ")\n";
            printExpr(*methodCall->object, indent + 2);
            for (const auto& argument : methodCall->arguments)
            {
                printExpr(*argument, indent + 2);
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

        if (const auto* constChar = dynamic_cast<const IrConstChar*>(&inst))
        {
            std::cout << pad << "%" << constChar->dest << " = const.char " << constChar->codepoint
                      << "\n";
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

        if (const auto* listNew = dynamic_cast<const IrListNew*>(&inst))
        {
            std::cout << pad << "%" << listNew->dest << " = list.new " << listNew->elementTypeName
                      << "\n";
            return;
        }

        if (const auto* listPush = dynamic_cast<const IrListPush*>(&inst))
        {
            std::cout << pad << "%" << listPush->dest << " = list.push %" << listPush->list << ", %"
                      << listPush->value << "\n";
            return;
        }

        if (const auto* listPop = dynamic_cast<const IrListPop*>(&inst))
        {
            std::cout << pad << "%" << listPop->dest << " = list.pop %" << listPop->list << "\n";
            return;
        }

        if (const auto* stackNew = dynamic_cast<const IrStackNew*>(&inst))
        {
            std::cout << pad << "%" << stackNew->dest << " = stack.new "
                      << stackNew->elementTypeName << "\n";
            return;
        }

        if (const auto* stackPush = dynamic_cast<const IrStackPush*>(&inst))
        {
            std::cout << pad << "%" << stackPush->dest << " = stack.push %" << stackPush->stack
                      << ", %" << stackPush->value << "\n";
            return;
        }

        if (const auto* stackPop = dynamic_cast<const IrStackPop*>(&inst))
        {
            std::cout << pad << "%" << stackPop->dest << " = stack.pop %" << stackPop->stack
                      << "\n";
            return;
        }

        if (const auto* stackPeek = dynamic_cast<const IrStackPeek*>(&inst))
        {
            std::cout << pad << "%" << stackPeek->dest << " = stack.peek %" << stackPeek->stack
                      << "\n";
            return;
        }

        if (const auto* linkedListNew = dynamic_cast<const IrLinkedListNew*>(&inst))
        {
            std::cout << pad << "%" << linkedListNew->dest << " = linkedlist.new "
                      << linkedListNew->elementTypeName << "\n";
            return;
        }

        if (const auto* pushFront = dynamic_cast<const IrLinkedListPushFront*>(&inst))
        {
            std::cout << pad << "%" << pushFront->dest << " = linkedlist.push_front %"
                      << pushFront->list << ", %" << pushFront->value << "\n";
            return;
        }

        if (const auto* pushBack = dynamic_cast<const IrLinkedListPushBack*>(&inst))
        {
            std::cout << pad << "%" << pushBack->dest << " = linkedlist.push_back %"
                      << pushBack->list << ", %" << pushBack->value << "\n";
            return;
        }

        if (const auto* popFront = dynamic_cast<const IrLinkedListPopFront*>(&inst))
        {
            std::cout << pad << "%" << popFront->dest << " = linkedlist.pop_front %"
                      << popFront->list << "\n";
            return;
        }

        if (const auto* popBack = dynamic_cast<const IrLinkedListPopBack*>(&inst))
        {
            std::cout << pad << "%" << popBack->dest << " = linkedlist.pop_back %" << popBack->list
                      << "\n";
            return;
        }

        if (const auto* dequeNew = dynamic_cast<const IrDequeNew*>(&inst))
        {
            std::cout << pad << "%" << dequeNew->dest << " = deque.new "
                      << dequeNew->elementTypeName << "\n";
            return;
        }

        if (const auto* dequePushFront = dynamic_cast<const IrDequePushFront*>(&inst))
        {
            std::cout << pad << "%" << dequePushFront->dest << " = deque.push_front %"
                      << dequePushFront->deque << ", %" << dequePushFront->value << "\n";
            return;
        }

        if (const auto* dequePushBack = dynamic_cast<const IrDequePushBack*>(&inst))
        {
            std::cout << pad << "%" << dequePushBack->dest << " = deque.push_back %"
                      << dequePushBack->deque << ", %" << dequePushBack->value << "\n";
            return;
        }

        if (const auto* dequePopFront = dynamic_cast<const IrDequePopFront*>(&inst))
        {
            std::cout << pad << "%" << dequePopFront->dest << " = deque.pop_front %"
                      << dequePopFront->deque << "\n";
            return;
        }

        if (const auto* dequePopBack = dynamic_cast<const IrDequePopBack*>(&inst))
        {
            std::cout << pad << "%" << dequePopBack->dest << " = deque.pop_back %"
                      << dequePopBack->deque << "\n";
            return;
        }

        if (const auto* queueNew = dynamic_cast<const IrQueueNew*>(&inst))
        {
            std::cout << pad << "%" << queueNew->dest << " = queue.new "
                      << queueNew->elementTypeName << "\n";
            return;
        }

        if (const auto* queueEnqueue = dynamic_cast<const IrQueueEnqueue*>(&inst))
        {
            std::cout << pad << "%" << queueEnqueue->dest << " = queue.enqueue %"
                      << queueEnqueue->queue << ", %" << queueEnqueue->value << "\n";
            return;
        }

        if (const auto* queueDequeue = dynamic_cast<const IrQueueDequeue*>(&inst))
        {
            std::cout << pad << "%" << queueDequeue->dest << " = queue.dequeue %"
                      << queueDequeue->queue << "\n";
            return;
        }

        if (const auto* priorityQueueNew = dynamic_cast<const IrPriorityQueueNew*>(&inst))
        {
            std::cout << pad << "%" << priorityQueueNew->dest << " = priorityqueue.new "
                      << priorityQueueNew->elementTypeName << "\n";
            return;
        }

        if (const auto* priorityQueuePush = dynamic_cast<const IrPriorityQueuePush*>(&inst))
        {
            std::cout << pad << "%" << priorityQueuePush->dest << " = priorityqueue.push %"
                      << priorityQueuePush->priorityQueue << ", %" << priorityQueuePush->value
                      << "\n";
            return;
        }

        if (const auto* priorityQueuePop = dynamic_cast<const IrPriorityQueuePop*>(&inst))
        {
            std::cout << pad << "%" << priorityQueuePop->dest << " = priorityqueue.pop %"
                      << priorityQueuePop->priorityQueue << "\n";
            return;
        }

        if (const auto* priorityQueuePeek = dynamic_cast<const IrPriorityQueuePeek*>(&inst))
        {
            std::cout << pad << "%" << priorityQueuePeek->dest << " = priorityqueue.peek %"
                      << priorityQueuePeek->priorityQueue << "\n";
            return;
        }

        if (const auto* mapNew = dynamic_cast<const IrMapNew*>(&inst))
        {
            std::cout << pad << "%" << mapNew->dest << " = map.new " << mapNew->keyTypeName << ", "
                      << mapNew->valueTypeName << "\n";
            return;
        }

        if (const auto* mapSet = dynamic_cast<const IrMapSet*>(&inst))
        {
            std::cout << pad << "%" << mapSet->dest << " = map.set %" << mapSet->map << ", %"
                      << mapSet->key << ", %" << mapSet->value << "\n";
            return;
        }

        if (const auto* mapGet = dynamic_cast<const IrMapGet*>(&inst))
        {
            std::cout << pad << "%" << mapGet->dest << " = map.get %" << mapGet->map << ", %"
                      << mapGet->key << "\n";
            return;
        }

        if (const auto* mapContains = dynamic_cast<const IrMapContains*>(&inst))
        {
            std::cout << pad << "%" << mapContains->dest << " = map.contains %" << mapContains->map
                      << ", %" << mapContains->key << "\n";
            return;
        }

        if (const auto* mapRemove = dynamic_cast<const IrMapRemove*>(&inst))
        {
            std::cout << pad << "%" << mapRemove->dest << " = map.remove %" << mapRemove->map
                      << ", %" << mapRemove->key << "\n";
            return;
        }

        if (const auto* setNew = dynamic_cast<const IrSetNew*>(&inst))
        {
            std::cout << pad << "%" << setNew->dest << " = set.new " << setNew->elementTypeName
                      << "\n";
            return;
        }

        if (const auto* sortedMapNew = dynamic_cast<const IrSortedMapNew*>(&inst))
        {
            std::cout << pad << "%" << sortedMapNew->dest << " = sortedmap.new "
                      << sortedMapNew->keyTypeName << ", " << sortedMapNew->valueTypeName << "\n";
            return;
        }

        if (const auto* sortedMapSet = dynamic_cast<const IrSortedMapSet*>(&inst))
        {
            std::cout << pad << "%" << sortedMapSet->dest << " = sortedmap.set %"
                      << sortedMapSet->sortedMap << ", %" << sortedMapSet->key << ", %"
                      << sortedMapSet->value << "\n";
            return;
        }

        if (const auto* sortedMapGet = dynamic_cast<const IrSortedMapGet*>(&inst))
        {
            std::cout << pad << "%" << sortedMapGet->dest << " = sortedmap.get %"
                      << sortedMapGet->sortedMap << ", %" << sortedMapGet->key << "\n";
            return;
        }

        if (const auto* sortedMapContains = dynamic_cast<const IrSortedMapContains*>(&inst))
        {
            std::cout << pad << "%" << sortedMapContains->dest << " = sortedmap.contains %"
                      << sortedMapContains->sortedMap << ", %" << sortedMapContains->key << "\n";
            return;
        }

        if (const auto* sortedMapRemove = dynamic_cast<const IrSortedMapRemove*>(&inst))
        {
            std::cout << pad << "%" << sortedMapRemove->dest << " = sortedmap.remove %"
                      << sortedMapRemove->sortedMap << ", %" << sortedMapRemove->key << "\n";
            return;
        }

        if (const auto* sortedSetNew = dynamic_cast<const IrSortedSetNew*>(&inst))
        {
            std::cout << pad << "%" << sortedSetNew->dest << " = sortedset.new "
                      << sortedSetNew->elementTypeName << "\n";
            return;
        }

        if (const auto* sortedSetAdd = dynamic_cast<const IrSortedSetAdd*>(&inst))
        {
            std::cout << pad << "%" << sortedSetAdd->dest << " = sortedset.add %"
                      << sortedSetAdd->sortedSet << ", %" << sortedSetAdd->value << "\n";
            return;
        }

        if (const auto* sortedSetContains = dynamic_cast<const IrSortedSetContains*>(&inst))
        {
            std::cout << pad << "%" << sortedSetContains->dest << " = sortedset.contains %"
                      << sortedSetContains->sortedSet << ", %" << sortedSetContains->value << "\n";
            return;
        }

        if (const auto* sortedSetRemove = dynamic_cast<const IrSortedSetRemove*>(&inst))
        {
            std::cout << pad << "%" << sortedSetRemove->dest << " = sortedset.remove %"
                      << sortedSetRemove->sortedSet << ", %" << sortedSetRemove->value << "\n";
            return;
        }

        if (const auto* stringNew = dynamic_cast<const IrStringNew*>(&inst))
        {
            std::cout << pad << "%" << stringNew->dest << " = string.new %" << stringNew->text
                      << "\n";
            return;
        }

        if (const auto* stringAppend = dynamic_cast<const IrStringAppend*>(&inst))
        {
            std::cout << pad << "%" << stringAppend->dest << " = string.append %"
                      << stringAppend->string << ", %" << stringAppend->other << "\n";
            return;
        }

        if (const auto* bufferNew = dynamic_cast<const IrBufferNew*>(&inst))
        {
            std::cout << pad << "%" << bufferNew->dest << " = buffer.new\n";
            return;
        }

        if (const auto* bufferAppend = dynamic_cast<const IrBufferAppend*>(&inst))
        {
            std::cout << pad << "%" << bufferAppend->dest << " = buffer.append %"
                      << bufferAppend->buffer << ", %" << bufferAppend->text << "\n";
            return;
        }

        if (const auto* bufferAppendLine = dynamic_cast<const IrBufferAppendLine*>(&inst))
        {
            std::cout << pad << "%" << bufferAppendLine->dest << " = buffer.append_line %"
                      << bufferAppendLine->buffer << ", %" << bufferAppendLine->text << "\n";
            return;
        }

        if (const auto* bufferClear = dynamic_cast<const IrBufferClear*>(&inst))
        {
            std::cout << pad << "%" << bufferClear->dest << " = buffer.clear %"
                      << bufferClear->buffer << "\n";
            return;
        }

        if (const auto* bufferReserve = dynamic_cast<const IrBufferReserve*>(&inst))
        {
            std::cout << pad << "%" << bufferReserve->dest << " = buffer.reserve %"
                      << bufferReserve->buffer << ", %" << bufferReserve->capacity << "\n";
            return;
        }

        if (const auto* bufferFinish = dynamic_cast<const IrBufferFinish*>(&inst))
        {
            std::cout << pad << "%" << bufferFinish->dest << " = buffer.finish %"
                      << bufferFinish->buffer << "\n";
            return;
        }

        if (const auto* setAdd = dynamic_cast<const IrSetAdd*>(&inst))
        {
            std::cout << pad << "%" << setAdd->dest << " = set.add %" << setAdd->set << ", %"
                      << setAdd->value << "\n";
            return;
        }

        if (const auto* setContains = dynamic_cast<const IrSetContains*>(&inst))
        {
            std::cout << pad << "%" << setContains->dest << " = set.contains %" << setContains->set
                      << ", %" << setContains->value << "\n";
            return;
        }

        if (const auto* setRemove = dynamic_cast<const IrSetRemove*>(&inst))
        {
            std::cout << pad << "%" << setRemove->dest << " = set.remove %" << setRemove->set
                      << ", %" << setRemove->value << "\n";
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
