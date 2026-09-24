FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    clang \
    clang-format \
    clang-tidy \
    cmake \
    curl \
    gdb \
    git \
    iproute2 \
    libclang-rt-18-dev \
    libgtest-dev \
    lsof \
    netcat-openbsd \
    ninja-build \
    procps \
    redis-tools \
    valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

ENV CC=clang
ENV CXX=clang++

CMD ["bash"]

