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
        sqlite3 \
        texinfo \
        xorriso \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The final compiler is staged around the target libc: first build a compiler
# without headers, use it to build musl, then finish GCC with libstdc++ support.
RUN python3 scripts/bootstrap_toolchain.py \
        x86_64-pedigree /opt/pedigree \
        --source-root /src \
        --sysroot /opt/pedigree/musl-sdk/usr \
        --jobs "$(nproc)"

RUN cmake -S . -B build/toolchain -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/build-etc/cmake/pedigree_amd64.cmake \
        -DPEDIGREE_TOOLCHAIN_ROOT=/opt/pedigree \
        -DPEDIGREE_BUILD_USER_DIR=OFF \
        -DPEDIGREE_BUILD_UEFI=OFF \
        -DPEDIGREE_WITH_INIT=OFF \
        -DPEDIGREE_WARNINGS=ON \
    && cmake --build build/toolchain \
        --target libc \
        --parallel "$(nproc)"

# Keep the complete, relocatable SDK in the final image. The compiler consumes
# its usr prefix while the manifest remains rooted at the package directory.
RUN cp -a build/toolchain/musl /opt/pedigree/musl-sdk \
    && python3 scripts/bootstrap_toolchain.py \
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
        sqlite3 \
        zlib1g-dev \
        xorriso \
    && rm -rf /var/lib/apt/lists/*

COPY --from=toolchain-builder /opt/pedigree /opt/pedigree

ENV PATH="/usr/lib/llvm-${LLVM_VERSION}/bin:/opt/pedigree/bin:${PATH}" \
    PEDIGREE_TOOLCHAIN_ROOT=/opt/pedigree

WORKDIR /workspace
