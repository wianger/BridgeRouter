#!/bin/bash

set -e

HOME=$(pwd)
KERNEL_SRC="$HOME/linux"
KERNEL_VERSION="v5.11"
LLVM_SRC="$HOME/llvm-project"
TOOL_SRC="$HOME/tools"
LLVM_INSTALL="$HOME/opt/llvm"
LLVM_VERSION="release/14.x"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

install_llvm() {
    echo -e "${BLUE}==> Installing LLVM...${NC}"
    rm -rf "$LLVM_INSTALL"
    mkdir -p "$LLVM_INSTALL"
    echo -e "${GREEN}Configuring LLVM...${NC}"
    cd $HOME/llvm-project
    git checkout $LLVM_VERSION
    rm -rf build

    sed -i "s|/home/user/Tools/w2l/code/out/defdebug/dumpResults/merged_output.json|$HOME/dump/merged.json|g" "$HOME/tools/llvm-caplog.patch"
    patch -p1 -d "$LLVM_SRC" < "$HOME/tools/llvm-caplog.patch"

    cmake -S llvm -B build \
        -G "Unix Makefiles" \
        -DCMAKE_INSTALL_PREFIX="$LLVM_INSTALL" \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DCMAKE_BUILD_TYPE=Release

    cd $HOME/llvm-project/build
    echo -e "${GREEN}Building LLVM...${NC}"
    make -j"$(nproc)"

    echo -e "${GREEN}Installing LLVM to ${LLVM_INSTALL}...${NC}"
    make install
    if [ ! -f "$LLVM_INSTALL/lib/LLVMCapabilityLog.so" ]; then
        echo -e "${RED}Error: LLVMCapabilityLog.so not found. Aborting.${NC}"
        exit 1
    fi
    mv $LLVM_INSTALL/bin/clang-14 $LLVM_INSTALL/bin/clang-14_bk
    cp "$HOME/tools/clang-trigger" $LLVM_INSTALL/bin/clang-14

    echo -e "${GREEN}LLVM installation completed!${NC}"
}

build_linux() {
    echo -e "${BLUE}==> Building Linux kernel...${NC}"
    cd $HOME
    cd $KERNEL_SRC
    git checkout $KERNEL_VERSION
    make mrproper
    patch -p1 -d linux < tools/linux-makefile.patch
    cp $TOOL_SRC/linux_config .config
    make menuconfig LLVM=$HOME/opt/llvm/bin/
    make LLVM=$HOME/opt/llvm/bin/ -j`nproc` bzImage 2> err
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Kernel build completed successfully!${NC}"
    else
        echo -e "${RED}Kernel build failed! Check the logs for details.${NC}"
        exit 1
    fi
}

build_identifier() {
    echo -e "${BLUE}==> Building Identifier...${NC}"
    cd $HOME/identifier
    make clean
    make
}

build_trigger() {
    echo -e "${BLUE}==> Building Trigger...${NC}"
    cd $HOME/trigger
    make clean
    make
}

git submodule update --init --recursive
install_llvm
build_linux
build_identifier
build_trigger