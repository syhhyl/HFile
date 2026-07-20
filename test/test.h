#ifndef HF_TEST_H
#define HF_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  void (*fn)(void);
} test_entry_t;

typedef struct {
  int total;
  int passed;
  int failed;
  const char *current;
  int test_aborted;
} test_runner_t;

static test_runner_t _runner;

static inline void _fail(const char *file, int line, const char *msg) {
  if (!_runner.test_aborted) {
    _runner.test_aborted = 1;
    _runner.failed++;
    fprintf(stderr, "  [FAIL] %s @ %s:%d — %s\n", _runner.current, file, line, msg);
  }
}

#define ASSERT(cond, msg) do { \
  if (!(cond)) { _fail(__FILE__, __LINE__, msg); return; } \
} while (0)

#define ASSERT_EQ(a, b) do { \
  long long _a = (long long)(a); \
  long long _b = (long long)(b); \
  if (_a != _b) { \
    fprintf(stderr, "  [FAIL] %s @ %s:%d — expected %lld, got %lld\n", \
            _runner.current, __FILE__, __LINE__, _b, _a); \
    _runner.failed++; _runner.test_aborted = 1; return; \
  } \
} while (0)

#define ASSERT_NE(a, b) do { \
  long long _a = (long long)(a); \
  long long _b = (long long)(b); \
  if (_a == _b) { \
    fprintf(stderr, "  [FAIL] %s @ %s:%d — expected different values\n", \
            _runner.current, __FILE__, __LINE__); \
    _runner.failed++; _runner.test_aborted = 1; return; \
  } \
} while (0)

#define ASSERT_STREQ(a, b) do { \
  const char *_sa = (a); \
  const char *_sb = (b); \
  if (strcmp(_sa, _sb) != 0) { \
    fprintf(stderr, "  [FAIL] %s @ %s:%d — expected \"%s\", got \"%s\"\n", \
            _runner.current, __FILE__, __LINE__, _sb, _sa); \
    _runner.failed++; _runner.test_aborted = 1; return; \
  } \
} while (0)

#define ASSERT_NULL(p) do { \
  if ((p) != NULL) { \
    fprintf(stderr, "  [FAIL] %s @ %s:%d — expected NULL\n", \
            _runner.current, __FILE__, __LINE__); \
    _runner.failed++; _runner.test_aborted = 1; return; \
  } \
} while (0)

#define ASSERT_NOT_NULL(p) do { \
  if ((p) == NULL) { \
    fprintf(stderr, "  [FAIL] %s @ %s:%d — expected non-NULL\n", \
            _runner.current, __FILE__, __LINE__); \
    _runner.failed++; _runner.test_aborted = 1; return; \
  } \
} while (0)

#define TEST(name) \
  static void test_##name(void)

typedef int (*test_setup_fn)(void);
typedef void (*test_teardown_fn)(void);

static inline void _run_tests(test_entry_t *entries, int count,
                              test_setup_fn setup,
                              test_teardown_fn teardown) {
  _runner.total = count;
  _runner.passed = 0;
  _runner.failed = 0;
  for (int i = 0; i < count; i++) {
    _runner.current = entries[i].name;
    _runner.test_aborted = 0;
    if (setup == NULL || setup() == 0) {
      entries[i].fn();
    } else {
      _fail(__FILE__, __LINE__, "fixture setup failed");
    }
    if (teardown != NULL) teardown();
    if (!_runner.test_aborted) {
      _runner.passed++;
      printf("  [PASS] %s\n", entries[i].name);
    }
  }
  printf("\n%d/%d passed", _runner.passed, _runner.total);
  if (_runner.failed > 0) printf(", %d failed", _runner.failed);
  printf("\n");
}

#define RUN_TESTS(...) do { \
  test_entry_t _entries[] = { __VA_ARGS__ }; \
  int _n = (int)(sizeof(_entries) / sizeof(_entries[0])); \
  _run_tests(_entries, _n, NULL, NULL); \
} while (0)

#define RUN_TESTS_WITH_FIXTURE(setup, teardown, ...) do { \
  test_entry_t _entries[] = { __VA_ARGS__ }; \
  int _n = (int)(sizeof(_entries) / sizeof(_entries[0])); \
  _run_tests(_entries, _n, setup, teardown); \
} while (0)

#define T(name) { #name, test_##name }

#endif /* HF_TEST_H */
