/**
 * tests/test_gc.c - Garbage collector stress tests.
 */
#include "flux/flux.h"
#include "flux/common.h"
#include <stdio.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; printf("  [test] %-40s ... ", #name); } while (0)
#define PASS() \
    do { tests_passed++; printf("OK\n"); } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); } while (0)

static FluxResult run(FluxVM *vm, const char *src) {
    return flux_eval(vm, src, "<gc_test>");
}

static void test_many_allocations(void) {
    TEST(many_string_allocations);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    /* Allocate thousands of strings to trigger GC */
    FluxResult r = run(vm,
        "i = 0\n"
        "while i < 1000:\n"
        "    s = str(i) + \" hello world\"\n"
        "    i = i + 1\n"
        "assert(i == 1000)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("many allocations");
}

static void test_list_churn(void) {
    TEST(list_allocation_churn);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "i = 0\n"
        "while i < 500:\n"
        "    lst = [1, 2, 3, 4, 5]\n"
        "    lst.append(i)\n"
        "    i = i + 1\n"
        "assert(i == 500)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("list churn");
}

static void test_dict_churn(void) {
    TEST(dict_allocation_churn);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "i = 0\n"
        "while i < 300:\n"
        "    d = {\"key\": i, \"val\": i * 2}\n"
        "    i = i + 1\n"
        "assert(i == 300)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("dict churn");
}

static void test_closure_gc(void) {
    TEST(closure_gc);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func make_adder(n):\n"
        "    func add(x):\n"
        "        return x + n\n"
        "    return add\n"
        "i = 0\n"
        "while i < 200:\n"
        "    f = make_adder(i)\n"
        "    assert(f(1) == i + 1)\n"
        "    i = i + 1\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("closure gc");
}

static void test_class_gc(void) {
    TEST(class_instance_gc);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "class Node:\n"
        "    func init(v):\n"
        "        self.v = v\n"
        "i = 0\n"
        "while i < 300:\n"
        "    n = Node(i)\n"
        "    assert(n.v == i)\n"
        "    i = i + 1\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("class gc");
}

static void test_deep_recursion(void) {
    TEST(deep_recursion_gc);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func depth(n):\n"
        "    if n == 0:\n"
        "        return 0\n"
        "    return depth(n - 1) + 1\n"
        "assert(depth(100) == 100)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("deep recursion gc");
}

int main(void) {
    printf("=== GC Tests ===\n");
    test_many_allocations();
    test_list_churn();
    test_dict_churn();
    test_closure_gc();
    test_class_gc();
    test_deep_recursion();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
