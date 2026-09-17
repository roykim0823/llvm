# RUN: %toy %s -emit=ast 2>&1 | FileCheck %s
# The AST dump makes parser behavior directly observable: node structure,
# operator precedence/associativity, and source locations.

def mul(a b) a * b + 1;
# CHECK: Module:
# CHECK: Function
# CHECK: Proto 'mul' @{{.*}}ast.k:5:5
# CHECK: Params: [a, b]
# '+' is the root: '*' binds tighter.
# CHECK: BinOp: +
# CHECK: BinOp: *
# CHECK: var: a
# CHECK: var: b

# Left associativity: a - b - c parses as (a - b) - c.
def assoc(a b c) a - b - c;
# CHECK: Proto 'assoc'
# CHECK: BinOp: -
# CHECK-NEXT: BinOp: -
# CHECK-NEXT: var: a
# CHECK-NEXT: var: b
# CHECK: var: c

extern sin(x);
# CHECK: Extern
# CHECK: Proto 'sin'

# A user-defined operator prototype records its precedence.
def binary| 5 (lhs rhs) if lhs then 1 else rhs;
# CHECK: Proto 'binary|'
# CHECK: binary op '|' prec: 5

1 + 2;
# CHECK: TopLevelExpr
# CHECK: Proto '__anon_expr0'
# CHECK: BinOp: +
