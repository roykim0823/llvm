# 6 — E-graphs & Term Rewriting

### The optimization that happens *before* lowering

Back in the [top-level pipeline](../README.md#the-modern-compiler-pipeline) there
was a step we skipped past: *"the core language is optimized by core-to-core
transformations, optionally using e-graphs."* This chapter builds that step. It's
a different kind of optimization from everything so far — it happens at the
**high level**, on the math itself, *before* anything is lowered to `linalg` or
`llvm`.

Why bother, when LLVM and MLIR already optimize? Because they don't know your
*algebra*. LLVM will not turn `transpose(transpose(A))` into `A`, or
`e^x · e^x · e^x` into `e^{3x}`, or cancel the 2s in `(a*2)/2` — those are
domain identities, and by the time the code is loops over memory the structure
needed to spot them is gone. Applying them up front, on the expression tree, is
"low-hanging fruit" that can delete whole matrix operations before codegen even
starts.

The obstacle is the **phase-ordering problem**: rewrites interfere. Given
`x*2 → x<<1` and `(x*y)/y → x`, the expression `(a*2)/2` *should* become `a` — but
if you greedily apply the first rule you get the stuck term `(a<<1)/2` and miss the
cancellation. Which rule, in which order? **Equality saturation** sidesteps the
question entirely.

This is not a solved problem you're re-implementing for sport — it's a live
frontier. Every rewriter that LLVM and MLIR ship today (`InstCombine`, the
greedy pattern rewriter, PDL patterns) is *destructive* and therefore
phase-ordered; **no upstream dialect or pass does equality saturation**. Yet one
production compiler (Cranelift, the WebAssembly backend) has already rebuilt its
entire mid-end on e-graphs, and two active research lines are racing to bring
them to MLIR. This chapter builds the technique from scratch and ends the way
those projects do — extract, emit IR, compile; the
[closing section](#where-e-graphs-stand-in-real-compilers-2026) maps what you
built here onto that landscape.

> Based on Stephen Diehl's *"MLIR Part 6 — Specializing Python with E-graphs"*
> ([`../reference/`](../reference/)). That part uses the `egglog` library (whose
> API churns); to keep the chapter self-contained and runnable, we build a small
> e-graph engine from scratch — then emit MLIR from the optimized expression and
> execute it, exactly as the PDF's pipeline does.

---

## How an e-graph works

An **e-graph** (equality graph) stores **many equivalent expressions at once**.
Rather than rewriting destructively (and losing the original), it records that two
forms are *equal* and keeps both:

- An **e-class** is a set of expressions known to be equivalent.
- An **e-node** is one operator applied to e-classes (`mul(class3, class7)`),
  hash-consed so shared subexpressions are stored once.
- A rewrite `lhs → rhs` doesn't replace anything; it **adds** `rhs` and **merges**
  its e-class with the one that matched `lhs`.

```text
   destructive rewrite            e-graph: keep both, mark them equal
   (a*2)/2  ──►  a                 ┌─ e-class ───────────────┐
   (original is gone)              │  (a*2)/2   ≡   a<<1/2    │  all equal;
                                   │      ≡        ≡   a      │  extraction
                                   └─────────────────────────┘  picks cheapest
```

That "keep both" is the whole trick: applying `x*2 → x<<1` no longer *destroys*
the `(a*2)/2` form, so `(x*y)/y → x` can still fire on it. The stuck-term problem
disappears because nothing ever gets stuck — every form coexists.

The algorithm is three phases ([`egraph.py`](egraph.py)):

1. **Saturate** — apply *every* rule everywhere, repeatedly, until no new
   equivalence appears. Order no longer matters: all reachable forms coexist.
2. **Rebuild** — restore congruence (if `a = b`, then `f(a)` and `f(b)` must share
   an e-class too).
3. **Extract** — walk the graph picking, for each e-class, the lowest-cost e-node
   according to a **cost model**, and read out the cheapest equivalent term.

A rewrite therefore never deletes; it only ever `merge`s the matched class with a
freshly-added right-hand side:

*egraph.py* (a rewrite adds `rhs` and merges — never replaces)
```python
  def merge(self, a, b):
    """Declare two e-classes equivalent."""
    a, b = self.find(a), self.find(b)
    if a == b:
      return a
    self.parent[b] = a
    self.nodes[a] |= self.nodes[b]
    self.nodes[b] = set()
    return a
```

The cost model is just a number per operation — `exp` is dear, `+` is cheap — so
extraction naturally prefers `e^{3x}` (one `exp`) over `e^x·e^x·e^x` (three).

---

## What's here

| File | Role |
| --- | --- |
| [`egraph.py`](egraph.py) | the engine: `add` / `merge` / `rebuild` / `ematch` / `saturate` / `extract` |
| [`optimize_demo.py`](optimize_demo.py) | the three concept demos below (pure Python) |
| [`to_mlir.py`](to_mlir.py) | emit a `func.func` of `arith` ops from an optimized term |
| [`capstone.py`](capstone.py) | optimize an expression, emit MLIR for original & optimized |
| [`aot_main.py`](aot_main.py) | run both compiled functions, confirm they agree |
| [`build.sh`](build.sh) | run the demos, then compile + execute the capstone |

---

## The concept demos (`optimize_demo.py`)

Each builds an expression, saturates a small rule set, and extracts the cheapest
form:

**Run:** `python3 optimize_demo.py`

```
Equality saturation — extracting the cheapest equivalent form:
  (a*2)/2           ->  a
  e^x * e^x * e^x   ->  exp(((x + x) + x))
  (A^T)^T           ->  A
All e-graph demos succeeded.
```

The first is the phase-ordering example: the e-graph holds `a*2`, `a<<1`, *and*
the cancelled `a` simultaneously, so extraction simply reads off `a` — no rule
ordering required. The second shows the **cost model** doing the deciding (with
`exp` priced at 40 and `+` at 1, the single-`exp` form `exp((x+x)+x)` wins over
three exps). The third is the kind of tensor identity that lets you delete an
operation `linalg` would otherwise dutifully compute.

---

## Capstone: optimize → emit MLIR → execute (`build.sh`) · ✅

This closes the loop from expression to running code — the toy compiler the PDF
builds, in miniature. We optimize

```
f(a) = ((a*2)/2) * ((a*2)/2)      # 5 ops: 2 mul + 2 div + 1 mul
```

The rule `(x*y)/y → x` cancels each half to `a`, so the e-graph extracts

```
f(a) = a * a                      # 1 op
```

`capstone.py` emits MLIR for **both** forms via `to_mlir.py` (free symbols become
`f32` arguments, the tree becomes `arith` ops). The contrast is the whole point —
five arithmetic ops become one. Before:

*build/orig.mlir* (the 5-op original)
```mlir
func.func @f(%a: f32) -> f32 {
  %v1 = arith.constant 2.000000e+00 : f32
  %v2 = arith.mulf %a, %v1 : f32
  %v3 = arith.constant 2.000000e+00 : f32
  %v4 = arith.divf %v2, %v3 : f32
  %v5 = arith.constant 2.000000e+00 : f32
  %v6 = arith.mulf %a, %v5 : f32
  %v7 = arith.constant 2.000000e+00 : f32
  %v8 = arith.divf %v6, %v7 : f32
  %v9 = arith.mulf %v4, %v8 : f32
  return %v9 : f32
}
```

after:

*build/opt.mlir* (the extracted 1-op form)
```mlir
func.func @f(%a: f32) -> f32 {
  %v1 = arith.mulf %a, %a : f32
  return %v1 : f32
}
```

`build.sh` compiles each to a shared library (the now-familiar
`mlir-opt → mlir-translate → llc → clang` pipeline), and `aot_main.py` calls both
over a range of inputs to confirm the rewrite **preserved meaning** while cutting
5 operations to 1.

**Run:** `cd 6_egraph && bash build.sh`

```
original :  (((a * 2) / 2) * ((a * 2) / 2))   (5 ops)
optimized:  (a * a)   (1 ops)
Wrote build/orig.mlir and build/opt.mlir
```
```
Capstone OK: optimized f(a) == original f(a) == a*a for all test inputs.
```

---

## Run everything

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"
cd 6_egraph && bash build.sh          # demos + compiled capstone (needs no extra deps)
```

## Key takeaways

- **E-graphs solve phase ordering** by keeping every equivalent form at once, then
  extracting the best — so rewrite order stops mattering.
- **Equality saturation** = apply all rules to a fixpoint (`saturate`);
  **extraction** = pick the cheapest form under a cost model (`extract`); a rewrite
  only ever `merge`s, never deletes.
- **This is high-level, domain-aware optimization** that LLVM/`linalg` can't do
  because they lack the algebraic identities — it runs on the core language
  *before* lowering.
- **It still ends in MLIR**: the optimized expression is emitted as `arith` ops
  and compiled, exactly the "core → MLIR → LLVM" path from Chapter 1.

---

## Where e-graphs stand in real compilers (2026)

A fair question after this chapter: *if equality saturation is this good, why
isn't it just a `mlir-opt` flag?* Status check, as of mid-2026:

**Upstream LLVM/MLIR: nothing.** MLIR documents 50-odd dialects and none is an
e-graph; all of its rewrite machinery (greedy pattern rewriter, PDL/PDLL, the
Transform dialect) applies patterns destructively, which is exactly the
phase-ordering trap this chapter opened with. LLVM's closest relative is
`NewGVN`, which builds congruence classes of equal values — the same
union-find machinery as our `merge`/`find` — but only to deduplicate, never to
saturate with algebraic rules or extract by cost.

**Production proof: Cranelift's ægraphs.** The Wasmtime/Cranelift compiler
replaced its whole machine-independent mid-end with an *acyclic e-graph*
("ægraph") in 2022 — rewrite rules written in a DSL (ISLE), GVN and LICM
falling out of the representation "for free", cost-based extraction at the end
([RFC](https://github.com/bytecodealliance/rfcs/blob/main/accepted/cranelift-egraph.md),
[EGRAPHS 2023 paper](https://pldi23.sigplan.org/details/egraphs-2023-papers/2/-graphs-Acyclic-E-graphs-for-Efficient-Optimization-in-a-Production-Compiler),
[design write-up](https://cfallin.org/blog/2026/04/09/aegraph/)). It's the
existence proof that e-graphs can carry a shipping compiler, and its
restriction to *acyclic* e-graphs with immediate rewriting is a deliberate
trade of completeness for predictable compile times.

**Bridging MLIR to an external engine: DialEgg.** [DialEgg (CGO
2025)](https://dl.acm.org/doi/10.1145/3696443.3708957), from McGill, translates
MLIR ops into facts for [egglog](https://github.com/egraphs-good/egglog) (the
successor to the `egg` Rust library), saturates there, extracts, and rebuilds
MLIR — dialect-agnostically, so `linalg`-level identities like our
`(Aᵀ)ᵀ → A` are in scope. Architecturally this is `capstone.py` grown up: the
e-graph lives outside the compiler, and translation in/out is the tax.

**Making the e-graph *be* the IR: the `eqsat` dialect.** The other line, from
Tobias Grosser's group, inverts that: represent the e-graph *natively in MLIR*
as an `eqsat.eclass` op whose operands are the equivalent alternatives —
first as a rewriting dialect ([EGRAPHS 2025](https://arxiv.org/abs/2505.09363),
prototyped in [xDSL](https://xdsl.dev/)), then generalized so the e-graph
*persists across the whole pass pipeline* instead of being discarded after one
optimization ([arXiv 2602.16707](https://arxiv.org/abs/2602.16707), Feb 2026).
No translation tax, and ordinary passes like CSE double as e-graph maintenance.

### Final thoughts

Two things make this chapter's toy honest. First, the architecture you just ran
— *lift a pure expression fragment, saturate outside the main IR, extract,
re-emit* — is not a pedagogical simplification; it is the state of practice,
the same shape as DialEgg and every egglog-based pipeline. The reason nothing
is upstream yet isn't doubt about the idea, it's the unsolved edges: real IR
has memory, side effects, and control flow that don't fit pure term graphs
(MLIR regions bind values — active research on "slotted" e-graphs), saturation
can blow up exponentially, and optimal extraction is NP-hard. Cranelift shipped
by *constraining* the e-graph; the MLIR efforts are still choosing between
bridging out (DialEgg) and building in (`eqsat`).

Second, the trend line is clear: destructive rewriting is increasingly seen as
the legacy design, and "equalities as first-class IR" as the destination. If
that lands, the skills from this chapter — thinking in e-classes, writing
rules instead of passes, pricing operations with a cost model — stop being a
side quest and become how mid-end optimization is simply *done*. Watch the
`eqsat` dialect: an e-graph that survives the entire pipeline would dissolve
the boundary between "optimize the algebra first" (this chapter) and "then
lower it" (the rest of this series).

**Next:** [`../7_transformer/`](../7_transformer/) — transformers.

