#!/bin/bash

# Configuration
CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-Wall -Wextra -std=c99 -O2 -fanalyzer"}
SRC_DIR="src"
INC_DIR="include"
BINDIR=${BINDIR:-"./"}
BUILDDIR=${BUILDDIR:-"build"}

# Source files
SOURCES=(
    "${SRC_DIR}/main.c"
    "${SRC_DIR}/http_client.c"
    "${SRC_DIR}/openai_client.c"
    "${SRC_DIR}/session_context.c"
    "${SRC_DIR}/tui.c"
    "${SRC_DIR}/toml.c"
)

# Object files mapping
OBJECT_FILES=()
for src in "${SOURCES[@]}"; do
    # Extract filename without extension and directory
    filename=$(basename "$src" .c)
    OBJECT_FILES+=("${BUILDDIR}/${filename}.o")
done

# --- Cleanup ---
echo "Cleaning previous build..."
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
rm -f ${OBJECT_FILES[@]} ${OUTPUT_EXE}

# --- Compilation ---
echo "Compiling source files..."
for src in "${SOURCES[@]}"; do
    # Extract filename without extension for output path
    filename=$(basename "$src" .c)
    output_obj="${BUILDDIR}/${filename}.o"
    
    # Compile source to object file in the build directory
    # Use -I "$INC_DIR" to include headers from include/
    $CC $CFLAGS -I "$INC_DIR" -c "$src" -o "$output_obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
done

# --- Linking ---
echo "Linking executable..."
# Link all object files from the build directory into the final executable
$CC ${OBJECT_FILES[@]} -o "$BINDIR"/emma-code -lcurl -lcjson
if [ $? -ne 0 ]; then
    echo "Error linking executable"
    exit 1
fi

echo "Build successful! Executable created: ${BINDIR}/emma-code"