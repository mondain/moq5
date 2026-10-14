# CMake compiler ABI probes

On Apple Clang, CMake's Apple platform module can set combined verbosity to
`-v -Wl,-v`. Using that combination for compilation produces an unused linker
argument warning. The pre-project registrar in `cmake/MoqAppleAbiProbe.cmake`
selects a per-language rules override. After platform initialization, the rules
set only an undefined `CMAKE_<LANG>_VERBOSE_COMPILE_FLAG` to `-v`, and only for
Apple Clang/Clang with that exact combined flag. Link verbosity is unchanged.
The compile-specific override is supported by the minimum CMake 3.20.

Generic and per-language caller hooks, including explicitly empty hooks, remain
caller-owned. Defined compile verbosity is never replaced. Other platforms and
compiler families are unchanged. There is no warning suppression or cache force
in the correction, and delayed C++ initialization re-enters the rules module.

A selected toolchain (variable or environment) owns hook selection: the root
does not register defaults before that toolchain runs. A toolchain that wants
the correction can include `cmake/MoqAppleAbiProbe.cmake` after choosing its own
hooks; existing hooks still win. Alternatively its existing rules hook can set
the compile-specific verbosity itself. This avoids preempting toolchain
undefined-only defaults; uncorrected caller-owned configurations still fail the
warning gate. This is not a waiver or a universal repair of custom toolchains.

With `add_subdirectory`, registration is local to the library directory; it
cannot repair languages already enabled by the embedding project. An embedding
caller can include the registrar before its own `project()` when it owns hook
selection. No parent or sibling variables are set by the registrar.

`ConsumerBuildContext.cmake` serializes the selected toolchain and generic/C/C++
hooks as undefined-only `CACHE STRING` entries without `FORCE` in test-owned
preloads. Cache entries survive `-C` processing; normal preload variables do not.
Existing normal or cache hook values, including empty values, are preserved. The
production registrar and rules continue to use only normal variables. Full consumer
context retains its existing compile/link flag serialization. Raw, WT and service tests
use the narrow preload for independent provider and CMake consumer configs.
These paths are not installed or exported. Arbitrary installed consumers must
configure their own probes before `find_package`; package exports cannot fix
an earlier compiler initialization.

The preload carries a selected producer toolchain/hook bundle. A different child
toolchain is rejected, including when the producer had no toolchain. Validation
runs before preload defaults (checking an environment selection when no variable
is defined), and again through a generated test-owned
`CMAKE_PROJECT_INCLUDE_BEFORE` module after CLI processing. Thus a toolchain `-D`
after `-C` cannot silently mix bundles. An explicitly empty toolchain is accepted
only for a no-toolchain bundle. Relative toolchain paths use CMake's build-then-
source lookup and are compared as real paths. This is not toolchain negotiation.

An existing project-entry hook, even explicitly empty, causes a clear collision
error unless it is the same generated validator. It is never replaced or chained.
The test invocation must retain that validator; replacing the validation hook
itself after `-C` is not supported. Neither validator nor preload is installed.

Unresolved rules module names are located without execution, using the builtin-
first lookup of CMake's compiler information modules and the producer's
`CMAKE_MODULE_PATH`. Relative file hooks are anchored to the producer project
source directory. Hooks needing another directory's relative lookup must be
selected by absolute path. Unresolvable selections fail rather than being
silently reinterpreted in a child. Defined-empty hooks remain empty.

## Focused verification

Run only when the runtime owner releases a window. The script test is registered
as `cmake_apple_abi_rules`; it checks platform/compiler guards, empty and custom
values in normal/cache bindings, hook preservation, escaping, and both context
serializers. Its `include()` checks do not replace actual `-C` child gates.
`cmake_consumer_preload_argv` checks all enabled raw/WT/service shell registrations
against their required preload argument counts and generated files. Run it in a
configuration enabling those consumers; disabled rows are not coverage.

The registered `cmake_consumer_abi_bundle` test uses the parent generator and
actual fresh `-C` subprocesses with LANGUAGES NONE. Each invocation retains a
distinct child directory, so repeated CTest runs do not reuse caches. Direct
script invocation defaults to Ninja:

```sh
cmake -DTEST_OUTPUT="$out/bundle-ownership" \
  -P "$src/tests/cmake/ConsumerAbiBundleTest.cmake"
```

It covers no-toolchain, distinct earlier/later CLI and environment selections,
equal selections, explicit empty values, and project-hook collision. Expected
failures must match their dedicated diagnostic; these are not ABI execution rows.

For real compiler discovery, use fresh output directories and the fixture below
(set `src` to this checkout and `out` to a new absolute output directory):

```sh
fixture="$src/tests/cmake/apple-abi-probe"
cmake -S "$fixture" -B "$out/fresh" -DMODE=fresh
cmake -S "$fixture" -B "$out/caller" -DMODE=caller
cmake -S "$fixture" -B "$out/embed" -DMODE=embed
cmake -S "$fixture" -B "$out/toolchain" -DMODE=toolchain \
  -DCMAKE_TOOLCHAIN_FILE="$fixture/Toolchain.cmake"
cmake -C "$out/fresh/full-context.cmake" -S "$fixture" \
  -B "$out/full-child" -DMODE=child
cmake -C "$out/fresh/abi-context.cmake" -S "$fixture" \
  -B "$out/abi-child" -DMODE=child
cmake -C "$out/toolchain/full-context.cmake" -S "$fixture" \
  -B "$out/toolchain-child" -DMODE=child
cmake -S "$src" -B "$out/root" -DMOQ_BUILD_TESTS=OFF \
  -DMOQ_BUILD_SIM=OFF -DMOQ_BUILD_EXAMPLES=OFF
cmake -S "$fixture" -B "$out/child-local" -DMODE=child-local
cmake -S "$fixture" -B "$out/opt-in" -DMODE=fresh \
  -DCMAKE_TOOLCHAIN_FILE="$fixture/OptInToolchain.cmake"
for mode in named relative; do
  cmake -S "$fixture" -B "$out/$mode" -DMODE="$mode"
  cmake -C "$out/$mode/delayed-context.cmake" -S "$fixture" \
    -B "$out/$mode-child" -DMODE=child -DEXPECT_DELAYED=ON
done
```

The ordinary fixture modes enable fresh C and delayed C++, assert ABI discovery,
and write `abi-evidence.txt`. The standalone root row tests actual registration;
the child-local row enables CXX in the library directory and checks parent/sibling
hooks remain unchanged. Those two rows retain evidence in CMake's compiler files
and logs. The named/relative rows serialize before enabling CXX and assert that
the delayed hook was not executed early. Capture all output and inspect every log
(`CMakeConfigureLog.yaml`, or CMakeOutput/CMakeError logs on older CMake) for
warnings/errors. For affected Apple compilers, verify the actual ABI compile
lines use `-v` without `-Wl,-v`, while ABI link lines retain both flags. Compare
pointer size, byte order, compiler identity, and implicit include/link discovery
to an existing same-compiler/platform baseline. Empty implicit C libraries can
be valid; do not invent a nonempty requirement. The fixtures do not rebuild
dependencies or execute consumers; actual raw/WT installed-consumer tests remain
separate integration coverage.
