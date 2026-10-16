"""Installed SDK contract, with external consumers and no transport traffic."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--libdir", required=True)
    p.add_argument("--includedir", required=True)
    p.add_argument("--cmake", required=True)
    p.add_argument("--cc", required=True)
    p.add_argument("--cxx", required=True)
    p.add_argument("--runtime", choices=("0", "1"), required=True)
    p.add_argument("--shared", choices=("0", "1"), required=True)
    p.add_argument("--managed", choices=("0", "1"), default="0")
    # Whether the SDK installs the simulator: the runtime FETCH retrieval
    # scenario, which pairs sessions with it, is then required to pass.
    p.add_argument("--sim", choices=("0", "1"), required=True)
    p.add_argument("--msquic-dir", default="")
    p.add_argument("--context", type=Path)
    args = p.parse_args()
    args.source = args.source.resolve()
    args.build = args.build.resolve()
    context = ["-C", args.context] if args.context else []
    direct = json.loads(Path(str(args.context) + ".json").read_text()) if args.context else {
        "C": [], "CXX": [], "LINK": []}
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(("DYLD_", "LD_", "CMAKE_", "PKG_CONFIG_")):
            env.pop(key)
    for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "DESTDIR"):
        env.pop(key, None)

    def run(argv, *, ok=True):
        result = subprocess.run([str(x) for x in argv], env=env,
                                capture_output=True, text=True, timeout=120)
        if ok:
            sys.stdout.write(result.stdout)
            sys.stdout.flush()
            sys.stderr.write(result.stderr)
            sys.stderr.flush()
            require(result.returncode == 0,
                    f"{argv}\n{result.stdout}\n{result.stderr}")
        return result

    with tempfile.TemporaryDirectory(prefix="relay SDK ") as name:
        work = Path(name)
        prefix = work / "installed"
        run([args.cmake, "--install", args.build, "--prefix", prefix])
        relocated = work / "relocated prefix"
        prefix.rename(relocated)
        inc = relocated / args.includedir
        lib = relocated / args.libdir
        pkg = lib / "cmake/libmoq"
        expected_headers = {"export.h", "types.h", "auth.h", "capacity.h", "log.h",
                            "placement.h", "relay.h", "trace.h", "wire_codes.h"}
        components = ["relay-core"]
        if args.runtime == "1":
            expected_headers.update(("moqr_bind.h", "moqr_shards.h", "moqr_obs.h"))
            components.append("relay")
        require({f.name for f in (inc / "moq/relay").iterdir()} == expected_headers,
                "installed header set differs from public API")
        for header in (inc / "moq/relay").iterdir():
            text = header.read_text()
            require(not re.search(r"MOQR_\w*TESTING|moqr_\w*debug_", text),
                    f"test API in {header}")
            # Each header must stand alone, including in C++.
            for cc, lang in ((args.cc, "c"), (args.cxx, "c++")):
                unit = work / "header.c"
                unit.write_text(f"#include <moq/relay/{header.name}>\nint header_probe;\n")
                run([cc, "-x", lang, "-std=c11" if lang == "c" else "-std=c++11",
                     *direct["C" if lang == "c" else "CXX"],
                     "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only",
                     "-I", inc, unit])
        for f in relocated.rglob("*"):
            require(not any(x in f.name for x in
                            ("test-internals", "relay-policy", "relay-inspect", "auth_toy")),
                    f"private library or header installed: {f}")
        for f in pkg.glob("libmoqRelay*.cmake"):
            require(str(args.source) not in f.read_text() and
                    str(args.build) not in f.read_text(), f"unrelocatable export: {f}")

        consumer = work / "consumer"
        shutil.copytree(args.source / "tests/consumer", consumer)
        # Base discovery must remain lazy; repeated component finds must compose.
        (consumer / "finds.cmake").write_text(
            "find_package(libmoq CONFIG REQUIRED)\n"
            "if(TARGET moq::relay OR TARGET moq::relay-core)\n"
            '  message(FATAL_ERROR "base package eagerly imports relay")\n'
            "endif()\nfind_package(libmoq CONFIG REQUIRED COMPONENTS relay-core)\n")
        cmake_source = consumer / "CMakeLists.txt"
        cmake_source.write_text(cmake_source.read_text().replace(
            "set(RELAY_COMPONENT", 'include("${CMAKE_CURRENT_LIST_DIR}/finds.cmake")\nset(RELAY_COMPONENT', 1))
        for component in components:
            build = work / (component + " build")
            run([args.cmake, *context, "-S", consumer, "-B", build, f"-Dlibmoq_DIR={pkg}",
                 f"-DRELAY_COMPONENT={component}", f"-DREQUIRE_SIM={args.sim}",
                 f"-DCMAKE_C_COMPILER={args.cc}",
                 f"-DCMAKE_CXX_COMPILER={args.cxx}",
                 "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                 "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"])
            run([args.cmake, "--build", build, "-j2"])
            for lang in ("c", "cxx"):
                run([build / ("consumer_" + lang)])
            retrieval = build / "consumer_fetch_retrieval"
            if component == "relay" and args.sim == "1":
                result = run([retrieval])
                require("PASS: installed runtime standalone FETCH retrieval" in result.stdout,
                        "installed runtime FETCH retrieval consumer did not pass")
            else:
                require(not retrieval.exists(),
                        f"simulator scenario built without an installed simulator: {retrieval}")

            binary = lib / ("libmoq-" + component +
                            (".a" if args.shared == "0" else
                             ".dylib" if sys.platform == "darwin" else ".so"))
            require(binary.is_file(), f"missing library: {binary}")
            command = ["nm", "-gU"] if sys.platform == "darwin" else ["nm", "-g", "--defined-only"]
            nm = run(command + [binary]).stdout
            symbols = {line.split()[-1].lstrip("_") for line in nm.splitlines()
                       if line.split() and line.split()[-1].lstrip("_").startswith("moqr_")}
            expected = set((args.source / "abi" / (component + ".symbols")).read_text().splitlines())
            # Same-build exports the runtime calls in relay-core: exported,
            # never declared by an installed header.
            internal_list = args.source / "abi" / (component + ".internal.symbols")
            internal = (set(internal_list.read_text().splitlines())
                        if internal_list.is_file() else set())
            require(not (expected & internal), f"symbol listed as public and internal: {expected & internal}")
            installed_text = "\n".join(h.read_text() for h in (inc / "moq/relay").iterdir())
            require(not any(re.search(r"\b" + name + r"\b", installed_text) for name in internal),
                    f"internal symbol declared by an installed header: {internal}")
            require(expected | internal <= symbols,
                    f"missing exports: {(expected | internal) - symbols}")
            if args.shared == "1":
                require(symbols == expected | internal,
                        f"extra shared exports: {symbols - (expected | internal)}")
            require(not any("_debug" in s or "_test_" in s for s in symbols),
                    f"test symbols in production {binary}")

        # The test removes only its own installed metadata to prove refusal.
        exports = list(pkg.glob("libmoqRelayTargets*.cmake"))
        saved = [(f, f.read_bytes()) for f in exports]
        for f, _ in saved:
            f.unlink()
        missing = run([args.cmake, *context, "-S", consumer, "-B", work / "missing runtime",
                       f"-Dlibmoq_DIR={pkg}", "-DRELAY_COMPONENT=relay"], ok=False)
        require(missing.returncode != 0 and "relay" in missing.stderr,
                "missing runtime was accepted")
        for f, data in saved:
            f.write_bytes(data)

        pkgconfig = shutil.which("pkg-config")
        require(pkgconfig is not None, "pkg-config required for SDK contract test")
        boundary = work / "metadata-boundary.c"
        boundary.write_text(r'''#include <moq/relay/relay.h>
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "installed-metadata:%d: %s\n", __LINE__, #expr); return 1; \
} } while (0)
int main(void)
{
    struct { moqr_binding_options_t options; uint32_t tail; } input;
    memset(&input, 0xa5, sizeof(input));
    moqr_binding_options_init_sized(&input.options, sizeof(input.options));
    CHECK(input.options.struct_size == 8 && input.options.flags == 0);
    CHECK(input.tail == UINT32_C(0xa5a5a5a5));
    bool managed = true;
    uint64_t generation = UINT64_MAX;
    moqr_track_t invalid = MOQR_TRACK_INVALID;
    CHECK(moqr_core_track_metadata_managed(NULL, invalid, &managed) == MOQR_ERR_INVAL);
    CHECK(managed);
    CHECK(moqr_core_track_current_generation(NULL, invalid, &generation) == MOQR_ERR_INVAL);
    CHECK(generation == UINT64_MAX);
    moqr_core_relay_cfg_t cfg;
    moqr_core_relay_cfg_init_sized(&cfg, sizeof(cfg), moq_alloc_default());
    moqr_core_t *core = NULL;
    CHECK(moqr_core_create(&cfg, &core) == MOQR_OK);
    moqr_binding_t source = MOQR_BINDING_INVALID, legacy = MOQR_BINDING_INVALID;
    input.options.flags = 2;
    CHECK(moqr_core_binding_open_with_options(core, 1, &input.options, &source) == MOQR_ERR_INVAL);
    CHECK(source._opaque == MOQR_BINDING_INVALID._opaque);
    input.options.flags = MOQR_BINDING_TRACK_METADATA_MANAGED;
    CHECK(moqr_core_binding_open_with_options(core, 1, &input.options, &source) == MOQR_OK);
    CHECK(moqr_core_binding_open(core, 2, &legacy) == MOQR_OK);
    moq_bytes_t part = { (const uint8_t *)"installed", 9 };
    moq_bytes_t name = { (const uint8_t *)"managed", 7 };
    moqr_ns_t ns = { &part, 1 };
    moqr_track_t track = MOQR_TRACK_INVALID;
    CHECK(moqr_core_publish_open(core, source, ns, name, 3, &track) == MOQR_OK);
    managed = false;
    CHECK(moqr_core_track_metadata_managed(core, track, &managed) == MOQR_OK && managed);
    CHECK(moqr_core_track_current_generation(core, track, &generation) == MOQR_OK && generation == 1);
    name.data = (const uint8_t *)"legacy";
    name.len = 6;
    CHECK(moqr_core_publish_open(core, legacy, ns, name, 4, &track) == MOQR_OK);
    CHECK(moqr_core_track_metadata_managed(core, track, &managed) == MOQR_OK && !managed);
    CHECK(moqr_core_track_current_generation(core, track, &generation) == MOQR_OK && generation == 1);
    moqr_core_destroy(core);
    return 0;
}
''')
        boundary_cpp = boundary.with_suffix(".cpp")
        boundary_cpp.write_bytes(boundary.read_bytes())
        env["PKG_CONFIG_LIBDIR"] = str(lib / "pkgconfig")
        env["PKG_CONFIG_PATH"] = ""
        for component in components:
            for static in (False, True):
                flags = shlex.split(run([pkgconfig] + (["--static"] if static else []) +
                                        ["--cflags", "--libs", "libmoq-" + component]).stdout)
                libraries = [f for f in flags if f.startswith("-l")]
                required = ["-lmoq-relay-core", "-lmoq-core"]
                if component == "relay":
                    required.insert(0, "-lmoq-relay")
                require([f for f in libraries if f != "-lpthread"] == required,
                        f"nonminimal pkg-config closure: {libraries}")
                exe = work / f"pc-{component}-{static}"
                run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                     *direct["C"],
                     *(["-DTEST_RUNTIME"] if component == "relay" else []),
                     consumer / "main.c", "-o", exe, *flags,
                     *direct["LINK"], "-Wl,-rpath," + str(lib)])
                run([exe])
                if not static:
                    for cc, source, standard, key in (
                        (args.cc, boundary, "c11", "C"),
                        (args.cxx, boundary_cpp, "c++11", "CXX"),
                    ):
                        probe = work / f"metadata-{component}-{key}"
                        run([cc, f"-std={standard}",
                             "-Wall", "-Wextra", "-Wpedantic", "-Werror", *direct[key],
                             source, "-o", probe, *flags,
                             *direct["LINK"], "-Wl,-rpath," + str(lib)])
                        run([probe])
        examples = list(relocated.rglob("in_memory.c"))
        require(len(examples) == 1, "installed deterministic example missing or duplicated")
        flags = shlex.split(run([pkgconfig, "--static", "--cflags", "--libs",
                                "libmoq-relay-core"]).stdout)
        exe = work / "installed-example"
        run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             *direct["C"], examples[0], "-o", exe, *flags,
             *direct["LINK"], "-Wl,-rpath," + str(lib)])
        run([exe])
        if args.runtime == "1":
            sources = list(relocated.rglob("simple-relay/main.c"))
            require(len(sources) == 1, "installed network example missing or duplicated")
            if args.managed == "1":
                # Copy only the installed files: no access to private source headers.
                source = work / "network consumer"
                shutil.copytree(sources[0].parent, source)
                build = work / "network build"
                run([args.cmake, *context, "-S", source, "-B", build,
                     f"-Dlibmoq_DIR={pkg}", f"-Dmsquic_DIR={args.msquic_dir}",
                     f"-DCMAKE_C_COMPILER={args.cc}", "-DMOQ_WARNINGS_AS_ERRORS=ON",
                     "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                     "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"])
                run([args.cmake, "--build", build, "-j2"])
                help_result = run([build / "moq_simple_relay", "--help"])
                require(help_result.stdout.startswith("Usage: moq_simple_relay"),
                        "installed example help missing")
        print("PASS: relocated SDK C/C++, standalone headers, symbols, lazy/missing components, pkg-config")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
