# RUN: not %toy %s -emit=ir 2>&1 | FileCheck %s
# Redefinitions and arity conflicts are caught by sema, with a note
# pointing at the earlier declaration (ChapterA generated IR against the
# old arity and failed confusingly inside the body).

def f(x) x;
def f(x) x + 1;
# CHECK: error-redefine.k:7:5: error: function 'f' cannot be redefined
# CHECK: error-redefine.k:6:5: note: previously defined here

extern g(x);
def g(a b) a + b;
# CHECK: error-redefine.k:12:5: error: conflicting declaration of 'g': 2 parameters, previously declared with 1
# CHECK: error-redefine.k:11:8: note: previous declaration is here
# CHECK: 2 errors generated.
