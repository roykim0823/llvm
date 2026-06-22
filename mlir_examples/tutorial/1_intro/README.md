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
`ctypes` to load the shared library and call `main()` directly:

```python
import ctypes
module = ctypes.CDLL("./build/libsimple.so")
module.main.argtypes = []
module.main.restype = ctypes.c_int
print(module.main())
```

Output:

```
42
```

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
func.func @loop_add() -> (index) {
  %init = index.constant 0
  %lb   = index.constant 0
  %ub   = index.constant 10
  %step = index.constant 1

  %sum = scf.for %iv = %lb to %ub step %step iter_args(%acc = %init) -> (index) {
    %sum_next = arith.addi %acc, %iv : index
    scf.yield %sum_next : index
  }
  return %sum : index
}

func.func @main() -> i32 {
  %out = call @loop_add() : () -> index
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

What to notice — this single file touches **four dialects**:

- **`func`** — `func.func` defines functions; `call` invokes one.
- **`index`** — `index.constant` creates platform-sized integers used for loop
  bounds and induction variables.
- **`scf`** — *structured control flow*. `scf.for ... iter_args(...)` is a loop
  that **carries a value** (`%acc`) across iterations and `scf.yield`s the
  updated value each time. This is the high-level construct we get for free.
- **`arith`** — `arith.addi` adds integers; `arith.index_cast` converts the
  `index` result to `i32` so `main` can return it as a normal exit code.

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

#### Step 2 — `mlir-runner`: JIT-execute directly

```bash
mlir-runner -e main -entry-point-result=i32 ./build/example_opt.mlir
```

`mlir-runner` JIT-compiles and runs the module without producing any files —
ideal for quick iteration. `-e main` names the entry function and
`-entry-point-result=i32` tells it the return type. Output:

```
45
```

(The script also shows the variant with
`-shared-libs=/opt/homebrew/opt/llvm@20/lib/libmlir_runner_utils.dylib`, needed
only when your MLIR calls runtime helpers like `printMemref` — not required
here.)

#### Step 3 — `mlir-translate`: `llvm` dialect → LLVM IR

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

#### Step 4 — `llc` + `clang`: LLVM IR → object → shared library

```bash
llc -filetype=obj --relocation-model=pic ./build/example.ll -o ./build/example.o
clang -shared -fPIC ./build/example.o -o ./build/libexample.so
```

Same final stage as Part A: a native object, then a shared library. From here
you could call `loop_add`/`main` from Python with `ctypes`, exactly as in
`simple.py`.

#### Step 5 — inspect the native assembly

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

---

## The big picture

| | `1_llvm_modules` | `2_mlir` |
| --- | --- | --- |
| You write | raw LLVM IR | high-level MLIR (`scf.for`) |
| Loops / phi nodes | manual | generated by lowering |
| Lowering tool | — (already LLVM IR) | `mlir-opt` passes |
| To LLVM IR | (it *is* LLVM IR) | `mlir-translate` |
| To native | `llc` → `clang` | `llc` → `clang` |
| JIT option | — | `mlir-runner` |

Both programs end up as ordinary native code in a shared library — but MLIR let
us express the loop at a human level and mechanically lower it, with the phi-node
bookkeeping handled for us. That lowering machinery is the whole point of MLIR,
and every later chapter adds *higher* dialects (`memref`, `linalg`, `tensor`,
`gpu`) on top of this same pipeline.

---

## Run it yourself

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"

cd 1_llvm_modules && bash build.sh && cd ..   # prints 42, then 42 (from Python)
cd 2_mlir         && bash build.sh && cd ..   # prints 45 (mlir-runner), builds artifacts
```

Inspect the generated files under each `build/` directory:
`example_opt.mlir` (lowered MLIR), `example.ll` (LLVM IR), `example.s`
(assembly), and `*.dis` (disassembly).

**Next:** [`../2_memory/`](../2_memory/) — how MLIR represents memory with the
`tensor` and `memref` dialects.
