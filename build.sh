#!/bin/bash

# Configuration
CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-Wall -Wextra -std=c99 -O2 -fanalyzer"}
SRC_DIR="src"
INC_DIR="include"
BINDIR=${BINDIR:-"./"}
BUILDDIR=${BUILDDIR:-"build"}

# All external dependencies resolved through pkg-config: libcurl, cJSON
# and the Lua runtime (system liblua, 5.4 preferred, 5.3 fallback)
for p in libcurl libcjson; do
    if ! pkg-config --exists "$p"; then
        echo "Error: pkg-config could not find '$p'. Install the corresponding development package." >&2
        exit 1
    fi
done
CURL_CFLAGS=$(pkg-config --cflags libcurl)
CURL_LIBS=$(pkg-config --libs libcurl)
CJSON_CFLAGS=$(pkg-config --cflags libcjson)
CJSON_LIBS=$(pkg-config --libs libcjson)
LUA_PKG=""
for p in lua5.4 lua5.3 lua; do
    if pkg-config --exists "$p"; then LUA_PKG="$p"; break; fi
done
if [ -z "$LUA_PKG" ]; then
    echo "Error: Lua development files not found. Install liblua5.4-dev (Debian/Ubuntu) or lua-devel (Fedora)" >&2
    exit 1
fi
LUA_CFLAGS=$(pkg-config --cflags "$LUA_PKG")
LUA_LIBS=$(pkg-config --libs "$LUA_PKG")

# Source files
SOURCES=(
    "${SRC_DIR}/main.c"
    "${SRC_DIR}/http_client.c"
    "${SRC_DIR}/openai_client.c"
    "${SRC_DIR}/session_context.c"
    "${SRC_DIR}/tui.c"
    "${SRC_DIR}/toml.c"
    "${SRC_DIR}/commands.c"
    "${SRC_DIR}/lua_scripting.c"
    "${SRC_DIR}/agent_loop.c"
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
    $CC $CFLAGS $CURL_CFLAGS $CJSON_CFLAGS $LUA_CFLAGS -I "$INC_DIR" -c "$src" -o "$output_obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
done

# --- Linking ---
echo "Linking executable..."
# Link all object files from the build directory into the final executable
$CC ${OBJECT_FILES[@]} -o "$BINDIR"/emma-code $CURL_LIBS $CJSON_LIBS $LUA_LIBS
if [ $? -ne 0 ]; then
    echo "Error linking executable"
    exit 1
fi

echo "Build successful! Executable created: ${BINDIR}/emma-code"