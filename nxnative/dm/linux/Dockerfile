# Build environment for the Linux host (dm/linux/build.py): native arm64 (colima / Docker on Apple silicon, or an arm64 Linux machine).
# Debian trixie: glibc 2.41, SDL3, glslc.
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang lld llvm python3 glslc libvulkan-dev libsdl3-dev zlib1g-dev make file \
    && rm -rf /var/lib/apt/lists/*
