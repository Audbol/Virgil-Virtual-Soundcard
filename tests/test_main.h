// Tiny self-contained test harness (no external dependencies).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct TestCase {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<TestCase>& test_registry() {
  static std::vector<TestCase> r;
  return r;
}
inline int& test_failures() {
  static int f = 0;
  return f;
}
struct TestRegistrar {
  TestRegistrar(const char* n, std::function<void()> f) { test_registry().push_back({n, f}); }
};

#define TEST(name)                                          \
  static void test_##name();                                \
  static TestRegistrar reg_##name(#name, test_##name);      \
  static void test_##name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++test_failures();                                                         \
    }                                                                            \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
  do {                                                                           \
    double _a = double(a), _b = double(b);                                       \
    if (std::fabs(_a - _b) > (eps)) {                                            \
      std::printf("  FAIL %s:%d: %s = %g, expected %g (+-%g)\n", __FILE__,       \
                  __LINE__, #a, _a, _b, double(eps));                            \
      ++test_failures();                                                         \
    }                                                                            \
  } while (0)

#define TEST_MAIN()                                                              \
  int main() {                                                                   \
    for (auto& t : test_registry()) {                                            \
      int before = test_failures();                                              \
      t.fn();                                                                    \
      std::printf("%s %s\n", test_failures() == before ? "ok  " : "FAIL", t.name); \
    }                                                                            \
    std::printf("%d failure(s)\n", test_failures());                             \
    return test_failures() ? 1 : 0;                                              \
  }
