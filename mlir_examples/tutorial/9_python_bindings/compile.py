#!/usr/bin/env python3
"""A mini MLIR-to-PTX compiler, driven entirely from Python.

This is Chapter 8's lowering pipeline rebuilt as a *library*: instead of
chaining mlir-opt invocations in a shell script, we parse the module, run the
passes, and extract the kernel in-process through the MLIR Python bindings
(mlir.ir + mlir.passmanager). Only the final ISA emission shells out, to
mlir-translate (LLVM dialect -> LLVM IR) and llc (LLVM IR -> PTX).

Usage:
    python3 compile.py [square.mlir] [--chip sm_80] [-v]

Needs the MLIR Python bindings (see README.md) plus mlir-translate and llc on
PATH (override with $MLIR_TRANSLATE / $LLC). No GPU required — PTX is text.
"""

import argparse
import os
import shutil
import subprocess
import sys

try:
    from mlir.ir import Context, Module
    from mlir.passmanager import PassManager
except ImportError:
    sys.exit(
        "error: MLIR Python bindings not found (see README.md, 'Installing the bindings'):\n"
        "  uv add mlir-python-bindings \\\n"
        "    --find-links https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest"
    )


def gpu_pipeline(chip: str) -> list:
    """The lowering pipeline, one entry per pass.

    Each string is exactly what you would pass to mlir-opt on the command
    line — PassManager.add() and mlir-opt flags share the same registry.
    """
    return [
        "canonicalize",
        # tensors -> memrefs (Ch 2)
        "one-shot-bufferize{bufferize-function-boundaries"
        " function-boundary-type-conversion=identity-layout-map}",
        "canonicalize",
        # linalg -> scf.parallel loop nest (Ch 3/4)
        "convert-linalg-to-parallel-loops",
        # scf.parallel -> gpu.launch -> outlined gpu.func (Ch 8)
        "func.func(gpu-map-parallel-loops)",
        "convert-parallel-loops-to-gpu",
        "gpu-kernel-outlining",
        "lower-affine",
        "expand-strided-metadata",
        "normalize-memrefs",
        # device kernel -> nvvm, pin the target chip, host side -> llvm
        "gpu.module(convert-gpu-to-nvvm{index-bitwidth=0 use-bare-ptr-memref-call-conv})",
        f"nvvm-attach-target{{chip={chip} features=+ptx80 O=3}}",
        "convert-nvvm-to-llvm",
        "reconcile-unrealized-casts",
        "gpu-to-llvm{use-bare-pointers-for-host use-bare-pointers-for-kernels}",
    ]


def apply_gpu_pipeline(module: Module, chip: str, verbose: bool = False) -> Module:
    """Run the GPU compilation pipeline on the module, in place."""
    pm = PassManager()
    if verbose:
        pm.enable_ir_printing(print_after_change=True)
    for p in gpu_pipeline(chip):
        pm.add(p)
    pm.run(module.operation)
    return module


def extract_gpu_module(module: Module) -> Module:
    """Pull the device kernel out of the lowered host+device module.

    After the pipeline, the top-level module holds the host-side llvm.func and
    one gpu.module wrapping the kernel. mlir-translate only understands plain
    llvm.func, so re-wrap the gpu.module's body as a standalone module.
    """
    for op in module.body.operations:
        if op.operation.name == "gpu.module":
            block = op.operation.regions[0].blocks[0]
            body = "\n".join(str(kernel) for kernel in block.operations)
            return Module.parse(f"module {{\n{body}\n}}")
    raise RuntimeError("no gpu.module found — did the pipeline run?")


def _tool(name: str, env_var: str, need_target: str = None) -> str:
    """Find an LLVM tool: $ENV override, then the version-matched binaries
    bundled with the `mlir` wheel (if installed), then PATH.

    The wheel-bundled tools matter: the bindings track LLVM trunk and print IR
    that an older mlir-translate (e.g. Homebrew llvm@20) may not parse. But a
    candidate is only usable if it has the backend we need — the wheel's llc,
    for instance, is built without NVPTX — hence the need_target probe.
    """
    if os.environ.get(env_var):
        return os.environ[env_var]

    def usable(path):
        if not need_target:
            return True
        probe = subprocess.run([path, "--version"], capture_output=True, text=True)
        return need_target in probe.stdout

    import mlir as _mlir_pkg
    candidates = [os.path.join(p, "bin", name) for p in _mlir_pkg.__path__]
    candidates = [c for c in candidates if os.path.exists(c)]
    on_path = shutil.which(name)
    if on_path:
        candidates.append(on_path)
    for c in candidates:
        if usable(c):
            return c
    sys.exit(f"error: no usable {name} found"
             + (f" with {need_target} support" if need_target else "")
             + f" (set ${env_var} to override)")


def _run(cmd: list, stdin_text: str, what: str) -> str:
    result = subprocess.run(cmd, input=stdin_text, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"error while {what}:\n{result.stderr}")
    return result.stdout


def generate_ptx(gpu_module_str: str, chip: str = "sm_80") -> str:
    """gpu.module (llvm dialect) -> LLVM IR -> PTX assembly."""
    llvm_ir = _run(
        [_tool("mlir-translate", "MLIR_TRANSLATE"), "--mlir-to-llvmir", "-"],
        gpu_module_str, "translating MLIR to LLVM IR",
    )
    return _run(
        [_tool("llc", "LLC", need_target="nvptx"), "-march=nvptx64",
         f"-mcpu={chip}", "-o", "-", "-"],
        llvm_ir, "compiling LLVM IR to PTX",
    )


def compile_mlir_to_ptx(mlir_module_str: str, chip: str = "sm_80",
                        verbose: bool = False):
    """Compile an MLIR module string to PTX. Returns (ptx, lowered_module_str)."""
    with Context():
        module = Module.parse(mlir_module_str)
        apply_gpu_pipeline(module, chip, verbose)
        gpu_module = extract_gpu_module(module)
        return generate_ptx(str(gpu_module), chip), str(module)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", nargs="?", default="square.mlir")
    ap.add_argument("--chip", default="sm_80",
                    help="target GPU architecture (sm_75 Turing, sm_80 Ampere, sm_90 Hopper)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the IR after every pass that changes it")
    args = ap.parse_args()

    ptx, lowered = compile_mlir_to_ptx(open(args.input).read(), args.chip, args.verbose)

    os.makedirs("build", exist_ok=True)
    with open("build/square_lowered.mlir", "w") as f:
        f.write(lowered)
    with open("build/kernel.ptx", "w") as f:
        f.write(ptx)

    print(ptx)
    print(f"// wrote build/square_lowered.mlir and build/kernel.ptx (target {args.chip})",
          file=sys.stderr)


if __name__ == "__main__":
    main()
