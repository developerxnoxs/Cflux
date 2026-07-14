/**
 * tests/test_vm.c - Integration tests for the Flux VM via the public API.
 */
#include "flux/flux.h"
#include "flux/common.h"
#include <stdio.h>
#include <string.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; printf("  [test] %-40s ... ", #name); } while (0)
#define PASS() \
    do { tests_passed++; printf("OK\n"); } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); } while (0)

static FluxResult run(FluxVM *vm, const char *src) {
    return flux_eval(vm, src, "<test>");
}

static void test_arithmetic(void) {
    TEST(arithmetic);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm, "x = 2 + 3 * 4\nassert(x == 14)");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("arithmetic failed");
}

static void test_string_concat(void) {
    TEST(string_concat);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm, "s = \"Hello\" + \" \" + \"World\"\nassert(s == \"Hello World\")");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("string concat failed");
}

static void test_if_else(void) {
    TEST(if_else);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "x = 10\n"
        "result = 0\n"
        "if x > 5:\n"
        "    result = 1\n"
        "else:\n"
        "    result = 2\n"
        "assert(result == 1)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("if/else failed");
}

static void test_while_loop(void) {
    TEST(while_loop);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "sum = 0\n"
        "i = 1\n"
        "while i <= 10:\n"
        "    sum = sum + i\n"
        "    i = i + 1\n"
        "assert(sum == 55)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("while loop failed");
}

static void test_for_range(void) {
    TEST(for_range);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "total = 0\n"
        "for i in range(5):\n"
        "    total = total + i\n"
        "assert(total == 10)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("for range failed");
}

static void test_function(void) {
    TEST(function_call);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func add(a, b):\n"
        "    return a + b\n"
        "result = add(3, 4)\n"
        "assert(result == 7)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("function call failed");
}

static void test_recursion(void) {
    TEST(recursion_fibonacci);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func fib(n):\n"
        "    if n <= 1:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "assert(fib(10) == 55)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("recursion failed");
}

static void test_closure(void) {
    TEST(closure_counter);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func make_counter():\n"
        "    n = 0\n"
        "    func inc():\n"
        "        n = n + 1\n"
        "        return n\n"
        "    return inc\n"
        "c = make_counter()\n"
        "assert(c() == 1)\n"
        "assert(c() == 2)\n"
        "assert(c() == 3)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("closure failed");
}

static void test_list(void) {
    TEST(list_operations);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "lst = [1, 2, 3]\n"
        "lst.append(4)\n"
        "assert(len(lst) == 4)\n"
        "assert(lst[0] == 1)\n"
        "assert(lst[3] == 4)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("list ops failed");
}

static void test_dict(void) {
    TEST(dict_operations);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "d = {\"a\": 1, \"b\": 2}\n"
        "assert(d[\"a\"] == 1)\n"
        "d[\"c\"] = 3\n"
        "assert(d[\"c\"] == 3)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("dict ops failed");
}

static void test_class(void) {
    TEST(class_and_instance);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "class Point:\n"
        "    func init(x, y):\n"
        "        self.x = x\n"
        "        self.y = y\n"
        "    func sum():\n"
        "        return self.x + self.y\n"
        "p = Point(3, 4)\n"
        "assert(p.sum() == 7)\n"
        "assert(p.x == 3)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("class failed");
}

static void test_inheritance(void) {
    TEST(class_inheritance);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "class Base:\n"
        "    func init(v):\n"
        "        self.v = v\n"
        "    func get():\n"
        "        return self.v\n"
        "class Child(Base):\n"
        "    func double():\n"
        "        return self.v * 2\n"
        "c = Child(5)\n"
        "assert(c.get() == 5)\n"
        "assert(c.double() == 10)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("inheritance failed");
}

static void test_bool_logic(void) {
    TEST(boolean_logic);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "assert(true and true)\n"
        "assert(not false)\n"
        "assert(true or false)\n"
        "assert(not (false and true))\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("boolean logic failed");
}

static void test_string_ops(void) {
    TEST(string_operations);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "s = \"hello world\"\n"
        "assert(len(s) == 11)\n"
        "assert(s[0] == \"h\")\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("string ops failed");
}

static void test_range(void) {
    TEST(range_function);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "r = range(5)\n"
        "assert(len(r) == 5)\n"
        "assert(r[0] == 0)\n"
        "assert(r[4] == 4)\n"
        "r2 = range(2, 8, 2)\n"
        "assert(len(r2) == 3)\n"
        "assert(r2[0] == 2)\n"
        "assert(r2[2] == 6)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("range failed");
}

static void test_while_loop_hoist(void) {
    TEST(while_loop_var_visible_after_loop);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func f():\n"
        "    i = 0\n"
        "    while i < 3:\n"
        "        x = i * 10\n"
        "        i = i + 1\n"
        "    return x\n"
        "assert(f() == 20)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("while-loop var not visible after loop");
}

static void test_for_loop_hoist(void) {
    TEST(for_loop_var_visible_after_loop);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func f():\n"
        "    for i in range(5):\n"
        "        last = i\n"
        "    return last\n"
        "assert(f() == 4)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("for-loop var not visible after loop");
}

static void test_scoping(void) {
    TEST(global_not_modified_by_function_assignment);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "g = 10\n"
        "func f():\n"
        "    g = 99\n"
        "f()\n"
        "assert(g == 10)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("global was modified by function assignment");
}

static void test_block_hoist(void) {
    TEST(block_assigned_var_visible_after_block);
    FluxVM *vm = flux_vm_new();
    flux_load_stdlib(vm);
    FluxResult r = run(vm,
        "func f():\n"
        "    if true:\n"
        "        x = 42\n"
        "    return x\n"
        "assert(f() == 42)\n");
    flux_vm_destroy(vm);
    if (r == FLUX_OK) PASS(); else FAIL("block-hoisted variable not visible after block");
}

int main(void) {
    printf("=== VM Tests ===\n");
    test_arithmetic();
    test_string_concat();
    test_if_else();
    test_while_loop();
    test_for_range();
    test_function();
    test_recursion();
    test_closure();
    test_list();
    test_dict();
    test_class();
    test_inheritance();
    test_bool_logic();
    test_string_ops();
    test_range();
    test_while_loop_hoist();
    test_for_loop_hoist();
    test_scoping();
    test_block_hoist();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
