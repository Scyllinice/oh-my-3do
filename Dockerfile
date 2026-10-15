# syntax=docker/dockerfile:1
FROM ubuntu:24.04

# Use --platform=linux/amd64: the original ARM tools are x86 ELF binaries.
ARG TARGETARCH
RUN test "$TARGETARCH" = amd64

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        bash build-essential ca-certificates file git \
        libc6:i386 libgcc-s1:i386 libstdc++6:i386 \
        python3 unzip zip wine wine32:i386 wine64 xauth xvfb \
    && rm -rf /var/lib/apt/lists/*

# Override with a commit SHA to lock a known-working toolchain revision.
# The resolved SHA is retained even when building from the default branch.
ARG DEVKIT_REF=4e370299bd273bb13089d41fa33c5e863dc6d405
RUN git init /opt/3do-devkit \
    && git -C /opt/3do-devkit remote add origin https://github.com/trapexit/3do-devkit.git \
    && git -C /opt/3do-devkit fetch --depth=1 origin "$DEVKIT_REF" \
    && git -C /opt/3do-devkit checkout --detach FETCH_HEAD \
    && git -C /opt/3do-devkit submodule update --init --recursive \
    && git -C /opt/3do-devkit rev-parse HEAD > /opt/3do-devkit.revision

ENV TDO_DEVKIT_PATH=/opt/3do-devkit
ENV PATH="/opt/3do-devkit/bin/buildtools/linux:/opt/3do-devkit/bin/tools/linux:/opt/3do-devkit/bin/compiler/linux:${PATH}"
ENV WINEDEBUG=-all
ENV WINEDLLOVERRIDES="mscoree,mshtml="

COPY docker/entrypoint.sh /usr/local/bin/devkit-entrypoint
COPY docker/smoke.sh /usr/local/bin/devkit-smoke
RUN chmod 755 /usr/local/bin/devkit-entrypoint /usr/local/bin/devkit-smoke \
    && useradd --create-home --uid 10001 --shell /bin/bash developer \
    && mkdir -p /workspace \
    && chown developer:developer /workspace

USER developer
WORKDIR /workspace

# Fail the image build if the compiler, linker, packer or verifier cannot run.
RUN devkit-entrypoint devkit-smoke /tmp/devkit-smoke

# Image preparation is optional at runtime but available in the same container.
USER root
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends imagemagick \
    && rm -rf /var/lib/apt/lists/*
USER developer

# Also compile our actual sorting component with Norcroft, using SDK ABI flags,
# and run the host tests. Temporary files stay out of the mounted project.
COPY --chown=developer:developer Makefile /tmp/project-check/Makefile
COPY --chown=developer:developer src/ /tmp/project-check/src/
COPY --chown=developer:developer tests/ /tmp/project-check/tests/
COPY --chown=developer:developer runtime/ /tmp/project-check/runtime/
COPY --chown=developer:developer tools/prepare-runtime.py /tmp/project-check/tools/prepare-runtime.py
COPY --chown=developer:developer ohmy3do.ini /tmp/project-check/ohmy3do.ini
RUN make -C /tmp/project-check test test-sdk \
    && armcc -I/opt/3do-devkit/include/3do \
        -bigend -za1 -zi4 -fpu none -arch 3 \
        -apcs 3/32/nofp/swst/wide/softfp \
        -c /tmp/project-check/src/menu_sort.c -o /tmp/project-check/menu_sort.o

ENTRYPOINT ["devkit-entrypoint"]
CMD ["make", "test"]
