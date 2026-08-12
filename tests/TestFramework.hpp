#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace axea::test
{
    struct Case
    {
        std::string name;
        void (*fn)();
    };

    inline std::vector<Case>& registry()
    {
        static std::vector<Case> cases;
        return cases;
    }

    struct Registrar
    {
        Registrar(std::string name, void (*fn)())
        {
            registry().push_back({std::move(name), fn});
        }
    };

    struct Failure
    {
        std::string message;
    };

    [[noreturn]] inline void fail(const char* file, int line, const std::string& message)
    {
        std::ostringstream oss;
        oss << file << ':' << line << ": " << message;
        throw Failure{oss.str()};
    }

    inline int runAll()
    {
        int failed = 0;
        for (const auto& testCase : registry())
        {
            try
            {
                testCase.fn();
                std::cout << "[PASS] " << testCase.name << '\n';
            }
            catch (const Failure& failure)
            {
                ++failed;
                std::cout << "[FAIL] " << testCase.name << ": " << failure.message << '\n';
            }
            catch (const std::exception& ex)
            {
                ++failed;
                std::cout << "[FAIL] " << testCase.name << ": unexpected exception: " << ex.what()
                          << '\n';
            }
        }

        const auto total = registry().size();
        std::cout << (total - static_cast<std::size_t>(failed)) << '/' << total
                  << " tests passed\n";
        return failed == 0 ? 0 : 1;
    }
} // namespace axea::test

#define AXEA_TEST_CONCAT_INNER(a, b) a##b
#define AXEA_TEST_CONCAT(a, b) AXEA_TEST_CONCAT_INNER(a, b)

#define TEST(name)                                                                                 \
    static void AXEA_TEST_CONCAT(axea_test_fn_, __LINE__)();                                       \
    static axea::test::Registrar AXEA_TEST_CONCAT(axea_test_reg_, __LINE__)(                       \
        name, &AXEA_TEST_CONCAT(axea_test_fn_, __LINE__));                                         \
    static void AXEA_TEST_CONCAT(axea_test_fn_, __LINE__)()

#define EXPECT_TRUE(expr)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            axea::test::fail(__FILE__, __LINE__, "expected true: " #expr);                         \
        }                                                                                          \
    } while (false)

#define EXPECT_EQ(actual, expected)                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!((actual) == (expected)))                                                             \
        {                                                                                          \
            axea::test::fail(__FILE__, __LINE__, "expected " #actual " == " #expected);            \
        }                                                                                          \
    } while (false)

#define EXPECT_THROWS(expr)                                                                        \
    do                                                                                             \
    {                                                                                              \
        bool axea_test_threw = false;                                                              \
        try                                                                                        \
        {                                                                                          \
            (void)(expr);                                                                          \
        }                                                                                          \
        catch (const std::exception&)                                                              \
        {                                                                                          \
            axea_test_threw = true;                                                                \
        }                                                                                          \
        if (!axea_test_threw)                                                                      \
        {                                                                                          \
            axea::test::fail(__FILE__, __LINE__, "expected throw: " #expr);                        \
        }                                                                                          \
    } while (false)
