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

should_rebuild() {
    [ "${FORCE_REBUILD:-0}" = "1" ]
}

skip_step() {
    echo -e "${YELLOW}==> Skipping $1; build output already exists.${NC}"
}

sync_clang_trigger() {
    cp "$TOOL_SRC/clang-trigger" "$LLVM_INSTALL/bin/clang-14"
}

apply_log_cap_patch() {
    if grep -q "sys_get_log_cap" "$KERNEL_SRC/arch/x86/entry/syscalls/syscall_64.tbl" &&
       grep -q "log_cap.o" "$KERNEL_SRC/kernel/Makefile" &&
       [ -f "$KERNEL_SRC/kernel/log_cap.c" ]; then
        return
    fi

    patch -p1 -d "$KERNEL_SRC" < "$TOOL_SRC/linux-log-cap.patch"
}

kernel_is_current() {
    local image="$KERNEL_SRC/arch/x86/boot/bzImage"
    local vmlinux="$KERNEL_SRC/vmlinux"
    local merged_json="$HOME/dump/merged.json"

    [ -f "$image" ] || return 1
    [ -f "$vmlinux" ] || return 1
    if [ -f "$merged_json" ] && [ "$merged_json" -nt "$image" ]; then
        return 1
    fi
    return 0
}

install_llvm() {
    if ! should_rebuild && \
        [ -x "$LLVM_INSTALL/bin/clang-14_bk" ] && \
        [ -x "$LLVM_INSTALL/bin/clang-14" ] && \
        [ -f "$LLVM_INSTALL/lib/LLVMCapabilityLog.so" ]; then
        sync_clang_trigger
        skip_step "LLVM"
        return
    fi

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
    sync_clang_trigger

    echo -e "${GREEN}LLVM installation completed!${NC}"
}

build_linux() {
    if ! should_rebuild && kernel_is_current; then
        skip_step "Linux kernel"
        return
    fi

    echo -e "${BLUE}==> Building Linux kernel...${NC}"
    cd $HOME
    cd $KERNEL_SRC
    git checkout $KERNEL_VERSION
    make mrproper
    git checkout -- Makefile
    patch -p1 -d $KERNEL_SRC < $TOOL_SRC/linux-makefile.patch
    apply_log_cap_patch
    cp $TOOL_SRC/linux_config .config
    make olddefconfig LLVM=$HOME/opt/llvm/bin/
    if make LLVM=$HOME/opt/llvm/bin/ -j`nproc` bzImage 2> err; then
        echo -e "${GREEN}Kernel build completed successfully!${NC}"
    else
        echo -e "${RED}Kernel build failed! Check the logs for details.${NC}"
        exit 1
    fi
}

build_identifier() {
    if ! should_rebuild && [ -x "$HOME/identifier/build/lib/identifier" ]; then
        skip_step "Identifier"
        return
    fi

    echo -e "${BLUE}==> Building Identifier...${NC}"
    cd $HOME/identifier
    if should_rebuild; then
        make clean
    fi
    make
}

build_trigger() {
    if ! should_rebuild && \
        [ -x "$HOME/trigger/bin/syz-manager" ] && \
        [ -x "$HOME/trigger/bin/linux_amd64/syz-executor" ]; then
        skip_step "Trigger"
        return
    fi

    echo -e "${BLUE}==> Building Trigger...${NC}"
    cd $HOME/trigger
    if should_rebuild; then
        make clean
    fi
    make
}

git submodule update --init --recursive
install_llvm
build_linux
build_identifier
build_trigger