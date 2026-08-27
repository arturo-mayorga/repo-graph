// Micro test harness. A dependency-free harness keeps the headless path buildable
// with nothing but a compiler, which matters because that path is the one that runs
// in CI and on machines without a GPU.
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace rgvtest {

struct Case {
    std::string           name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct Failure : std::exception {
    std::string msg;
    explicit Failure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

inline void check(bool cond, const char* expr, const char* file, int line,
                  const std::string& extra = {}) {
    if (cond) return;
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + expr +
                  (extra.empty() ? "" : "  [" + extra + "]"));
}

inline int run_all() {
    int failed = 0;
    for (auto& c : registry()) {
        try {
            c.fn();
            std::cout << "  ok   " << c.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cout << "  FAIL " << c.name << "\n         " << ex.what() << "\n";
        }
    }
    std::cout << "\n" << registry().size() - failed << "/" << registry().size()
              << " passed\n";
    return failed ? 1 : 0;
}

} // namespace rgvtest

#define TEST(name)                                                                 \
    static void name();                                                            \
    static ::rgvtest::Registrar reg_##name(#name, name);                           \
    static void name()

#define CHECK(cond) ::rgvtest::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                             \
    ::rgvtest::check((a) == (b), #a " == " #b, __FILE__, __LINE__,                 \
                     ::rgvtest::show(a) + " vs " + ::rgvtest::show(b))

namespace rgvtest {
template <class T> std::string show(const T& v) {
    if constexpr (std::is_convertible_v<T, std::string>) return std::string(v);
    else return std::to_string(v);
}
} // namespace rgvtest
