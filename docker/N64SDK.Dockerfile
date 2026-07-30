# Reproducible Linux/amd64 compatibility environment for Mine64's NuSystem build.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl file git make python3 xz-utils libc6-i386 lib32z1 \
    && rm -rf /var/lib/apt/lists/*

# Cross compiler and newlib packages from the public ModernN64SDKArchives mirror.
RUN mkdir -p /tmp/loose-debs \
    && for deb in \
      binutils-mips-n64_2.39-3-1_amd64.deb \
      gcc-mips-n64_12.2.0-3-1_amd64.deb \
      newlib-mips-n64_4.3.0.20230120-5-1_amd64.deb; do \
        curl --fail --location --retry 2 --output /tmp/loose-debs/$deb \
          https://raw.githubusercontent.com/ModernN64SDKArchives/n64sdkmod/master/loose-debs/$deb; \
      done \
    && dpkg -i /tmp/loose-debs/*.deb

# Build installable compatibility packages directly from the public archive.
RUN git clone --depth 1 --filter=blob:none --no-checkout \
      https://github.com/ModernN64SDKArchives/n64sdkmod.git /tmp/n64sdkmod \
    && cd /tmp/n64sdkmod \
    && git sparse-checkout set \
      packages/n64sdk \
      packages/n64sdk-common \
      packages/libnusys \
      packages/libnustd \
      packages/libcart \
      packages/makemask \
      packages/spicy \
    && git checkout \
    && for package in n64sdk-common n64sdk libnustd libnusys libcart makemask spicy; do \
      dpkg-deb --build /tmp/n64sdkmod/packages/$package /tmp/$package.deb; \
    done \
    && dpkg -i /tmp/n64sdk-common.deb /tmp/n64sdk.deb /tmp/libnustd.deb \
      /tmp/libnusys.deb /tmp/libcart.deb /tmp/makemask.deb /tmp/spicy.deb \
    && mkdir -p /etc/n64/usr \
    && ln -s /usr/include/n64 /etc/n64/usr/include \
    && ln -s /usr/lib/n64 /etc/n64/usr/lib

ENV ROOT=/etc/n64
ENV PATH=/opt/crashsdk/bin:${PATH}
CMD ["/bin/bash"]
