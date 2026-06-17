#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

HOME=$(pwd)
KERNEL_SRC="$HOME/linux"
IDNT_SRC="$HOME/identifier"
TRIG_SRC="$HOME/trigger"
TOOL_SRC="$HOME/tools"
LLVM_INSTALL="$HOME/opt/llvm"
KERNEL_VERSION="v5.11"

apply_log_cap_patch() {
    if grep -q "sys_get_log_cap" "$KERNEL_SRC/arch/x86/entry/syscalls/syscall_64.tbl" &&
       grep -q "log_cap.o" "$KERNEL_SRC/kernel/Makefile" &&
       [ -f "$KERNEL_SRC/kernel/log_cap.c" ]; then
        return
    fi

    patch -p1 -d "$KERNEL_SRC" < "$TOOL_SRC/linux-log-cap.patch"
}

rebuild_kernel() {
    echo -e "${BLUE}==> Rebuilding Linux kernel with identifier output...${NC}"

    cd "$KERNEL_SRC" || exit 1
    git checkout "$KERNEL_VERSION"
    make mrproper
    git checkout -- Makefile
    patch -p1 -d "$KERNEL_SRC" < "$TOOL_SRC/linux-makefile.patch"
    apply_log_cap_patch
    cp "$TOOL_SRC/linux_config" .config
    make olddefconfig LLVM="$LLVM_INSTALL/bin/"
    make LLVM="$LLVM_INSTALL/bin/" -j"$(nproc)" bzImage 2> err

    echo -e "${GREEN}Kernel rebuild completed successfully.${NC}"
}

run_identifier() {
    echo -e "${BLUE}==> Starting identifier tool...${NC}"
    output_dir="$HOME/dump"
    mkdir -p "$output_dir"

    $IDNT_SRC/build/lib/identifier \
        -debug-verbose 0 \
        -dump-keystructs \
        -output-dir "$output_dir" \
        $(find "${KERNEL_SRC}" -name "*.c.bc") \
        2> $IDNT_SRC/err

    echo -e "${GREEN}Identifier tool execution completed.${NC}"

    echo -e "${BLUE}==> Merging output files...${NC}"
    cp "$HOME/tools/merge.py" "$output_dir"
    cd "$output_dir" || exit 1
    python3 merge.py $KERNEL_SRC

    merged_file="$output_dir/merged.json"
    if [ -f "$merged_file" ]; then
        echo -e "${GREEN}Output file has been successfully merged: $merged_file${NC}"
    else
        echo -e "${RED}Error: merged.json was not generated.${NC}"
        exit 1
    fi

    rebuild_kernel
}

run_trigger() {
    echo -e "${BLUE}==> Running trigger tool...${NC}"

    cd "$TRIG_SRC" || exit 1
    ./bin/syz-manager -debug -config "$HOME/tools/default.cfg"

    echo -e "${GREEN}Trigger tool execution completed.${NC}"
}

show_help() {
    echo -e "${YELLOW}Usage: $0 <tool>${NC}"
    echo ""
    echo -e "${GREEN}Tools:${NC}"
    echo -e "  ${BLUE}identifier${NC}    Analyze the kernel source and identify specific objects."
    echo -e "  ${BLUE}trigger${NC}       Generate code to trigger specific kernel objects based on identifier output."
    exit 1
}

if [ $# -eq 0 ]; then
    echo -e "${RED}Error: No tool specified.${NC}"
    show_help
fi

case "$1" in
    identifier)
        run_identifier
        ;;
    trigger)
        run_trigger
        ;;
    *)
        echo -e "${RED}Error: Unknown tool '$1'.${NC}"
        show_help
        ;;
esac
