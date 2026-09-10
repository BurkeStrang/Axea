#include "TestFramework.hpp"

#include "ast/Expr.hpp"
#include "ast/Stmt.hpp"
#include "interpreter/Interpreter.hpp"
#include "module/ModuleLoader.hpp"
#include "sema/CapabilityChecker.hpp"
#include "sema/RegionChecker.hpp"
#include "sema/TypeChecker.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace
{
    // A fresh, uniquely-named scratch directory per test (see docs/language/0066-modules.md) -
    // loadProgram discovers modules by scanning the entry file's own directory, so every test
    // needs its own isolated directory rather than sharing one across the whole suite.
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
            : path(std::filesystem::temp_directory_path() /
                   ("axea_module_test_" + std::to_string(nextId())))
        {
            std::filesystem::create_directories(path);
        }

        static int nextId()
        {
            static std::atomic<int> counter{0};
            return counter++;
        }

        ~TempDir()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }

        std::string write(const std::string& filename, const std::string& content) const
        {
            const auto filePath = path / filename;
            std::ofstream out(filePath);
            out << content;
            out.close();
            return filePath.string();
        }
    };

    const FunctionDecl* findFunction(const Program& program, const std::string& name)
    {
        for (const auto& item : program.items)
        {
            if (const auto* function = dynamic_cast<const FunctionDecl*>(item.get());
                function && function->name == name)
            {
                return function;
            }
        }
        return nullptr;
    }

    const ExternDecl* findExtern(const Program& program, const std::string& name)
    {
        for (const auto& item : program.items)
        {
            if (const auto* externDecl = dynamic_cast<const ExternDecl*>(item.get());
                externDecl && externDecl->name == name)
            {
                return externDecl;
            }
        }
        return nullptr;
    }

    void checkAll(const Program& program)
    {
        TypeChecker typeChecker;
        typeChecker.check(program);
        CapabilityChecker capabilityChecker;
        capabilityChecker.check(program);
        RegionChecker regionChecker;
        regionChecker.check(program, capabilityChecker.effectiveCapabilities());
    }
} // namespace

TEST("ModuleLoader merges a discovered module's own FunctionDecl with its name qualified by the "
     "module name, exactly ImplDecl's own 'TypeName.methodName' mangling")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "pub square(x: i32) -> i32 { return x * x }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "y = math_utils.square(5)\n");

    Program merged = loadProgram(mainPath);
    EXPECT_TRUE(findFunction(merged, "math_utils.square") != nullptr);
    EXPECT_TRUE(findFunction(merged, "square") == nullptr);
}

TEST("ModuleLoader leaves an ExternDecl's own name bare (the real, externally-linked C symbol) "
     "and records its owning module in ExternDecl::moduleName instead of qualifying `name`")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "extern c abs(x: i32) -> i32\n"
              "pub magnitude(x: i32) -> i32 { return math_utils.abs(x) }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "y = math_utils.magnitude(0 - 3)\n");

    Program merged = loadProgram(mainPath);
    const ExternDecl* externDecl = findExtern(merged, "abs");
    EXPECT_TRUE(externDecl != nullptr);
    EXPECT_EQ(externDecl->moduleName, "math_utils");
    EXPECT_TRUE(findExtern(merged, "math_utils.abs") == nullptr);
}

TEST("ModuleLoader rewrites an aliased 'use math_utils as mu' call site to the real module name "
     "at parse time, so the merged program's own call is already qualified by the real name, "
     "never the alias")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "pub square(x: i32) -> i32 { return x * x }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils as mu\n"
                                           "y = mu.square(5)\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 25);
}

TEST("ModuleLoader follows a module's own transitive 'use' - a chain of two modules loads "
     "correctly")
{
    TempDir dir;
    dir.write("base.ax",
              "module base\n"
              "pub double(x: i32) -> i32 { return x * 2 }\n");
    dir.write("derived.ax",
              "module derived\n"
              "use base\n"
              "pub quadruple(x: i32) -> i32 { return base.double(base.double(x)) }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use derived\n"
                                           "y = derived.quadruple(3)\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 12);
}

TEST("ModuleLoader throws a clear error when a 'use'd module can't be found")
{
    TempDir dir;
    const std::string mainPath = dir.write("main.ax",
                                           "use nonexistent\n"
                                           "x = 1\n");
    EXPECT_THROWS(loadProgram(mainPath));
}

TEST("ModuleLoader rejects a module file with top-level executable code - only the entry file's "
     "own top-level code is meant to run")
{
    TempDir dir;
    dir.write("bad.ax",
              "module bad\n"
              "pub f() -> i32 { return 1 }\n"
              "y = 5\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use bad\n"
                                           "x = 1\n");
    EXPECT_THROWS(loadProgram(mainPath));
}

TEST("ModuleLoader's merged program end to end: TypeChecker rejects a qualified call into a "
     "module's own non-pub function from outside it, exactly like a real Rust-style private item")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "square(x: i32) -> i32 { return x * x }\n"
              "pub distance(x: i32, y: i32) -> i32 { "
              "  return math_utils.square(x) - math_utils.square(y) "
              "}\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "y = math_utils.square(5)\n");

    Program merged = loadProgram(mainPath);
    TypeChecker typeChecker;
    EXPECT_THROWS(typeChecker.check(merged));
}

TEST("ModuleLoader's merged program end to end: a module's own function calling a sibling "
     "(non-pub) function via its own fully-qualified name typechecks, runs, and compiles - a "
     "module's own internal code is exempt from the pub check external access needs")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "square(x: i32) -> i32 { return x * x }\n"
              "pub distance(x: i32, y: i32) -> i32 { "
              "  return math_utils.square(x) - math_utils.square(y) "
              "}\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "y = math_utils.distance(5, 3)\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 16);
}

TEST("ModuleLoader finds a module in a 'std' directory reached by walking up from the entry "
     "file's own directory, not just the entry file's own directory itself - the layout "
     "docs/language/0066-modules.md's own worked example (examples/modules/main.ax using this "
     "repo's own std/) depends on")
{
    TempDir dir;
    // A nested layout: <dir>/std/math.ax (a sibling of <dir>/app/), <dir>/app/main.ax as the
    // entry file. The entry file's own directory (<dir>/app) has no "math.ax" sibling at all -
    // it's only reachable by walking up to <dir> and finding <dir>/std there.
    std::filesystem::create_directories(dir.path / "std");
    std::filesystem::create_directories(dir.path / "app");
    {
        std::ofstream mathFile(dir.path / "std" / "math.ax");
        mathFile << "module math\n"
                    "pub square(x: i32) -> i32 { return x * x }\n";
    }
    std::string mainPath;
    {
        const auto mainFilePath = dir.path / "app" / "main.ax";
        std::ofstream mainFile(mainFilePath);
        mainFile << "use math\n"
                    "y = math.square(5)\n";
        mainPath = mainFilePath.string();
    }

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 25);
}

TEST("ModuleLoader prefers a module in the entry file's own directory over a same-named module "
     "in a 'std' directory found by walking up - the nearer, more specific match wins")
{
    TempDir dir;
    std::filesystem::create_directories(dir.path / "std");
    std::filesystem::create_directories(dir.path / "app");
    {
        std::ofstream farMath(dir.path / "std" / "math.ax");
        farMath << "module math\n"
                   "pub square(x: i32) -> i32 { return 0 - 1 }\n"; // wrong on purpose - must not
                                                                   // be the one picked
    }
    std::string mainPath;
    {
        std::ofstream nearMath(dir.path / "app" / "math.ax");
        nearMath << "module math\n"
                    "pub square(x: i32) -> i32 { return x * x }\n";
        const auto mainFilePath = dir.path / "app" / "main.ax";
        std::ofstream mainFile(mainFilePath);
        mainFile << "use math\n"
                    "y = math.square(5)\n";
        mainPath = mainFilePath.string();
    }

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 25);
}

TEST("ModuleLoader resolves a bare unqualified call to a used module's own public function, with "
     "no qualification and no special keyword needed")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "pub square(x: i32) -> i32 { return x * x }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "y = square(5)\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 25);
}

TEST("ModuleLoader resolves a bare unqualified call to a used module's own public GENERIC "
     "function - the rewrite happens before monomorphizeGenerics runs, so "
     "GenericMonomorphizer's own exact-match lookup sees the already-qualified callee")
{
    TempDir dir;
    dir.write("boxing.ax",
              "module boxing\n"
              "struct Box<T> { value: T }\n"
              "pub makeBox<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n");
    const std::string mainPath =
        dir.write("main.ax",
                  "use boxing\n"
                  "b: Box<i32> = makeBox<i32>(7)\n"
                  "y = b.value\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 7);
}

TEST("ModuleLoader lets a local, entry-file-declared function silently shadow a same-named "
     "public function from a used module - no error, the local one wins")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "pub square(x: i32) -> i32 { return x * x }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "square() -> i32 { return 999 }\n"
                                           "y = square()\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 999);
}

TEST("ModuleLoader allows two used modules to both export the same bare public function name "
     "with no error, as long as that bare name is never actually called unqualified")
{
    TempDir dir;
    dir.write("mod_a.ax", "module mod_a\npub frobnicate() -> i32 { return 1 }\n");
    dir.write("mod_b.ax", "module mod_b\npub frobnicate() -> i32 { return 2 }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use mod_a\n"
                                           "use mod_b\n"
                                           "y = mod_a.frobnicate()\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 1);
}

TEST("ModuleLoader throws a clear, both-modules-named error when a bare name colliding across "
     "two used modules IS actually called unqualified")
{
    TempDir dir;
    dir.write("mod_a.ax", "module mod_a\npub frobnicate() -> i32 { return 1 }\n");
    dir.write("mod_b.ax", "module mod_b\npub frobnicate() -> i32 { return 2 }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use mod_a\n"
                                           "use mod_b\n"
                                           "y = frobnicate()\n");

    EXPECT_THROWS(loadProgram(mainPath));
}

TEST("ModuleLoader leaves a qualified call and a genuinely unknown bare call both untouched - "
     "the unqualified-call resolver never masks a real 'unknown function' error")
{
    TempDir dir;
    dir.write("math_utils.ax",
              "module math_utils\n"
              "pub square(x: i32) -> i32 { return x * x }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use math_utils\n"
                                           "y = totallyUnknownFunction()\n");

    Program merged = loadProgram(mainPath);
    TypeChecker typeChecker;
    EXPECT_THROWS(typeChecker.check(merged));
}

TEST("ModuleLoader resolves TypeName<T>(args) construction sugar to a used module's own "
     "conventionally-named 'new' + TypeName constructor")
{
    TempDir dir;
    dir.write("boxing.ax",
              "module boxing\n"
              "struct Box<T> { value: T }\n"
              "pub newBox<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use boxing\n"
                                           "b = Box<i32>(7)\n"
                                           "y = b.value\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 7);
}

TEST("ModuleLoader resolves non-generic TypeName(args) construction sugar the same way")
{
    TempDir dir;
    dir.write("points.ax",
              "module points\n"
              "struct Point { x: i32  y: i32 }\n"
              "pub newPoint(x: i32, y: i32) -> Point { return Point { x: x, y: y } }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use points\n"
                                           "p = Point(3, 4)\n"
                                           "y = p.x + p.y\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 7);
}

TEST("ModuleLoader resolves TypeName<T>() construction sugar for a purely local struct + local "
     "constructor, with no used module (and no 'use' declaration) at all")
{
    TempDir dir;
    const std::string mainPath =
        dir.write("main.ax",
                  "struct Box<T> { value: T }\n"
                  "newBox<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n"
                  "b = Box<i32>(7)\n"
                  "y = b.value\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 7);
}

TEST("ModuleLoader leaves TypeName<T>() construction sugar untouched when no matching "
     "'new' + TypeName constructor exists anywhere - the ordinary downstream error surfaces "
     "unchanged")
{
    TempDir dir;
    const std::string mainPath =
        dir.write("main.ax", "struct Box<T> { value: T }\n"
                             "b = Box<i32>(7)\n");

    EXPECT_THROWS(loadProgram(mainPath));
}

TEST("ModuleLoader throws an ambiguous-constructor error when two used modules both provide a "
     "matching 'new' + TypeName constructor for the same struct-shaped bare call")
{
    TempDir dir;
    dir.write("mod_a.ax", "module mod_a\n"
                          "struct Box<T> { value: T }\n"
                          "pub newBox<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n");
    dir.write("mod_b.ax", "module mod_b\n"
                          "pub newBox<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n");
    const std::string mainPath = dir.write("main.ax",
                                           "use mod_a\n"
                                           "use mod_b\n"
                                           "b = Box<i32>(7)\n");

    EXPECT_THROWS(loadProgram(mainPath));
}

TEST("ModuleLoader lets a real function literally sharing a struct's own bare name win over "
     "the constructor-sugar fallback")
{
    TempDir dir;
    const std::string mainPath =
        dir.write("main.ax",
                  "struct Box<T> { value: T }\n"
                  "newBox<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n"
                  "Box<T>(v: T) -> Box<T> { return Box<T> { value: v } }\n"
                  "b = Box<i32>(7)\n"
                  "y = b.value\n");

    Program merged = loadProgram(mainPath);
    checkAll(merged);

    Interpreter interpreter;
    interpreter.run(merged);
    EXPECT_EQ(std::get<std::int64_t>(interpreter.variables().at("y")), 7);
}
