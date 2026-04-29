# ════════════════════════════════════════════════════════════════════════
#  Dockerfile — SDR Radio Resource Task Manager
#  Multi-stage: builder → runtime
#  Base: Ubuntu 24.04 (Noble)
# ════════════════════════════════════════════════════════════════════════

# ── Stage 1: Builder ──────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

# Install build tools and dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    git \
    ca-certificates \
    # SoapySDR
    libsoapysdr-dev \
    soapysdr-module-remote \
    # AMQP 1.0 (qpid-proton C++ binding)
    libqpid-proton-cpp12-dev \
    # XML parsing
    libtinyxml2-dev \
    # JSON (try system package, fallback to FetchContent in CMake)
    nlohmann-json3-dev \
    # Logging
    libspdlog-dev \
    libfmt-dev \
    # FFTW3 single precision
    libfftw3-dev \
    # UUID generation
    uuid-dev \
    # POSIX
    libpthread-stubs0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy source
COPY CMakeLists.txt .
COPY include/     include/
COPY src/         src/
COPY client/      client/
COPY config/      config/

# Build release
RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/install \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --install build


# ── Stage 2: Runtime image ────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

# Only runtime libraries — no dev headers
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsoapysdr0.8 \
    soapysdr-module-remote \
    libqpid-proton-cpp12 \
    libtinyxml2-10 \
    libspdlog1.12 \
    libfftw3-single3 \
    libfmt9 \
    uuid-runtime \
    # For tini (proper PID 1 in containers)
    tini \
    && rm -rf /var/lib/apt/lists/*

# Copy built binaries and default config
COPY --from=builder /install/bin/sdr_controller /usr/local/bin/sdr_controller
COPY --from=builder /install/bin/sdr_client     /usr/local/bin/sdr_client
COPY --from=builder /install/etc/sdr-controller /etc/sdr-controller

# Create non-root user (radios accessed via SoapyRemote, no USB devices needed)
RUN groupadd -r sdr && useradd -r -g sdr -s /sbin/nologin sdr

# Config volume mount point (ConfigMap mounts here in Kubernetes)
RUN mkdir -p /etc/sdr-controller && chown sdr:sdr /etc/sdr-controller

USER sdr

# Environment variables with defaults (override via ConfigMap / Deployment env)
ENV SDR_CONFIG_PATH=/etc/sdr-controller/devices.xml
ENV SDR_LOG_LEVEL=info

ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["/usr/local/bin/sdr_controller"]
