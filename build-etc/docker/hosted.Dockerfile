FROM ubuntu:24.04

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
        flex \
        gettext \
        git \
        libgmp-dev \
        libmpc-dev \
        libmpfr-dev \
        libssl-dev \
        nasm \
        patch \
        perl \
        python3 \
        python3-requests \
        sqlite3 \
        texinfo \
        wget \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
