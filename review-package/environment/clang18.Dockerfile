FROM ubuntu:24.04

RUN apt-get update -qq \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
       clang-18 \
       clang-tools-18 \
       bc \
       bison \
       cmake \
       dwarves \
       flex \
       libclang-18-dev \
       libelf-dev \
       libssl-dev \
       llvm-18-dev \
       lld-18 \
       make \
       ninja-build \
       perl \
       pkg-config \
       python3 \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update -qq \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
