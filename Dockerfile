FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive

# Add ARM architecture and install cross-compiler + SDL for ARM
RUN dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y \
        gcc-arm-linux-gnueabihf \
        libsdl1.2-dev:armhf \
        libsdl-mixer1.2-dev:armhf \
        libsdl-ttf2.0-dev:armhf \
        libsdl-image1.2-dev:armhf \
        make \
        wget \
        bzip2 \
    && rm -rf /var/lib/apt/lists/*

# Cross-compile mpg123 for ARM as shared lib (glibc 2.27 compat)
RUN wget -q https://mpg123.de/download/mpg123-1.32.10.tar.bz2 && \
    tar xf mpg123-1.32.10.tar.bz2 && \
    cd mpg123-1.32.10 && \
    ./configure --host=arm-linux-gnueabihf \
        --prefix=/usr/arm-linux-gnueabihf \
        --disable-largefile \
        --with-audio=dummy \
        --enable-shared \
        --disable-static && \
    make -j$(nproc) && \
    make install && \
    cd / && rm -rf mpg123-1.32.10*

WORKDIR /work
