#pragma once
#include "pch.h"

namespace TestHarness
{
    class TestFailureException : public std::exception
    {
    public:
        TestFailureException(std::string message, std::string file, int line)
            : m_message(std::move(message)), m_file(std::move(file)), m_line(line)
        {
        }

        const char* what() const noexcept override { return m_message.c_str(); }
        const std::string& File() const noexcept { return m_file; }
        int Line() const noexcept { return m_line; }

    private:
        std::string m_message;
        std::string m_file;
        int m_line;
    };

    struct TestCaseInfo
    {
        std::string suite;
        std::string name;
        std::function<void()> func;
    };

    class TestRegistry
    {
    public:
        static TestRegistry& Instance()
        {
            static TestRegistry s_instance;
            return s_instance;
        }

        void Register(std::string suite, std::string name, std::function<void()> func)
        {
            m_tests.push_back({ std::move(suite), std::move(name), std::move(func) });
        }

        const std::vector<TestCaseInfo>& Tests() const { return m_tests; }

        int Run(int argc, char* argv[])
        {
            // Enable ANSI color codes on Windows console
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD dwMode = 0;
            if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &dwMode))
            {
                SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }

            std::string filter;
            bool listOnly = false;

            for (int i = 1; i < argc; ++i)
            {
                std::string arg = argv[i];
                if ((arg == "--filter" || arg == "-f") && i + 1 < argc)
                {
                    filter = argv[++i];
                }
                else if (arg == "--list" || arg == "-l")
                {
                    listOnly = true;
                }
                else if (arg == "--help" || arg == "-h")
                {
                    std::cout << "Mandelbrot Viewer Unit Test Runner\n"
                              << "Usage: " << argv[0] << " [options]\n\n"
                              << "Options:\n"
                              << "  --filter, -f <text>   Run tests matching substring\n"
                              << "  --list, -l            List all registered tests\n"
                              << "  --help, -h            Show this help message\n";
                    return 0;
                }
            }

            if (listOnly)
            {
                std::cout << "Registered Tests (" << m_tests.size() << " total):\n";
                for (const auto& test : m_tests)
                {
                    std::cout << "  " << test.suite << "." << test.name << "\n";
                }
                return 0;
            }

            std::vector<const TestCaseInfo*> matchedTests;
            for (const auto& test : m_tests)
            {
                std::string fullName = test.suite + "." + test.name;
                if (filter.empty() || fullName.find(filter) != std::string::npos)
                {
                    matchedTests.push_back(&test);
                }
            }

            std::cout << "\033[1;36m======================================================================\033[0m\n";
            std::cout << "\033[1;37m                 MANDELBROT VIEWER UNIT TESTS                         \033[0m\n";
            std::cout << "\033[1;36m======================================================================\033[0m\n";
            if (!filter.empty())
            {
                std::cout << "Filter: \"" << filter << "\" (matched " << matchedTests.size() << " of " << m_tests.size() << " tests)\n\n";
            }
            else
            {
                std::cout << "Running " << matchedTests.size() << " test cases...\n\n";
            }

            int passedCount = 0;
            int failedCount = 0;
            const auto suiteStart = std::chrono::high_resolution_clock::now();

            for (const auto* pTest : matchedTests)
            {
                std::string fullName = pTest->suite + "." + pTest->name;
                std::cout << "\033[1;34m[ RUN      ]\033[0m " << fullName << "\n";

                bool testPassed = false;
                std::string failureMsg;
                std::string failureFile;
                int failureLine = 0;

                const auto testStart = std::chrono::high_resolution_clock::now();
                try
                {
                    pTest->func();
                    testPassed = true;
                }
                catch (const TestFailureException& ex)
                {
                    testPassed = false;
                    failureMsg = ex.what();
                    failureFile = ex.File();
                    failureLine = ex.Line();
                }
                catch (const std::exception& ex)
                {
                    testPassed = false;
                    failureMsg = std::string("Unhandled exception: ") + ex.what();
                }
                catch (...)
                {
                    testPassed = false;
                    failureMsg = "Unhandled non-std::exception thrown!";
                }

                const auto testEnd = std::chrono::high_resolution_clock::now();
                const std::chrono::duration<double, std::milli> elapsed = testEnd - testStart;

                if (testPassed)
                {
                    passedCount++;
                    std::cout << "\033[1;32m[       OK ]\033[0m " << fullName
                              << " (" << std::fixed << std::setprecision(2) << elapsed.count() << " ms)\n";
                }
                else
                {
                    failedCount++;
                    std::cout << "\033[1;31m[  FAILED  ]\033[0m " << fullName
                              << " (" << std::fixed << std::setprecision(2) << elapsed.count() << " ms)\n";
                    if (!failureFile.empty())
                    {
                        std::cout << "             \033[33m" << failureFile << ":" << failureLine << "\033[0m\n";
                    }
                    std::cout << "             \033[1;31m" << failureMsg << "\033[0m\n";
                }
            }

            const auto suiteEnd = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double, std::milli> suiteElapsed = suiteEnd - suiteStart;

            std::cout << "\033[1;36m======================================================================\033[0m\n";
            if (failedCount == 0)
            {
                std::cout << "\033[1;32m[  PASSED  ]\033[0m All " << passedCount << " tests passed! ("
                          << std::fixed << std::setprecision(1) << suiteElapsed.count() << " ms total)\n";
            }
            else
            {
                std::cout << "\033[1;31m[  FAILED  ]\033[0m " << failedCount << " of " << matchedTests.size()
                          << " tests failed! (" << std::fixed << std::setprecision(1) << suiteElapsed.count() << " ms total)\n";
            }
            std::cout << "\033[1;36m======================================================================\033[0m\n";

            return (failedCount == 0) ? 0 : 1;
        }

    private:
        TestRegistry() = default;
        std::vector<TestCaseInfo> m_tests;
    };

    struct TestRegistrar
    {
        TestRegistrar(const char* suite, const char* name, std::function<void()> func)
        {
            TestRegistry::Instance().Register(suite, name, std::move(func));
        }
    };
}

#define TEST_CASE(suite, name) \
    static void Test_##suite##_##name(); \
    static const ::TestHarness::TestRegistrar s_registrar_##suite##_##name(#suite, #name, Test_##suite##_##name); \
    static void Test_##suite##_##name()

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_TRUE(" #condition ")"; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_FALSE(condition) \
    do { \
        if (condition) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_FALSE(" #condition ") was true"; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_EQ(expected, actual) \
    do { \
        auto exp_val = (expected); \
        auto act_val = (actual); \
        if (!(exp_val == act_val)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_EQ(" #expected ", " #actual ")\n" \
               << "             Expected: " << exp_val << "\n" \
               << "             Actual:   " << act_val; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_NE(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (v1 == v2) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_NE(" #val1 ", " #val2 ") both equal " << v1; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_NEAR(expected, actual, tolerance) \
    do { \
        double exp_d = static_cast<double>(expected); \
        double act_d = static_cast<double>(actual); \
        double tol_d = static_cast<double>(tolerance); \
        double diff = std::abs(exp_d - act_d); \
        if (diff > tol_d) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_NEAR(" #expected ", " #actual ", " #tolerance ")\n" \
               << "             Expected:  " << std::setprecision(8) << exp_d << "\n" \
               << "             Actual:    " << std::setprecision(8) << act_d << "\n" \
               << "             Diff:      " << std::setprecision(8) << diff << " (tolerance: " << tol_d << ")"; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_GT(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 > v2)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_GT(" #val1 ", " #val2 ")\n" \
               << "             Value 1: " << v1 << "\n" \
               << "             Value 2: " << v2; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_LT(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 < v2)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_LT(" #val1 ", " #val2 ")\n" \
               << "             Value 1: " << v1 << "\n" \
               << "             Value 2: " << v2; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_GE(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 >= v2)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_GE(" #val1 ", " #val2 ")\n" \
               << "             Value 1: " << v1 << "\n" \
               << "             Value 2: " << v2; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_LE(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 <= v2)) { \
            std::ostringstream ss; \
            ss << "Assertion failed: ASSERT_LE(" #val1 ", " #val2 ")\n" \
               << "             Value 1: " << v1 << "\n" \
               << "             Value 2: " << v2; \
            throw ::TestHarness::TestFailureException(ss.str(), __FILE__, __LINE__); \
        } \
    } while (0)
