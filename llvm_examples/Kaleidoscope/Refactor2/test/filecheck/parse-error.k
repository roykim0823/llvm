# RUN: %toy < %s 2>&1 | FileCheck %s
# The driver must report errors and keep going (error recovery), never crash.

def 123foo(x) x;
# CHECK: Error: Expected function name in prototype

def bar(x) x;
# CHECK: Parsed a function definition.

def baz(x x;
# CHECK: Error: Expected ')' in prototype
