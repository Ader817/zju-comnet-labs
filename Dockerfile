# Use the official Ubuntu Noble image as the base image
FROM ubuntu:noble

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC \
    LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

# Install dependencies
RUN sed -i 's|archive.ubuntu.com|mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|security.ubuntu.com|mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|ports.ubuntu.com|mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && apt update \
    && apt install -y --no-install-recommends \
       build-essential \
       clang \
       clang-format \
       clang-tidy \
       cmake \
       gdb \
       git \
       libpcap-dev \
       pkg-config \
       tcpdump \
       tshark \
       locales \
       sudo \
       vim \
       fish \
       telnet \
    && locale-gen en_US.UTF-8 \
    && apt clean \
    && rm -rf /var/lib/apt/lists/*

# Run fish shell by default
CMD ["fish"]
