#pragma once

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct StructInstance;

using Value =
    std::variant<std::int64_t, bool, std::string, std::shared_ptr<StructInstance>, std::monostate>;

struct StructInstance
{
    std::string typeName;
    std::vector<std::pair<std::string, Value>>
        fields; // declared field order, for deterministic printing
};

std::string toString(const Value& value);

class Environment
{
public:
    explicit Environment(const Environment* parent = nullptr);

    void define(const std::string& name, Value value);
    Value get(const std::string& name) const;

    const std::unordered_map<std::string, Value>& bindings() const;

private:
    std::unordered_map<std::string, Value> values_;
    const Environment* parent_;
};

class Interpreter
{
public:
    void run(const Program& program);

    Value evaluate(const Expr& expr, Environment& env);
    void execute(const Stmt& stmt, Environment& env);

    const std::unordered_map<std::string, Value>& variables() const;

private:
    Value callFunction(const FunctionDecl& decl, std::vector<Value> args);

    std::unordered_map<std::string, const FunctionDecl*> functions_;
    std::unordered_map<std::string, const StructDecl*> structs_;
    Environment globalEnv_;
};
