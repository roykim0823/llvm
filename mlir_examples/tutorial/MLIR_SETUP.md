# MLIR Part 0 — Installing MLIR

> Based on *"MLIR Part 0 - Installing MLIR"* by Stephen Diehl, with additional
> explanation, background, and macOS/Apple-Silicon-specific notes for this
> tutorial series.

Installing MLIR can be a royal pain. There is no single "MLIR installer" —
MLIR ships *inside* the LLVM monorepo, so installing MLIR really means getting
a build of LLVM that has the `mlir` subproject enabled. This document collects
the practical ways to do that.

If you just want to follow this tutorial on a Mac, jump straight to
[From Homebrew (macOS)](#from-homebrew-macos) — that is all you need.

---

## Table of Contents

1. [Background: what are we even installing?](#background-what-are-we-even-installing)
2. [Which method should I pick?](#which-method-should-i-pick)
3. [From Homebrew (macOS)](#from-homebrew-macos)
4. [Using Apt (Ubuntu 24.04)](#using-apt-ubuntu-2404)
5. [Using llvm.sh](#using-llvmsh)
6. [Precompiled Packages](#precompiled-packages)
7. [From Source (macOS)](#from-source-macos)
8. [From Source (Ubuntu 24.04)](#from-source-ubuntu-2404)
9. [From Wheel (pip)](#from-wheel-pip)
10. [From Wheel (Poetry)](#from-wheel-poetry)
11. [From Wheel (uv)](#from-wheel-uv)
12. [Using Conda](#using-conda)
13. [Using Docker](#using-docker)
14. [Installing MLIR Python Bindings](#installing-mlir-python-bindings)
15. [Verifying your installation](#verifying-your-installation)
16. [The tools you just installed](#the-tools-you-just-installed)
17. [Troubleshooting](#troubleshooting)

---

## Background: what are we even installing?

MLIR (Multi-Level Intermediate Representation) is a subproject of
[LLVM](https://llvm.org/). It is *not* distributed on its own — it lives in the
`mlir/` directory of the `llvm-project` monorepo and is built together with
LLVM. As a result, "installing MLIR" is really "getting an LLVM build with
`-DLLVM_ENABLE_PROJECTS=mlir`".

What you actually need for this tutorial is a handful of **command-line tools**
and, optionally, the **Python bindings**:

| Component            | What it is                                                        |
| -------------------- | ----------------------------------------------------------------- |
| `mlir-opt`           | The optimizer/transformer. Runs passes that lower one dialect to another. |
| `mlir-translate`     | Converts MLIR to/from other formats (notably LLVM IR).            |
| `mlir-runner`        | A JIT that executes MLIR directly (formerly `mlir-cpu-runner`).  |
| `mlir-transform-opt` | Driver for the Transform dialect.                                 |
| `llc`, `clang`       | LLVM's static compiler and C front-end — used to produce `.o`/`.so`/`.s` from LLVM IR. |
| MLIR Python bindings | `import mlir` from Python; build IR programmatically.             |

> **Version note for this tutorial.** The examples here target **LLVM/MLIR 20**.
> MLIR has *no stable API or IR guarantee* — dialect names, pass flags, and
> tool names change between major releases (e.g. `mlir-cpu-runner` was renamed
> to `mlir-runner`). Pin to one major version and stick with it. The `build.sh`
> scripts in this repo assume `llvm@20` installed via Homebrew at
> `/opt/homebrew/opt/llvm@20`.

---

## Which method should I pick?

| Your situation | Recommended method |
| --- | --- |
| macOS, just want to follow the tutorial | **Homebrew** (`brew install llvm@20`) |
| Ubuntu/Debian, want pre-built tools | **Apt** or **llvm.sh** |
| You only work in Python | **pip / uv / conda wheels** |
| You need to *modify* MLIR or want the bleeding edge | **From source** |
| You want a reproducible, throwaway environment | **Docker** |

The trade-off is always **time/disk vs. control**. Pre-built packages get you
running in minutes; building from source costs an hour+ and tens of GB of disk
but lets you pick the exact revision and enable extra features (Python
bindings, integration tests, sanitizers).

---

## From Homebrew (macOS)

The simplest way to install MLIR on macOS. This installs a stable LLVM that
includes the MLIR tools and libraries.

```bash
brew install llvm@20
```

### Why `llvm@20` and not plain `llvm`?

Homebrew's `llvm` formula tracks the *latest* major release, which will move on
to 21, 22, … over time and silently break the pass flags used in this tutorial.
The versioned formula `llvm@20` pins you to LLVM 20.

### Homebrew is "keg-only" — you must fix your PATH

Homebrew does **not** symlink `llvm@20` tools into `/opt/homebrew/bin` by
default (it is "keg-only" to avoid clashing with Apple's own toolchain). After
installing, add it to your shell so `mlir-opt`, `mlir-runner`, etc. are found:

```bash
# Apple Silicon (M1/M2/M3...) — prefix is /opt/homebrew
echo 'export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc

# Intel Macs — prefix is /usr/local instead
# echo 'export PATH="/usr/local/opt/llvm@20/bin:$PATH"' >> ~/.zshrc
```

`brew info llvm@20` prints the exact path and the lines to add — when in doubt,
trust its output over this document.

### The runtime libraries you'll need

Several tutorial examples print from inside MLIR or call into C, which needs
MLIR's runtime support libraries. Homebrew installs them next to the tools:

```
/opt/homebrew/opt/llvm@20/lib/libmlir_runner_utils.dylib
/opt/homebrew/opt/llvm@20/lib/libmlir_c_runner_utils.dylib
```

This is why the `build.sh` scripts pass, e.g.:

```bash
mlir-runner -e main -entry-point-result=i32 \
  -shared-libs=/opt/homebrew/opt/llvm@20/lib/libmlir_runner_utils.dylib \
  ./build/example_opt.mlir
```

---

## Using Apt (Ubuntu 24.04)

The official LLVM APT repository provides pre-built MLIR packages with all
dependencies configured. Ubuntu 24.04's codename is **noble**, so the repo line
uses `noble`/`llvm-toolchain-noble-20` (on 22.04 it was `jammy`).

```bash
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
add-apt-repository "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main"
apt-get update
apt-get install -y libmlir-20-dev mlir-20-tools
```

> The tools land as version-suffixed binaries like `mlir-opt-20`. Either call
> them by that name or create unsuffixed symlinks / add them to PATH via
> `update-alternatives`.

---

## Using `llvm.sh`

LLVM ships a convenience script that sets up the APT repo and installs
LLVM/MLIR automatically — handy on Ubuntu/Debian.

```bash
bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"
```

To install a specific version:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 20
```

---

## Precompiled Packages

The LLVM project publishes official pre-compiled release archives. Useful when
you need a specific version or want to skip compilation. They are best-effort
and may not match your distribution's glibc.

1. Download the appropriate package from the
   [LLVM releases page](https://github.com/llvm/llvm-project/releases). Look for
   files named like `mlir-<version>.src.tar.xz` (e.g. `mlir-20.1.4.src.tar.xz`).
2. Extract it:
   ```bash
   tar xf mlir-<version>.src.tar.xz
   ```
3. Add the binaries to your PATH, either by moving the directory to a system
   location:
   ```bash
   sudo mv mlir-<version>.src /usr/local/mlir
   echo 'export PATH=/usr/local/mlir/bin:$PATH' >> ~/.bashrc
   ```
   or by pointing PATH at where it already is:
   ```bash
   echo 'export PATH=/path/to/mlir-<version>.src/bin:$PATH' >> ~/.bashrc
   ```
4. Verify:
   ```bash
   mlir-opt --version
   ```

> **Note:** Precompiled packages save build time but may omit optimizations
> available in a custom source build.

---

## From Source (macOS)

Building from source gives you the most control and the latest features, at the
cost of time and disk space (expect ~30–60+ minutes and tens of GB).

```bash
brew install cmake ccache ninja
git clone https://github.com/llvm/llvm-project.git
mkdir llvm-project/build
cd llvm-project/build
cmake -G Ninja ../llvm \
  -DLLVM_ENABLE_PROJECTS=mlir \
  -DLLVM_BUILD_EXAMPLES=ON \
  -DLLVM_TARGETS_TO_BUILD="Native" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DLLVM_CCACHE_BUILD=ON
cmake --build . -t mlir-opt mlir-translate mlir-transform-opt mlir-runner
cmake --build . -t install
```

### What the important flags mean

| Flag | Why |
| --- | --- |
| `-G Ninja` | Use Ninja instead of Make — dramatically faster incremental builds. |
| `-DLLVM_ENABLE_PROJECTS=mlir` | The flag that actually enables MLIR. Without it you get plain LLVM. |
| `-DLLVM_TARGETS_TO_BUILD="Native"` | Only build the code generator for *your* CPU. Building all targets is much slower and unnecessary here. For the later GPU/ARM chapters you may want extra targets, e.g. `"Native;ARM;X86"`. |
| `-DCMAKE_BUILD_TYPE=Release` | Optimized build. `Debug` is far larger/slower; `RelWithDebInfo` is a middle ground. |
| `-DLLVM_ENABLE_ASSERTIONS=ON` | Keep MLIR's internal assertions in a Release build — invaluable for catching malformed IR while you learn. |
| `-DLLVM_CCACHE_BUILD=ON` | Cache compiled objects so rebuilds after a `git pull` are fast. Requires `ccache`. |

> The first `cmake --build` builds only the named tools (faster than building
> everything). `cmake --build . -t install` then installs them.

> **Verify the build (optional).** To run MLIR's own test suite and confirm the
> build is healthy before installing, build the `check-mlir` target:
> ```bash
> cmake --build . --target check-mlir
> ```

---

## From Source (Ubuntu 24.04)

Recommended for developers who need to modify MLIR or guarantee specific
optimizations. The commands below are unchanged from 22.04 — building from
source doesn't depend on the Ubuntu release codename, only on having `clang`,
`lld`, `cmake`, and `ninja` available.

```bash
sudo apt-get install clang lld
git clone https://github.com/llvm/llvm-project.git
mkdir llvm-project/build
cd llvm-project/build
cmake -G Ninja ../llvm \
  -DLLVM_ENABLE_PROJECTS=mlir \
  -DLLVM_BUILD_EXAMPLES=ON \
  -DLLVM_TARGETS_TO_BUILD="Native" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DLLVM_CCACHE_BUILD=ON
cmake --build . -t mlir-opt mlir-translate mlir-transform-opt mlir-runner
cmake --build . -t install
```

---

## From Wheel (pip)

For Python users, pre-built wheels are available through a custom repository —
the easiest way to get MLIR into a Python project.

```bash
pip install mlir -f https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest
```

> These wheels are community-maintained by Maks Levental, not by the LLVM
> project. They bundle the same `mlir-opt`/`mlir-runner` binaries plus the
> Python bindings.

---

## From Wheel (Poetry)

```toml
[tool.poetry.dependencies]
python = "^3.12"
mlir = { version = "latest", source = "mlir-wheels" }

[[tool.poetry.source]]
name = "mlir-wheels"
url = "https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest"
priority = "supplemental"
```

Check it works:

```bash
poetry run mlir-opt --version
```

---

## From Wheel (uv)

`uv` is a fast, modern Python package manager.

```bash
uv add mlir --index mlir=https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest
```

Or declaratively in `pyproject.toml`:

```toml
[project]
dependencies = ["mlir"]

[tool.uv.sources]
mlir = { index = "mlir-wheels" }

[[tool.uv.index]]
name = "mlir-wheels"
url = "https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest"
```

Check it works:

```bash
uv run mlir-opt --version
```

---

## Using Conda

Conda can install both MLIR and the Python bindings from conda-forge:

```bash
conda install conda-forge::mlir
```

---

## Using Docker

For a containerized, reproducible environment, Stephen Diehl maintains pre-built
images at <https://github.com/sdiehl/docker-mlir-cuda>, in CUDA and non-CUDA
variants for Ubuntu 22.04 and 24.04.

```bash
# Ubuntu 24.04 — with CUDA
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir20-cuda-ubuntu24.04
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir19-cuda-ubuntu24.04
# Ubuntu 24.04 — without CUDA
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir20-ubuntu24.04
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir19-ubuntu24.04

# Ubuntu 22.04 — with CUDA
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir20-cuda-ubuntu22.04
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir19-cuda-ubuntu22.04
# Ubuntu 22.04 — without CUDA
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir20-ubuntu22.04
docker pull ghcr.io/sdiehl/docker-mlir-cuda:mlir19-ubuntu22.04
```

Run one interactively:

```bash
docker run -it ghcr.io/sdiehl/docker-mlir-cuda:mlir20-cuda-ubuntu24.04 bash
```

Or use it as a base in your own Dockerfile:

```dockerfile
FROM ghcr.io/sdiehl/docker-mlir-cuda:mlir20-cuda-ubuntu24.04
```

> **macOS note:** GPU/CUDA passthrough does not work in Docker on a Mac. The
> CUDA images are only useful on a Linux host with an NVIDIA GPU; on macOS use
> the non-CUDA tags (and they run under emulation/virtualization).

---

## Installing MLIR Python Bindings

The Python bindings are a separate package. Once the `mlir-wheels` index is set
up (above), install them with your package manager of choice:

```bash
pip install mlir-python-bindings -f https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest
poetry add mlir-python-bindings
uv add mlir --index mlir=https://github.com/makslevental/mlir-wheels/releases/expanded_assets/latest
conda install conda-forge::mlir-python-bindings
```

If none of those work for your platform, you must build them inside the LLVM
source tree with `-DMLIR_ENABLE_BINDINGS_PYTHON=On`.

On Ubuntu, install the build dependencies first:

```bash
sudo apt-get install -y \
  bash-completion ca-certificates ccache clang cmake cmake-curses-gui \
  git lld man-db ninja-build pybind11-dev python3 python3-numpy \
  python3-pybind11 python3-yaml unzip wget xz-utils
```

Build [nanobind](https://github.com/wjakob/nanobind) (the bindings' C++↔Python
glue) and install it locally:

```bash
git clone https://github.com/wjakob/nanobind && \
  cd nanobind && \
  git submodule update --init --recursive && \
  cmake \
    -G Ninja \
    -B build \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_INSTALL_PREFIX=$HOME/usr && \
  cmake --build build --target install
```

Then configure and build LLVM/MLIR with the bindings enabled:

```bash
cmake llvm \
  -G Ninja \
  -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_PREFIX_PATH=$HOME/usr \
  -DLLVM_BUILD_EXAMPLES=On \
  -DLLVM_TARGETS_TO_BUILD="Native" \
  -DLLVM_CCACHE_BUILD=On \
  -DLLVM_CCACHE_DIR=$HOME/ccache \
  -DLLVM_ENABLE_ASSERTIONS=On \
  -DLLVM_ENABLE_LLD=On \
  -DLLVM_ENABLE_PROJECTS="mlir;clang;clang-tools-extra" \
  -DLLVM_USE_SPLIT_DWARF=On \
  -DMLIR_ENABLE_BINDINGS_PYTHON=On \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DMLIR_INCLUDE_INTEGRATION_TESTS=On
cmake --build build -t mlir-opt mlir-translate mlir-transform-opt mlir-runner
cmake --build build -t install
```

> Change `-DPython3_EXECUTABLE` to point at the Python you actually use
> (`which python3`). Using the wrong interpreter is the #1 cause of
> `import mlir` failing later.

The bindings are built to:

```
build/tools/mlir/python_packages/mlir_core/mlir
```

Package them into a wheel:

```bash
cd build/tools/mlir/python_packages/mlir_core
python setup.py bdist_wheel
pip install dist/mlir_core-*.whl
```

…or add them to `PYTHONPATH` manually (adjust `$HOME` to your build location):

```bash
export PYTHONPATH=$PYTHONPATH:$HOME/build/tools/mlir/python_packages/mlir_core
```

---

## Verifying your installation

Whichever method you used, confirm the core tools are on your PATH:

```bash
mlir-opt --version
mlir-translate --version
mlir-runner --version    # older builds: mlir-cpu-runner
which mlir-opt           # should point inside your llvm@20 / install dir
```

A quick end-to-end smoke test — write a trivial module and JIT it:

```bash
cat > /tmp/smoke.mlir <<'EOF'
func.func @main() -> i32 {
  %0 = arith.constant 42 : i32
  return %0 : i32
}
EOF

mlir-opt /tmp/smoke.mlir \
  --convert-func-to-llvm \
  --convert-arith-to-llvm \
  --reconcile-unrealized-casts | \
mlir-runner -e main -entry-point-result=i32
# Expected program exit/result: 42
```

If that runs, you're ready for the rest of the tutorial.

For the Python bindings:

```bash
python -c "import mlir; from mlir.ir import Context; print('mlir bindings OK')"
```

---

## The tools you just installed

A short map of the pipeline used throughout this tutorial (see any
`build.sh` in this repo for concrete invocations):

```
example.mlir
   │  mlir-opt        (run lowering passes: func/arith/scf/... → llvm dialect)
   ▼
example_opt.mlir
   │  mlir-translate  (--mlir-to-llvmir)
   ▼
example.ll  (LLVM IR)
   │  llc             (LLVM IR → object code / assembly)
   ▼
example.o / example.s
   │  clang           (link into a shared object)
   ▼
libexample.so

   ── or, skipping codegen, run it directly with ──►  mlir-runner (JIT)
```

- **`mlir-opt`** is the workhorse: it applies *passes*. Most of your time
  learning MLIR is spent figuring out which passes lower your dialects down to
  the `llvm` dialect.
- **`mlir-runner`** JITs and executes MLIR immediately — great for quick
  iteration. Pass `-shared-libs=.../libmlir_runner_utils.dylib` when your code
  uses MLIR's runtime helpers (e.g. `printMemref`).
- **`mlir-translate`** crosses the boundary out of MLIR into textual LLVM IR.
- **`llc` / `clang`** are plain LLVM tools that take it the rest of the way to a
  native object or shared library (the AOT path).

---

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `command not found: mlir-opt` | PATH not updated. Re-check the Homebrew `export PATH=...` line and `source` your shell rc. |
| `mlir-cpu-runner: command not found` | Renamed to `mlir-runner` in recent LLVM. Use the new name (or vice-versa on older installs). |
| Unknown pass, e.g. `--convert-cf-to-llvm` not recognized | Version mismatch. Pass flags differ across LLVM majors — make sure you're on the version the examples target (20). |
| `library not found` / `dlopen` error from `mlir-runner` | Wrong path to `libmlir_runner_utils`. On Apple Silicon it's `/opt/homebrew/opt/llvm@20/lib/...`; on Intel `/usr/local/opt/...`. |
| `import mlir` fails in Python | Bindings built against a different Python than the one you're running, or `PYTHONPATH` not set. Rebuild with the right `-DPython3_EXECUTABLE`. |
| Source build runs out of disk / RAM | Use `-DCMAKE_BUILD_TYPE=Release`, `-DLLVM_TARGETS_TO_BUILD="Native"`, and link with `lld` (`-DLLVM_ENABLE_LLD=On`) to cut memory during linking. |

---

*Next: MLIR Part 1 — Introduction to MLIR.*
