# Dockerfile for moq-interop-client
#
# Build context should be the parent directory containing libmoq/,
# picoquic/, and picotls/ as siblings.
# Both providers must be independent Git checkouts, not copied worktree gitfiles.
#
# Example:
#   cd /path/to/parent   # contains libmoq/ picoquic/ picotls/
#   docker build -f libmoq/tools/moq-interop-client/Dockerfile -t moq-interop-client .
#
# Run:
#   docker run --rm moq-interop-client --relay moqt://relay:4443 --test setup-only --tls-disable-verify
#   docker run --rm -e RELAY_URL=moqt://relay:4443 -e TESTCASE=setup-only -e TLS_DISABLE_VERIFY=1 moq-interop-client

FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libssl-dev ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY picotls/ picotls/
COPY picoquic/ picoquic/
COPY libmoq/ libmoq/

# Apply only the ownership correction still required by the adapter. Accept
# patched contexts, but refuse conflicting provider sources before building.
RUN cd /src/picoquic \
    && { test "$(git rev-parse --show-toplevel 2>/dev/null)" = /src/picoquic \
         || { echo 'picoquic patch input must be its own git root' >&2; exit 1; }; } \
    && patch=/src/libmoq/patches/picoquic/0006-own-application-close-reason.patch \
    && if git apply --check "$patch" >/dev/null 2>&1; then \
         git apply "$patch"; \
       elif git apply --reverse --check "$patch" >/dev/null 2>&1; then \
         echo 'picoquic close-reason ownership patch already applied'; \
       else \
         echo 'picoquic close-reason ownership patch conflicts' >&2; exit 1; \
       fi

# GCC needs explicit initialization of picotls's bounded scratch storage.
RUN cd /src/picotls \
    && { test "$(git rev-parse --show-toplevel 2>/dev/null)" = /src/picotls \
         || { echo 'picotls patch input must be its own git root' >&2; exit 1; }; } \
    && patch=/src/libmoq/patches/picotls/0001-initialize-compiler-visible-scratch.patch \
    && if git apply --check "$patch" >/dev/null 2>&1; then \
         git apply "$patch"; \
       elif git apply --reverse --check "$patch" >/dev/null 2>&1; then \
         echo 'picotls scratch initialization patch already applied'; \
       else \
         echo 'picotls scratch initialization patch conflicts' >&2; exit 1; \
       fi

# Build picotls (fresh build dir to avoid host CMakeCache)
RUN cmake -B /build/picotls -S picotls \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PROJECT_picotls_INCLUDE=/src/libmoq/cmake/MoqPicotlsBuildPolicy.cmake \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /build/picotls -j$(nproc) \
    && cmake --install /build/picotls

# Build libmoq with the interop client on the service tier.
# The endpoint owns transport selection + version negotiation, so the client
# needs BOTH transports compiled in:
#   raw QUIC      -> MOQ_BUILD_PQ_THREADED (+ ADAPTER_PICOQUIC)
#   WebTransport  -> MOQ_BUILD_PICO_WT_MANAGED (+ ADAPTER_PICO_WT)
# and the service tier itself (+ its public moq::msf dependency).
RUN cmake -B /build/libmoq -S libmoq \
    -DCMAKE_BUILD_TYPE=Release \
    -DMOQ_BUILD_ADAPTER_PICOQUIC=ON \
    -DMOQ_BUILD_PQ_THREADED=ON \
    -DMOQ_BUILD_ADAPTER_PICO_WT=ON \
    -DMOQ_BUILD_PICO_WT_MANAGED=ON \
    -DMOQ_BUILD_MSF=ON \
    -DMOQ_BUILD_SERVICE=ON \
    -DMOQ_BUILD_INTEROP_CLIENT=ON \
    -DMOQ_BUILD_TESTS=OFF \
    -DMOQ_PICOQUIC_SOURCE_DIR=/src/picoquic \
    -DMOQ_PICOTLS_PREFIX=/build/picotls \
    -DPTLS_INCLUDE_DIR=/src/picotls/include \
    && cmake --build /build/libmoq -j$(nproc) --target moq-interop-client

# Runtime image
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/libmoq/tools/moq-interop-client/moq-interop-client /usr/local/bin/

ENTRYPOINT ["moq-interop-client"]
