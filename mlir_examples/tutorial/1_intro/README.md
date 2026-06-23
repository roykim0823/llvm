# 1 — Introduction: From LLVM IR to MLIR

This chapter walks the **complete compilation pipeline** twice, on two small
programs, so you can see exactly what each tool does and what it produces:

- [`1_llvm_modules/`](1_llvm_modules/) — a minimal program written in **raw LLVM
  IR**, compiled to a shared library and called from Python.
- [`2_mlir/`](2_mlir/) — the same kind of program, but written in **high-level
  MLIR** (a loop in the `scf` dialect) and lowered all the way down to native
  code.

Together they answer: *what does MLIR buy us over plain LLVM IR?*

> All commands assume `llvm@20` is on your `PATH`:
> `export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"`. Outputs shown below were
> produced with **Homebrew LLVM 20.1.8** on Apple Silicon — your hex addresses
> and exact assembly may differ.

---

## Part A — `1_llvm_modules/`: raw LLVM IR

### The source: [`simple.ll`](1_llvm_modules/simple.ll)

```llvm
define i32 @main() {
	ret i32 42
}
```

This is hand-written **LLVM IR**. It is equivalent to the C program:

```c
int main() {
  return 42;
}
```

`define i32 @main()` declares a function named `main` returning a 32-bit
integer; `ret i32 42` returns the constant `42`.

### The build: [`build.sh`](1_llvm_modules/build.sh)

```bash
mkdir -p build
llc -filetype=obj --relocation-model=pic simple.ll -o ./build/simple.o   # 1
clang -shared -fPIC ./build/simple.o -o ./build/libsimple.so             # 2
clang ./build/simple.o -o ./build/simple   # optional executable          # 3
./build/simple; echo $?                                                  # 4
python3 simple.py                                                        # 5
```

**Step 1 — `llc`: LLVM IR → object code.** `llc` is LLVM's static compiler. It
turns the textual IR into a native object file (`.o`). `--relocation-model=pic`
emits *position-independent code*, which is required for a shared library.

**Step 2 — `clang -shared`: object → shared library.** Links the object into
`libsimple.so` (a `.dylib`-style shared object on macOS) that other programs —
including Python — can load at runtime.

**Step 3 — `clang`: object → executable.** Optionally links the same object into
a standalone runnable binary `simple`.

**Step 4 — run it.** The program does nothing but return `42`, which becomes the
process **exit code**. `echo $?` prints the exit code of the last command:

```
42
```

**Step 5 — call it from Python.** [`simple.py`](1_llvm_modules/simple.py) uses
the standard-library `ctypes` module to load the shared library and call
`main()` directly:

```python
import ctypes

# Load the shared library (the .so we just built) into the process.
module = ctypes.CDLL("./build/libsimple.so")

# Declare the C signature of `main` so ctypes marshals values correctly:
module.main.argtypes = []            # main takes no arguments
module.main.restype = ctypes.c_int   # main returns a C int (i32)

# Call the native function and print its return value.
print(module.main())
```

Line by line:

- `ctypes.CDLL(path)` `dlopen`s the shared library and returns a handle whose
  attributes are its exported symbols (`module.main` is the C `main`).
- `argtypes` / `restype` tell ctypes how to convert Python values to/from C.
  Without `restype`, ctypes assumes the function returns a C `int` — correct
  here, but setting it explicitly is good practice and essential once return
  types aren't `int`.
- `module.main()` actually invokes the compiled native code and returns `42` as
  a Python `int`.

Output:

```
42
```

> Unlike Step 4, the value `42` here is a **return value** we print, not a
> process exit code.

This is the punchline of Part A: **anything you compile through LLVM becomes a
plain shared library you can call from Python** — the bridge into the ML
ecosystem we'll rely on throughout the series.

---

## Part B — `2_mlir/`: high-level MLIR

Returning `42` is trivial in LLVM IR. But real programs have loops, and writing
a loop in raw LLVM IR means manually managing basic blocks and **phi nodes**
(the SSA construct that merges values across control-flow paths). MLIR lets us
write the loop at a high level and *lower* it for us.

### The source: [`example.mlir`](2_mlir/example.mlir)

```mlir
// loop_add: sum the integers 0..9 using a structured (scf) for-loop.
// Returns `index` (a platform-sized integer, like size_t).
func.func @loop_add() -> (index) {
  // `index` dialect: loop bounds and the running total are index-typed.
  %init = index.constant 0   // initial accumulator value
  %lb = index.constant 0     // loop lower bound (inclusive)
  %ub = index.constant 10    // loop upper bound (exclusive)
  %step = index.constant 1   // loop step

  // scf.for is a *structured* loop. `iter_args` threads a loop-carried value
  // (%acc) through each iteration; whatever we scf.yield becomes %acc next
  // time, and the final value is bound to %sum.
  %sum = scf.for %iv = %lb to %ub step %step iter_args(%acc = %init) -> (index) {
    %sum_next = arith.addi %acc, %iv : index   // arith dialect: acc + iv
    scf.yield %sum_next : index                // carry sum_next into next iter
  }
  return %sum : index
}

// main: call loop_add and return the result as a C-style i32 exit code.
func.func @main() -> i32 {
  %out = call @loop_add() : () -> index
  // index is i64 here; narrow it to i32 so main can return a normal exit code.
  %out_i32 = arith.index_cast %out : index to i32
  return %out_i32 : i32
}
```

In C this is:

```c
int loop_add() {
  int sum = 0;
  for (int iv = 0; iv < 10; iv += 1)
    sum = sum + iv;
  return sum;          // 0+1+2+...+9 = 45
}
int main() { return loop_add(); }
```

The single file touches **four dialects** — `func` (functions & `call`), `index`
(loop bounds / induction var), `scf` (the structured `scf.for` loop), and `arith`
(`addi`, `index_cast`). See the comments above for what each line does.

> **Result preview:** the loop sums `0..9`, so the program returns **45**, not
> 42. (Different from Part A on purpose.)

### The build: [`build.sh`](2_mlir/build.sh)

The script runs the full pipeline. We'll go through it stage by stage.

#### Step 1 — `mlir-opt`: lower high-level dialects to the `llvm` dialect

```bash
mlir-opt example.mlir \
  --convert-func-to-llvm \
  --convert-math-to-llvm \
  --convert-index-to-llvm \
  --convert-scf-to-cf \
  --convert-cf-to-llvm \
  --convert-arith-to-llvm \
  --reconcile-unrealized-casts \
  -o ./build/example_opt.mlir
```

Each `--convert-X-to-Y` flag is a **pass** that rewrites one dialect into a
lower one. The order matters: `convert-scf-to-cf` turns the structured loop into
unstructured branches (`cf` dialect), and **only then** can `convert-cf-to-llvm`
turn those branches into the `llvm` dialect. `reconcile-unrealized-casts` cleans
up the temporary cast ops the conversions insert.

The result, `build/example_opt.mlir`, is entirely in the `llvm` dialect — note
how the `scf.for` loop has become explicit basic blocks and branches:

```mlir
module {
  llvm.func @loop_add() -> i64 {
    %0 = llvm.mlir.constant(0 : i64) : i64
    %1 = llvm.mlir.constant(0 : i64) : i64
    %2 = llvm.mlir.constant(10 : i64) : i64
    %3 = llvm.mlir.constant(1 : i64) : i64
    llvm.br ^bb1(%1, %0 : i64, i64)
  ^bb1(%4: i64, %5: i64):  // 2 preds: ^bb0, ^bb2
    %6 = llvm.icmp "slt" %4, %2 : i64        // %4 < 10 ?
    llvm.cond_br %6, ^bb2, ^bb3
  ^bb2:  // pred: ^bb1
    %7 = llvm.add %5, %4 : i64               // sum += iv
    %8 = llvm.add %4, %3 : i64               // iv  += 1
    llvm.br ^bb1(%8, %7 : i64, i64)
  ^bb3:  // pred: ^bb1
    llvm.return %5 : i64
  }
  llvm.func @main() -> i32 {
    %0 = llvm.call @loop_add() : () -> i64
    %1 = llvm.trunc %0 : i64 to i32          // index (i64) -> i32
    llvm.return %1 : i32
  }
}
```

The loop's carried value (`iter_args`) became the **block arguments** of `^bb1`
(`%4` = induction var, `%5` = accumulator) — MLIR's alternative to phi nodes.

#### Step 2 — `mlir-translate`: `llvm` dialect → LLVM IR

```bash
mlir-translate ./build/example_opt.mlir -mlir-to-llvmir -o ./build/example.ll
```

This crosses out of MLIR into textual **LLVM IR**. Notice that here the loop is
expressed with real **phi nodes** (`%2`, `%3`) — exactly the bookkeeping MLIR
saved us from writing by hand:

```llvm
define i64 @loop_add() {
  br label %1
1:                                       ; preds = %5, %0
  %2 = phi i64 [ %7, %5 ], [ 0, %0 ]     ; induction variable
  %3 = phi i64 [ %6, %5 ], [ 0, %0 ]     ; accumulator
  %4 = icmp slt i64 %2, 10
  br i1 %4, label %5, label %8
5:                                       ; preds = %1
  %6 = add i64 %3, %2
  %7 = add i64 %2, 1
  br label %1
8:                                       ; preds = %1
  ret i64 %3
}

define i32 @main() {
  %1 = call i64 @loop_add()
  %2 = trunc i64 %1 to i32
  ret i32 %2
}
```

#### Step 3 — `llc` + `clang`: LLVM IR → object → shared library + executable

```bash
llc -filetype=obj --relocation-model=pic ./build/example.ll -o ./build/example.o
clang -shared -fPIC ./build/example.o -o ./build/libexample.so   # shared library
clang ./build/example.o -o ./build/example                       # executable
./build/example; echo $?
```

Exactly the same final stages as Part A. `llc` produces the native object, then
`clang` links it two ways:

- `-shared` → `libexample.so`, loadable from Python with `ctypes` just like
  `simple.py` (`ctypes.CDLL("./build/libexample.so").main()` returns `45`).
- no `-shared` → a standalone executable `example`. Running it makes `main`'s
  return value the **process exit code**:

```
45
```

This is the binary/executable equivalent of Part A's `simple` — the high-level
MLIR loop has become an ordinary native program.

#### Step 4 — inspect the native assembly

```bash
llc -filetype=asm --relocation-model=pic ./build/example.ll -o ./build/example.s
```

On Apple Silicon this emits ARM64. The optimizer recognized the loop and the
`loop_add` body compiles to a tight inner loop:

```asm
_loop_add:                              ; @loop_add
	mov	x8, xzr            ; iv  = 0
	mov	x0, xzr            ; sum = 0
	cmp	x8, #9
	b.gt	LBB0_2
LBB0_1:                                 ; inner loop, depth 1
	add	x0, x0, x8         ; sum += iv
	add	x8, x8, #1         ; iv  += 1
	cmp	x8, #9
	b.le	LBB0_1
LBB0_2:
	ret                        ; return sum (in x0)
```

The `build.sh` also disassembles the object/dylib with `objdump` if you want to
see resolved addresses:

```bash
objdump -d --no-show-raw-insn ./build/example.o    > ./build/example.dis
objdump -d --no-show-raw-insn ./build/libexample.so > ./build/libexample.dis
```

#### Step 5 — (alternative) `mlir-runner`: JIT-execute directly

Everything above goes through full code generation to a native binary. For quick
iteration you can instead **skip codegen entirely** and JIT-run the lowered MLIR:

```bash
mlir-runner -e main -entry-point-result=i32 ./build/example_opt.mlir
```

`mlir-runner` JIT-compiles and runs the module in-process without producing any
files. `-e main` names the entry function and `-entry-point-result=i32` tells it
the return type. Output:

```
45
```

(The script also shows the variant with
`-shared-libs=/opt/homebrew/opt/llvm@20/lib/libmlir_runner_utils.dylib`, needed
only when your MLIR calls runtime helpers like `printMemref` — not required
here.)

This is a shortcut for *running* the code, not part of the build-to-native path:
it consumes `example_opt.mlir` (the output of Step 1), parallel to Steps 2–4
rather than after them.

---

## The big picture

| | `1_llvm_modules` | `2_mlir` |
| --- | --- | --- |
| You write | raw LLVM IR | high-level MLIR (`scf.for`) |
| Loops / phi nodes | manual | generated by lowering |
| Lowering tool | — (already LLVM IR) | `mlir-opt` passes |
| To LLVM IR | (it *is* LLVM IR) | `mlir-translate` |
| To native (object) | `llc` | `llc` |
| Shared library + executable | `clang` | `clang` |
| JIT option | — | `mlir-runner` |

Both programs end up as ordinary native code — a shared library *and* a
standalone executable — but MLIR let us express the loop at a human level and
mechanically lower it, with the phi-node bookkeeping handled for us. That
lowering machinery is the whole point of MLIR, and every later chapter adds
*higher* dialects (`memref`, `linalg`, `tensor`, `gpu`) on top of this same
pipeline.

---

## Run it yourself

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"

cd 1_llvm_modules && bash build.sh && cd ..   # exit code 42, then 42 (from Python)
cd 2_mlir         && bash build.sh && cd ..   # exit code 45 (executable), then 45 (JIT)
```

Inspect the generated files under each `build/` directory:
`example_opt.mlir` (lowered MLIR), `example.ll` (LLVM IR), `example` (executable),
`libexample.so` (shared library), `example.s` (assembly), and `*.dis`
(disassembly).

**Next:** [`../2_memory/`](../2_memory/) — how MLIR represents memory with the
`tensor` and `memref` dialects.
