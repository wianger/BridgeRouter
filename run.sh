#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

HOME=$(pwd)
KERNEL_SRC="$HOME/linux"
IDNT_SRC="$HOME/identifier"
TRIG_SRC="$HOME/trigger"

run_graph() {
    echo -e "${BLUE}==> Starting identifier tool...${NC}"
    output_dir="$HOME/datalog/out/graph"
    facts_dir="$HOME/datalog/out/facts"
    mkdir -p "$output_dir" "$facts_dir"

    echo -e "${BLUE}==> Generating graph...${NC}"

    $IDNT_SRC/build/lib/identifier \
        -debug-verbose 0 \
        -dump-graph \
        -ignore-reachable \
        -output-dir "$output_dir" \
        $(find "${KERNEL_SRC}" -name "*.c.bc")
        # "${KERNEL_SRC}/net/core/skbuff.c.bc" \
        # "${KERNEL_SRC}/fs/pipe.c.bc" \
        # "${KERNEL_SRC}/ipc/msg.c.bc" \
        # "${KERNEL_SRC}/ipc/msgutil.c.bc" \
        2> "$output_dir/identifier.err"

    echo -e "${GREEN}Graph generation completed.${NC}"

    echo -e "${BLUE}==> Formatting graph.json...${NC}"
    python3 "$HOME/format_graph_json.py" \
        --graph "$output_dir/graph.json" \
        --in-place

    echo -e "${BLUE}==> Converting graph.json to Souffle facts...${NC}"
    python3 "$HOME/graph_to_facts.py" \
        --graph "$output_dir/graph.json" \
        --out "$facts_dir"

    echo -e "${GREEN}Pipeline completed. Facts at: ${facts_dir}${NC}"
}

run_trigger() {
    echo -e "${BLUE}==> Running trigger tool...${NC}"

    export GOROOT=/home/user/go
    export GOPATH=/home/user/gopath
    export PATH="$GOROOT/bin:$PATH"

    cd "$TRIG_SRC" || exit 1
    ./bin/syz-manager -config my.cfg

    echo -e "${GREEN}Trigger tool execution completed.${NC}"
}

show_help() {
    echo -e "${YELLOW}Usage: $0 <tool>${NC}"
    echo ""
    echo -e "${GREEN}Tools:${NC}"
    echo -e "  ${BLUE}graph${NC}         Generate graph.json + Souffle facts"
    echo -e "  ${BLUE}trigger${NC}       Run syzkaller fuzzer"
    exit 1
}

if [ $# -eq 0 ]; then
    echo -e "${RED}Error: No tool specified.${NC}"
    show_help
fi

case "$1" in
    graph)
        run_graph
        ;;
    trigger)
        run_trigger
        ;;
    *)
        echo -e "${RED}Error: Unknown tool '$1'.${NC}"
        show_help
        ;;
esac
