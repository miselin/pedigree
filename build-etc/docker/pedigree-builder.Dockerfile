FROM alpine:3.22 AS alpine-sdk

RUN apk add --no-cache e2fsprogs curl tar coreutils
COPY scripts/alpine/build-rootfs.sh /build-rootfs.sh
RUN mkdir /out \
    && ALPINE_ARCH=x86_64 ALPINE_PROFILE=base /build-rootfs.sh

FROM ubuntu:24.04 AS toolchain-builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        binutils \
        bison \
        build-essential \
        ca-certificates \
        cmake \
        e2fsprogs \
        flex \
        genisoimage \
        gettext \
        git \
        libgmp-dev \
        libmpc-dev \
        libmpfr-dev \
        libssl-dev \
        mtools \
        nasm \
        ninja-build \
        patch \
        perl \
        python3 \
        python3-requests \
        texinfo \
        xorriso \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
COPY --from=alpine-sdk /out/sysroot /opt/pedigree/musl-sdk

# Keep the existing compiler bootstrap and C++ runtime build, using the
# prepared Alpine headers and libraries for the final stage.
RUN python3 scripts/bootstrap_toolchain.py \
        x86_64-pedigree /opt/pedigree \
        --source-root /src \
        --sysroot /opt/pedigree/musl-sdk/usr \
        --jobs "$(nproc)"

RUN python3 scripts/bootstrap_toolchain.py \
        x86_64-pedigree /opt/pedigree \
        --source-root /src \
        --sysroot /opt/pedigree/musl-sdk/usr \
        --libcpp \
        --jobs "$(nproc)"

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

ARG LLVM_VERSION=22

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/apt.llvm.org.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/apt.llvm.org.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils \
        bison \
        build-essential \
        clang-${LLVM_VERSION} \
        clang-tools-${LLVM_VERSION} \
        clang-format-${LLVM_VERSION} \
        clang-tidy-${LLVM_VERSION} \
        cmake \
        dosfstools \
        e2fsprogs \
        flex \
        git \
        gettext \
        libssl-dev \
        mtools \
        nasm \
        ninja-build \
        python3 \
        python3-requests \
        qemu-system-x86 \
        zlib1g-dev \
        xorriso \
    && rm -rf /var/lib/apt/lists/*

COPY --from=toolchain-builder /opt/pedigree /opt/pedigree
COPY --from=alpine-sdk /out/rootfs.img /opt/pedigree/alpine/rootfs.img
COPY --from=alpine-sdk /out/rootfs /opt/pedigree/alpine/rootfs

ENV PATH="/usr/lib/llvm-${LLVM_VERSION}/bin:/opt/pedigree/bin:${PATH}" \
    PEDIGREE_TOOLCHAIN_ROOT=/opt/pedigree \
    PEDIGREE_TARGET_SYSROOT=/opt/pedigree/musl-sdk

WORKDIR /workspace
