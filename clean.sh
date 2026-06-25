#!/bin/bash

set -e

HOME=$(pwd)
KERNEL_SRC="$HOME/linux"
LLVM_SRC="$HOME/llvm-project"
LLVM_INSTALL="$HOME/opt"
IDNT_SRC="$HOME/identifier"
TRIG_SRC="$HOME/trigger"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

clean_llvm() {
    rm -rf "$LLVM_INSTALL"
    cd "$LLVM_SRC"
    rm -rf build
}

clean_linux() {
    cd "$KERNEL_SRC"
    make mrproper
}

clean_identifier() {
    cd "$IDNT_SRC"
    rm -rf build
}

clean_trigger() {
    cd "$TRIG_SRC"
    rm -rf bin
}

clean_llvm
clean_linux
clean_identifier
clean_trigger