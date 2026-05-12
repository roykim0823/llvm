# LLVM TableGen — A Step-by-Step Tutorial

This tutorial walks you through LLVM's **TableGen** language from the ground up, based on the official *TableGen Programmer's Reference*. Each lesson introduces one new concept, gives you a complete file you can run with `llvm-tblgen`, and shows the expected output.

> **What is TableGen?** TableGen is a declarative language used inside LLVM to describe large, repetitive data tables (registers, instructions, intrinsics, AST nodes, diagnostics…). You write `.td` files containing **classes** (templates) and **records** (concrete data). A *backend* program then reads those records and emits C++ `.inc` files (or anything else it wants).

---

## Table of Contents

0. [Prerequisites & Setup](#lesson-0--prerequisites--setup)
1. [Your First Record](#lesson-1--your-first-record)
2. [Classes and Inheritance](#lesson-2--classes-and-inheritance)
3. [Types: bit, int, bits<N>, string, list, dag, code](#lesson-3--types)
4. [Template Arguments](#lesson-4--template-arguments)
5. [The `let` Statement](#lesson-5--the-let-statement)
6. [Values, Expressions, and Bang Operators](#lesson-6--values-expressions-and-bang-operators)
7. [The Paste Operator `#`](#lesson-7--the-paste-operator-)
8. [`multiclass` and `defm`](#lesson-8--multiclass-and-defm)
9. [`defvar`, `defset`, `deftype`](#lesson-9--defvar-defset-deftype)
10. [Control Flow: `foreach`, `if`, `assert`, `dump`](#lesson-10--control-flow)
11. [DAGs — Directed Acyclic Graphs](#lesson-11--dags)
12. [Classes as Subroutines](#lesson-12--classes-as-subroutines)
13. [Preprocessing (`#define`, `#ifdef`, `#ifndef`)](#lesson-13--preprocessing)
14. [Capstone: A Mini Toy ISA](#lesson-14--capstone--a-mini-toy-isa)
15. [Appendix: Bang-Operator Cheat Sheet](#appendix--bang-operator-cheat-sheet)

---

## Lesson 0 — Prerequisites & Setup

### What you need
- A built copy of LLVM, or an installed package that includes the `llvm-tblgen` binary.
- A text editor.

### Verify the tool
```bash
llvm-tblgen --version
```
You should see LLVM's version info. If you don't, install LLVM (e.g. `apt install llvm`, `brew install llvm`, or build from source).

### The two flags you'll use most
| Flag | Meaning |
|---|---|
| `--print-records` | Dump every record TableGen builds (the *default* if no backend is specified). |
| `--print-detailed-records` | Same, plus inheritance info, classes, defsets, etc. |
| `-I <dir>` | Add an include search path. |
| `-D <macro>` | Define a preprocessor macro from the command line. |

### How to "run" every example in this tutorial
Save the snippet as `example.td`, then:
```bash
llvm-tblgen --print-records example.td
```
That's it — every lesson uses this command unless noted otherwise.

---

## Lesson 1 — Your First Record

### Concepts
- A **record** is a named bag of typed fields, defined with `def`.
- Fields have a **type** and an optional **initial value**.
- An uninitialized value is written `?`.

### Code — `01_first.td`
```tablegen
def Apple {
  string Color = "red";
  int    Weight = 150;        // grams
  bit    Edible = 1;
  string Origin = ?;          // uninitialized
}
```

### Run it
```bash
llvm-tblgen --print-records 01_first.td
```

### Expected output (excerpt)
```
def Apple {
  string Color = "red";
  int Weight = 150;
  bit Edible = 1;
  string Origin = ?;
}
```

### Key takeaways
- `def NAME { ... }` defines a single record.
- The type **must** be written explicitly before each field name — TableGen does *not* infer the type of a field from its initializer.
- `?` is a real value meaning "no value yet."

### Try it yourself
1. Add a `list<int>` field called `SeedCounts` initialized to `[2, 3, 5]`.
2. Add a `bits<4>` field called `RipenessScale` set to `0b1010`.

---

## Lesson 2 — Classes and Inheritance

### Concepts
- A **class** (`class`) is an *abstract* record — a template that other records can inherit from.
- A `def D : C` says "record `D` inherits the fields of class `C`."
- Inherited fields can be **overridden** in the body of the derived record using `let`.

### Code — `02_class.td`
```tablegen
class Fruit {
  string Color  = "unknown";
  bit    Edible = 1;
}

def Apple  : Fruit { let Color = "red"; }
def Lemon  : Fruit { let Color = "yellow"; }
def Acorn  : Fruit { let Edible = 0; }      // keeps Color = "unknown"
```

### Expected output
```
def Acorn {     // Fruit
  string Color = "unknown";
  bit Edible = 0;
}
def Apple {     // Fruit
  string Color = "red";
  bit Edible = 1;
}
def Lemon {     // Fruit
  string Color = "yellow";
  bit Edible = 1;
}
```

Note the comment `// Fruit` next to each record — that's TableGen telling you which classes the record inherits.

### Multiple inheritance
A record can inherit from any number of classes:
```tablegen
class HasPrice  { int Price = 0; }
class HasColor  { string Color = "white"; }

def Banana : HasPrice, HasColor {
  let Price = 50;
  let Color = "yellow";
}
```

If two parent classes define the **same field**, the *last* parent's value wins (before the record body's own `let`s).

### Try it yourself
- Define a class `Vehicle` with `int Wheels = 4;` and `bit Motorized = 1;`.
- Define `Bicycle`, `Truck`, and `Skateboard` records inheriting from it, overriding fields appropriately.

---

## Lesson 3 — Types

TableGen's built-in types:

| Type | Meaning |
|---|---|
| `bit` | A single boolean (0 or 1). |
| `int` | A signed 64-bit integer. |
| `bits<N>` | A fixed-width vector of N bits — *individual bits are addressable*. |
| `string` | An ordered sequence of characters. |
| `code` | Alias for `string`, traditionally used for code blocks `[{ ... }]`. |
| `list<T>` | A homogenous list of type T. |
| `dag` | A directed-acyclic-graph node (see Lesson 11). |
| `ClassName` | The value must be a record that inherits from `ClassName`. |

### Code — `03_types.td`
```tablegen
class Anything;
def  X : Anything;
def  Y : Anything;

def TypeShowcase {
  bit        Flag   = 1;
  int        N      = -42;
  bits<8>    Opcode = 0b00010110;       // 8 bits
  string     S      = "hello";
  string     Big    = "abc"  "def";     // adjacent literals concatenate -> "abcdef"
  code       Body   = [{
    return x + 1;
  }];
  list<int>  Primes = [2, 3, 5, 7];
  list<list<int>> Matrix = [[1,2],[3,4]];
  Anything   Ref    = X;                // a typed record reference
  list<Anything> Refs = [X, Y];
}
```

### Accessing individual bits
You can pull out a bit or a slice of a `bits<N>` field using **braces**:

```tablegen
def Demo {
  bits<8> Op  = 0b10110010;
  bit     B0  = Op{0};        // -> 0   (least significant)
  bit     B7  = Op{7};        // -> 1
  bits<4> Top = Op{7...4};    // -> { 1, 0, 1, 1 }
}
```

> **Bit endianness:** `{7...4}` means "from bit 7 down to bit 4." `{4...7}` means the **same range reversed**.

### Try it yourself
1. Make a record with a `bits<16>` field for an instruction word.
2. Extract the top 6 bits as a `bits<6>` opcode field and the bottom 10 as a `bits<10>` immediate field.

---

## Lesson 4 — Template Arguments

A class can take **template arguments** in angle brackets `< ... >`. They're like function parameters — they let one class generate many records.

### Code — `04_templates.td`
```tablegen
class Register<string n, int num> {
  string Name   = n;
  int    Number = num;
  bit    IsCallerSaved = 0;            // default
}

def R0 : Register<"r0", 0>;
def R1 : Register<"r1", 1>;
def R2 : Register<"r2", 2> { let IsCallerSaved = 1; }
```

### Expected output
```
def R0 {        // Register
  string Name = "r0";
  int Number = 0;
  bit IsCallerSaved = 0;
}
def R1 {        // Register
  string Name = "r1";
  int Number = 1;
  bit IsCallerSaved = 0;
}
def R2 {        // Register
  string Name = "r2";
  int Number = 2;
  bit IsCallerSaved = 1;
}
```

### Default values & required vs optional arguments
```tablegen
class Inst<int opc, string mnem = "?", bit hasSideFx = 0> {
  int    Opcode    = opc;
  string Mnemonic  = mnem;
  bit    HasSideFx = hasSideFx;
}

def ADD : Inst<0x01, "add">;     // uses default HasSideFx = 0
def NOP : Inst<0x00>;            // uses two defaults
def OUT : Inst<0x10, "out", 1>;  // overrides everything
```

> **Rule:** all *required* arguments (no `=`) must come before any *optional* ones.

### Positional vs named arguments
```tablegen
def DIV : Inst<opc=0x20, mnem="div">;     // named
def MUL : Inst<0x21, hasSideFx=1>;        // mix: positional then named
```

> Positional must come before named, and you can't specify the same argument twice.

### The implicit `NAME` template argument
Every class has a hidden template argument **`NAME`** that holds the name of the `def`/`defm` that's instantiating it. This becomes very powerful with multiclasses (Lesson 8). For now, a taste:

```tablegen
class Tagged {
  string MyName = NAME;
}
def Alpha : Tagged;     // MyName = "Alpha"
def Beta  : Tagged;     // MyName = "Beta"
```

### Try it yourself
- Write a class `GPR<int n>` that auto-builds `string AsmName = "x" # n;` (paste operator — covered in Lesson 7, just copy this for now).
- Instantiate `X0` through `X3`.

---

## Lesson 5 — The `let` Statement

`let` overrides field values. It comes in three flavors.

### Flavor A — inside a record body
```tablegen
class Inst { bit hasSideFx = 0; int Latency = 1; }

def LOAD : Inst {
  let hasSideFx = 1;
  let Latency = 4;
}
```

### Flavor B — top-level `let ... in { ... }` block
This applies bindings to **every** record defined inside the block.

```tablegen
class Inst { bit hasSideFx = 0; int Latency = 1; }

let hasSideFx = 1, Latency = 8 in {
  def LOAD  : Inst;
  def STORE : Inst;
}
```
Both `LOAD` and `STORE` end up with `hasSideFx = 1` and `Latency = 8`.

> **Important:** Top-level `let` only overrides *inherited* fields. A field defined *directly* in the record body is **not** overridden by an outer `let`.

### Flavor C — `let append` / `let prepend`, Not working!
Concatenate instead of replace. Works for `list<T>`, `string`/`code`, and `dag`.

```tablegen
// TableGen has no `let append`/`let prepend`. Use !listconcat — and because
// `let items = f(items)` self-references the field being assigned, the clean
// idiom is to pass the extras as class template parameters.
// class Base   { list<int> items = [2, 3]; }
// class Middle : Base   { let append  items = [4]; }   // -> [2, 3, 4]
// def   Final  : Middle { let prepend items = [1]; }   // -> [1, 2, 3, 4]

class Base { list<int> items = [2, 3]; }
class WithExtras<list<int> pre, list<int> post> {
  list<int> items = !listconcat(pre, !listconcat([2, 3], post));
}

def Appended  : WithExtras<[],  [4]>;         // -> [2, 3, 4]
def Prepended : WithExtras<[1], []>;          // -> [1, 2, 3]
def Both      : WithExtras<[1], [4]>;         // -> [1, 2, 3, 4]
def Override  : Base { let items = [10, 20, 30]; }   // plain override
```

### Setting individual bits
```tablegen
class Inst { bits<8> Opcode = 0; }

def ADD : Inst {
  let Opcode{7-4} = 0b0010;   // upper nibble
  let Opcode{3-0} = 0b1100;   // lower nibble
}
```

### Try it yourself
- Use a top-level `let isCall = true in { ... }` to mark a group of "instruction" records as calls.
- Use `let append` to grow a `list<string> Predicates` across an inheritance chain.

---

## Lesson 6 — Values, Expressions, and Bang Operators

A **bang operator** is a built-in function whose name starts with `!`. They turn TableGen from a glorified config language into a real metaprogramming tool.

### The core arithmetic & logic ones
```tablegen
def Math {
  int   Sum    = !add(1, 2, 3, 4);        // 10
  int   Diff   = !sub(10, 3);             // 7
  int   Prod   = !mul(2, 3, 4);           // 24
  int   Quot   = !div(20, 6);             // 3   (signed integer division)
  int   Shl    = !shl(1, 4);              // 16
  bit   And    = !and(1, 1, 0);           // 0
  bit   Or     = !or(0, 0, 1);            // 1
  bit   Not    = !not(0);                 // 1
  bit   Eq     = !eq(3, 3);               // 1
  bit   Lt     = !lt(2, 5);               // 1
  int   Lg2    = !logtwo(16);             // 4
}
```

### Strings
```tablegen
def Strs {
  string Hi    = !strconcat("Hel", "lo");        // "Hello"
  int    Len   = !size("Hello");                 // 5
  int    Pos   = !find("Hello world", "world");  // 6
  string Sub   = !substr("Hello world", 6, 5);   // "world"
  string Up    = !toupper("abc");                // "ABC"
  string Down  = !tolower("ABC");                // "abc"
  string Rep   = !repr([1, 2, 3]);               // "[1, 2, 3]" (debug only)
}
```

### Lists
```tablegen
def Ls {
  list<int> A     = !listconcat([1,2], [3,4]);     // [1,2,3,4]
  list<int> Spl   = !listsplat(7, 3);              // [7,7,7]
  list<int> Rng   = !range(0, 5);                  // [0,1,2,3,4]
  list<int> Rng2  = !range(10, 0, -2);             // [10,8,6,4,2]
  int       Sz    = !size([10, 20, 30]);           // 3
  int       Hd    = !head([10, 20, 30]);           // 10
  list<int> Tl    = !tail([10, 20, 30]);           // [20, 30]
  list<int> Flat  = !listflatten([[1,2],[3,4]]);   // [1,2,3,4]
  string    Join  = !interleave([1,2,3], ", ");    // "1, 2, 3"
}
```

### Conditionals — `!if` and `!cond`
```tablegen
class Sign<int n> {
  // !if(test, then, else)
  string S1 = !if(!lt(n, 0), "neg", "non-neg");

  // !cond(c1: v1, c2: v2, ..., true: default)   -- evaluated in order
  string S2 = !cond(!lt(n, 0): "negative",
                    !eq(n, 0): "zero",
                    true     : "positive");
}

def A : Sign<-5>;   // S1="neg",     S2="negative"
def B : Sign<0>;    // S1="non-neg", S2="zero"
def C : Sign<7>;    // S1="non-neg", S2="positive"
```

### List comprehension — `!foreach`, `!filter`, `!foldl`
```tablegen
def Comp {
  list<int> Doubled = !foreach(x, [1,2,3,4], !mul(x, 2));   // [2,4,6,8]
  list<int> Evens   = !filter(x, [1,2,3,4,5,6], !eq(!and(x,1), 0));  // [2,4,6]
  int       Total   = !foldl(0, [1,2,3,4,5], acc, x, !add(acc, x)); // 15
}
```

### Casting — `!cast`
```tablegen
class Marker;
def  M : Marker;

def Casting {
  string  AsStr = !cast<string>(M);           // "M"      (record -> name)
  int     I     = !cast<int>(0b1010);         // 10       (bits -> int)
  // Looking up a record by name:
  Marker  Ref   = !cast<Marker>("M");         // -> M
}
```

> `!cast<T>("Name")` is a *late-bound lookup*: the record named "Name" must exist (eventually) and inherit from T.

### Type assertions — `!isa`, `!exists`, `!initialized`
```tablegen
class Animal;
def  Dog : Animal;

def Checks {
  bit IsAn   = !isa<Animal>(Dog);           // 1
  bit Exists = !exists<Animal>("Dog");      // 1
  bit Init   = !initialized(?);             // 0
}
```

### Try it yourself
- Use `!foldl` to compute the maximum of a list of ints.
- Use `!filter` to extract odd numbers from `!range(0, 10)`.
- Use `!cond` to map an integer 0–6 to weekday names.

---

## Lesson 7 — The Paste Operator `#`

`#` is the **only infix operator** in TableGen. It glues things together — strings, lists, or *identifier-like name fragments*.

### Rule 1 — In a `def`/`defm` *name*, `#` builds a string
```tablegen
foreach i = 0...3 in
  def R#i;            // produces R0, R1, R2, R3
```
The right-hand side `i` is the **loop variable** (a real value here, `0` through `3`), and `#` concatenates it as text into the record name.

### Rule 2 — In *other* expressions: LHS evaluated, RHS may be verbatim
This is the trickiest part of TableGen syntax. If the RHS of `#` is:
- a **defined** local value → it's evaluated normally;
- an **undefined identifier** or a **global** name → it's used as a literal string.

```tablegen
defvar suffix = "_TAIL";

def Demo {
  // suffix is global -> taken VERBATIM as the string "suffix" on the RHS.
  string A = "x" # suffix;        // "xsuffix"   (probably surprising!)

  // To get the VALUE of suffix on the right, force normal evaluation:
  string B = "x" # !cast<string>(suffix); // "x_TAIL"
}
```

> **Rule of thumb:** when in doubt, wrap the right operand in a bang operator (`!cast<string>`, `!strconcat`, etc.) to force evaluation.

### Rule 3 — Trailing `#` means "concat to empty string"
```tablegen
defvar n = 42;
def D { string S = n#; }    // S = "42"
```

### Pasting lists
```tablegen
def L { list<int> X = [1,2,3] # [4,5]; }   // [1,2,3,4,5]
```

### Try it yourself
1. Define a `class Reg<int n>` whose `string AsmName` is computed as `"x" # n`.
2. Inside a `foreach i = 0...7`, generate `X0` through `X7`.

---

## Lesson 8 — `multiclass` and `defm`

A `multiclass` is a **macro that defines multiple records at once**. You define it with `multiclass` and *invoke* it with `defm`.

### Why it exists
Consider a 3-address ISA where every arithmetic op has both a `reg, reg, reg` form and a `reg, reg, imm` form. Without multiclasses you'd write twice as many `def`s. With a multiclass you write each pattern once.

### Code — `08_multiclass.td`
```tablegen
def GPR;
def Imm;
class Inst<int opc, string asm, dag operands> {
  int Opcode = opc;
  string Asm = asm;
  dag Operands = operands;
}

multiclass ArithRI<int opc, string asm> {
  def _rr : Inst<opc, !strconcat(asm, " $d, $s1, $s2"),
                 (ops GPR:$d, GPR:$s1, GPR:$s2)>;
  def _ri : Inst<opc, !strconcat(asm, " $d, $s1, $s2"),
                 (ops GPR:$d, GPR:$s1, Imm:$s2)>;
}

defm ADD : ArithRI<0b111, "add">;
defm SUB : ArithRI<0b101, "sub">;
defm MUL : ArithRI<0b100, "mul">;
```

You'll need a stub class for `ops`; for an isolated demo just add `def ops;` at the top.

### What records are produced
```
ADD_rr, ADD_ri
SUB_rr, SUB_ri
MUL_rr, MUL_ri
```

The trick: a `def Foo` *inside* a multiclass is equivalent to `def NAME # Foo`, where `NAME` becomes whatever the outer `defm` is named. So `defm ADD` + `def _rr` → `ADD_rr`.

### Multiclasses can call other multiclasses
```tablegen
class Inst<int opc, string n> { int Opcode = opc; string Name = n; }

multiclass Basic<int opc> {
  def rr : Inst<opc, "rr">;
  def rm : Inst<opc, "rm">;
}

multiclass Scalar<int opc> {
  defm SS : Basic<opc>;       // -> NAME_SS_rr, NAME_SS_rm
  defm SD : Basic<opc>;       // -> NAME_SD_rr, NAME_SD_rm
  def  X  : Inst<opc, "x">;   // -> NAME_X
}

defm ADD : Scalar<0xF>;
// Records: ADD_SS_rr, ADD_SS_rm, ADD_SD_rr, ADD_SD_rm, ADD_X
```

### Combining multiclasses in one `defm`
You can inherit several multiclasses (and even regular classes after them) in a single `defm`:
```tablegen
class Predicated { bit IsPredicated = 1; }

defm CMP : Scalar<0xA>, Predicated;
// All five CMP records get IsPredicated = 1.
```
> A `defm`'s parent list must list all multiclasses **before** any plain classes.

### Try it yourself
- Write a multiclass `ShiftOps<int opc>` that produces three records: `_l` (left), `_r` (right logical), `_ra` (right arithmetic), all inheriting from your `Inst` class with the same opcode.
- Invoke it with `defm SH : ShiftOps<0x3>;`.

---

## Lesson 9 — `defvar`, `defset`, `deftype`

### `defvar` — a named, immutable variable
```tablegen
defvar BaseOpc = 0x20;
defvar Names   = ["add", "sub", "mul"];

class Op<int idx> {
  int Opc      = !add(BaseOpc, idx);
  string Asm   = Names[idx];
}

def OP0 : Op<0>;       // Opc = 0x20, Asm = "add"
def OP1 : Op<1>;       // Opc = 0x21, Asm = "sub"
def OP2 : Op<2>;       // Opc = 0x22, Asm = "mul"
```
A `defvar` *cannot be reassigned* once defined. Inside a `foreach`, a `defvar` only lives for one iteration.

### `defset` — collect records into a global list
```tablegen
class Reg<int n> { int Num = n; }

defset list<Reg> AllRegs = {
  def R0 : Reg<0>;
  def R1 : Reg<1>;
  def R2 : Reg<2>;
}

// Now you can do:
def Counter {
  int NumRegs = !size(AllRegs);     // 3
}
```
- Records inside the braces are still defined globally as usual.
- They are *also* appended to the named list (`AllRegs` here).
- Anonymous records produced by inline `ClassID<...>` are **not** added.

### `deftype` — alias for a type
```tablegen
deftype byte = bits<8>;
deftype halfword = bits<16>;

def Insn { byte Opcode = 0xAB; halfword Imm = 0xBEEF; }
```
Only allowed at top level, and only for primitive types / aliases.

### Try it yourself
- Define a `defset list<Reg> Callee = { ... }` of callee-saved registers and another `defset list<Reg> Caller = { ... }` of caller-saved ones.
- Use `!listconcat` and `!size` to produce a summary record.

---

## Lesson 10 — Control Flow

### `foreach`
```tablegen
class Reg<int n> { int Num = n; }

foreach i = 0...7 in
  def R#i : Reg<i>;
// Produces R0..R7

foreach name = ["sp", "lr", "pc"] in
  def !toupper(name) : Reg<0>;
// Hint: the iterator can be a list of any type.
```
Nested `foreach`:
```tablegen
foreach b = 0...1 in
  foreach n = 0...3 in
    def B#b#_R#n;
// B0_R0, B0_R1, ... B1_R3
```

### `if ... then ... else`
Works at top level, in record bodies, and in multiclasses.
```tablegen
class Reg<int n> {
  int  Num   = n;
  bit  IsLow = !lt(n, 16);
}

foreach i = 0...31 in {
  if !lt(i, 16) then
    def R#i : Reg<i> { let IsLow = 1; }
  else
    def R#i : Reg<i> { let IsLow = 0; }
}
```

### `assert`
Checks an invariant. Non-fatal at top level / on record completion.
```tablegen
class Person<string name, int age> {
  assert !le(!size(name), 32), "name too long: " # name;
  assert !and(!ge(age, 0), !le(age, 130)), "bad age: " # age;
  string Name = name;
  int    Age  = age;
}

def Knuth : Person<"Donald Knuth", 86>;     // OK
// def Bad : Person<"X", 999>;              // would print a note
```
- In a **class**: assertions are inherited and checked on each record.
- In a **multiclass**: checked at each `defm` instantiation.

### `dump` — debug print to stderr
```tablegen
multiclass MC<dag d> {
  dump "received dag = " # !repr(d);
  def : Inst<...>;
}
```
Useful while iterating; remove before committing.

### Try it yourself
- Use `foreach` + `if` to generate 32 registers, where the first 8 have `IsArg = 1`, the rest `IsArg = 0`.
- Add an `assert` that rejects names longer than 4 characters.

---

## Lesson 11 — DAGs

A `dag` value is a tree node with an **operator** and zero or more **arguments**, each of which can itself be a `dag`.

### Syntax
```
(operator  arg1, arg2, ...)
```
Each `arg` can be:
- `value` — just a value;
- `value:$name` — value with a name tag;
- `$name` — name only, value is `?`.

The operator **must be a record**.

### Code — `11_dag.td`
```tablegen
def set;
def add;
def GR32;

class Reg;
def EAX : Reg;
def EBX : Reg;

def Pattern {
  // (set EAX:$dst, (add EBX:$src1, 5))
  dag P = (set EAX:$dst, (add EBX:$src1, 5));
}
```

### DAG-manipulating bang operators
| Operator | Effect |
|---|---|
| `!getdagop(d)` | the operator record |
| `!getdagarg<T>(d, k)` | argument by index or name |
| `!getdagname(d, i)` | the `$name` of argument i |
| `!setdagop(d, op)` | new DAG with operator replaced |
| `!setdagarg(d, k, v)` | new DAG with argument k replaced |
| `!setdagname(d, k, n)` | new DAG with name k replaced |
| `!con(d1, d2, ...)` | concatenate DAGs (operators must match) |
| `!dag(op, args, names)` | construct DAG from pieces |
| `!size(d)` | number of arguments |
| `!empty(d)` | 1 iff no arguments |
| `!foreach(v, d, expr)` | map over arguments |

### Example
```tablegen
def op;

def Demo {
  dag D1 = (op 1:$a, 2:$b);
  dag D2 = (op 3:$c);
  dag D3 = !con(D1, D2);     // (op 1:$a, 2:$b, 3:$c)
  int N  = !size(D3);         // 3
}
```

### Try it yourself
- Build a DAG representing `(add r1, (mul r2, r3))` using suitable stub records.
- Use `!getdagarg<int>` to extract a numbered argument.

---

## Lesson 12 — Classes as Subroutines

Because `ClassName<args>` builds an *anonymous record* and you can immediately access its fields, classes work as ad-hoc **functions** that return multiple values.

### Code — `12_subroutine.td`
```tablegen
class IsPow2<int n> {
  bit ret = !and(!ne(n, 0), !eq(!and(n, !sub(n, 1)), 0));
}

class IsValidSize<int sz> {
  bit ret = !cond(!eq(sz,  1): 1,
                  !eq(sz,  2): 1,
                  !eq(sz,  4): 1,
                  !eq(sz,  8): 1,
                  !eq(sz, 16): 1,
                  true        : 0);
}

def Data1 {
  int Size = 8;
  bit IsPow2     = IsPow2<Size>.ret;       // 1
  bit IsValidSz  = IsValidSize<Size>.ret;  // 1
}
```

You can return multiple "values" by having more than one named field:
```tablegen
class DivMod<int a, int b> {
  int q = !div(a, b);
  int r = !sub(a, !mul(!div(a, b), b));
}

def QR { int Q = DivMod<23, 5>.q; int R = DivMod<23, 5>.r; }   // Q=4, R=3
```

> Each call creates a fresh anonymous record. That's fine — they're cheap.

### Try it yourself
- Write a "subroutine" class `Clamp<int v, int lo, int hi>` whose `out` field is the clamped value.
- Compose: write `ClampPow2<int v>` which clamps and then checks `IsPow2`.

---

## Lesson 13 — Preprocessing

TableGen ships with a tiny preprocessor — three directives only.

| Directive | Purpose |
|---|---|
| `#define MACRO` | Define a macro (no value, just defined-ness). |
| `#ifdef MACRO` | Compile-in if macro defined. |
| `#ifndef MACRO` | Compile-in if macro *not* defined. |
| `#else` | Else branch. |
| `#endif` | Close the region. |

### Code
```tablegen
#define HAS_FP

def Common { int X = 1; }

#ifdef HAS_FP
  def FloatUnit { string Name = "fpu"; }
#else
  def NoFPU { string Note = "no FPU"; }
#endif

#ifndef HAS_VECTOR
  def Scalar;
#endif
```

### Defining macros from the command line
```bash
llvm-tblgen --print-records 13_preproc.td -DHAS_VECTOR
```

### Includes
```tablegen
include "Registers.td"
include "Instructions.td"
```
Lookup honors `-I <dir>` flags. The included file is lexically substituted.

---

## Lesson 14 — Capstone: A Mini Toy ISA

Let's combine everything into a small but realistic example: a 4-register, 8-instruction toy ISA called **MiniISA**.

### Code — `MiniISA.td`
```tablegen
//===-- MiniISA.td - A toy ISA description in TableGen ---------*- tablegen -*-===//

// ---------- 1. Registers ---------------------------------------------------
class Register<string n, bits<2> num> {
  string AsmName = n;
  bits<2> Encoding = num;
  bit CalleeSaved = 0;
}

defset list<Register> AllRegs = {
  def R0 : Register<"r0", 0b00>;
  def R1 : Register<"r1", 0b01>;
  def R2 : Register<"r2", 0b10> { let CalleeSaved = 1; }
  def R3 : Register<"r3", 0b11> { let CalleeSaved = 1; }
}

// ---------- 2. Operand classes ---------------------------------------------
class Operand;
def REG  : Operand;
def IMM  : Operand;

// ---------- 3. Instruction skeleton ----------------------------------------
class Inst<bits<4> opc, string mnem, dag outs, dag ins> {
  bits<4>     Opcode  = opc;
  string      Mnemonic = mnem;
  dag         OutOps  = outs;
  dag         InOps   = ins;
  list<string> Predicates = [];
  bit         HasSideFx = 0;
  bit         IsBranch  = 0;
  bit         IsCall    = 0;
}

def ops;     // placeholder operator for operand DAGs

// ---------- 4. A multiclass for RR / RI shape ------------------------------
multiclass ArithOp<bits<4> opc, string mnem> {
  def _rr : Inst<opc, mnem,
                 (ops REG:$dst),
                 (ops REG:$s1, REG:$s2)>;
  def _ri : Inst<opc, mnem,
                 (ops REG:$dst),
                 (ops REG:$s1, IMM:$s2)>;
}

// ---------- 5. Define every arithmetic op in one block ---------------------
let HasSideFx = 0 in {
  defm ADD : ArithOp<0b0001, "add">;
  defm SUB : ArithOp<0b0010, "sub">;
  defm AND : ArithOp<0b0011, "and">;
  defm OR  : ArithOp<0b0100, "or">;
  defm XOR : ArithOp<0b0101, "xor">;
}

// ---------- 6. Memory + control flow ---------------------------------------
def LOAD  : Inst<0b1000, "ld",  (ops REG:$dst), (ops REG:$addr)> { let HasSideFx = 1; }
def STORE : Inst<0b1001, "st",  (ops),           (ops REG:$src, REG:$addr)> { let HasSideFx = 1; }
def JMP   : Inst<0b1110, "jmp", (ops),           (ops IMM:$tgt)>  { let IsBranch = 1; }
def CALL  : Inst<0b1111, "call",(ops),           (ops IMM:$tgt)>  { let IsCall   = 1; let IsBranch = 1; }

// ---------- 7. A sanity assertion ------------------------------------------
foreach R = AllRegs in
  assert !le(!size(R.AsmName), 4), "register name too long: " # R.AsmName;

// ---------- 8. A summary record using bang operators ------------------------
def Summary {
  int    NumRegs        = !size(AllRegs);
  int    NumCalleeSaved = !size(!filter(R, AllRegs, R.CalleeSaved));
  string CalleeList     = !interleave(
                            !foreach(R, !filter(R, AllRegs, R.CalleeSaved), R.AsmName),
                            ", ");
}
```

### Run it
```bash
llvm-tblgen --print-records MiniISA.td
```

Look at the `Summary` record in the output — `NumRegs=4`, `NumCalleeSaved=2`, `CalleeList="r2, r3"` — all computed at TableGen time.

### What this exercises
- Classes & inheritance (Lesson 2)
- `bits<N>`, `dag`, `list` types (Lesson 3)
- Template arguments (Lesson 4)
- Top-level `let ... in` (Lesson 5)
- Bang operators (Lesson 6)
- `defm` + `multiclass` (Lesson 8)
- `defset` (Lesson 9)
- `foreach` + `assert` (Lesson 10)
- DAGs (Lesson 11)

---

## Appendix — Bang-Operator Cheat Sheet

| Operator | Description |
|---|---|
| `!add`, `!sub`, `!mul`, `!div` | Arithmetic on ints. |
| `!and`, `!or`, `!xor`, `!not` | Bitwise / logical. |
| `!shl`, `!srl`, `!sra` | Shift left, shift right logical, shift right arithmetic. |
| `!eq`, `!ne`, `!lt`, `!le`, `!gt`, `!ge` | Comparisons. |
| `!if(c, a, b)` | Conditional expression. |
| `!cond(c1:v1, c2:v2, ..., true:vd)` | Multi-way conditional. |
| `!cast<T>(x)` | Type cast or record-by-name lookup. |
| `!isa<T>(x)` | Type test. |
| `!exists<T>(name)` | Does a record by that name & type exist? |
| `!initialized(x)` | Is `x` not `?` ? |
| `!size`, `!head`, `!tail`, `!empty` | List/string/DAG size & access. |
| `!listconcat`, `!listflatten`, `!listsplat`, `!listremove`, `!range` | List construction. |
| `!foreach(v, seq, expr)` | Map over a list or dag. |
| `!filter(v, list, pred)` | Filter a list. |
| `!foldl(init, list, acc, v, expr)` | Left fold. |
| `!strconcat`, `!substr`, `!find`, `!interleave`, `!toupper`, `!tolower` | String ops. |
| `!logtwo` | Integer floor log₂. |
| `!repr` | Debug stringification. |
| `!con`, `!dag`, `!getdagop`, `!getdagarg`, `!getdagname`, `!setdagop`, `!setdagarg`, `!setdagname`, `!getdagopname`, `!setdagopname` | DAG manipulation. |
| `!match` | Regex match on strings. |
| `!instances<T>([regex])` | Enumerate records of type T (with optional regex filter). |
| `!subst(target, repl, value)` | Substitute target with repl in a string or record name. |

---

## Where to go next

- **TableGen Backends** — `llvm/docs/TableGen/BackEnds.html` — what each official backend consumes.
- **TableGen Backend Developer's Guide** — how to write your own backend that walks the records and emits text.
- **`llvm/lib/Target/<Target>/*.td`** — real-world examples (start with AArch64 or RISCV).

> **Pro tip.** When something doesn't behave the way you expect, add a `dump !repr(...)` in a multiclass or `--print-detailed-records` on the command line. TableGen's metaprogramming surface is small but unusual, and printing the actual record state is almost always faster than reasoning about it.

Happy TableGenning!