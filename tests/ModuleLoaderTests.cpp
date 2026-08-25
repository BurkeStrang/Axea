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
