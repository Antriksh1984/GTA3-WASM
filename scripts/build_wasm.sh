#!/bin/bash

# Ensure emcmake is in the PATH
if ! command -v emcmake &> /dev/null; then
    echo "Error: emcmake could not be found. Please ensure the Emscripten SDK is installed and activated."
    exit 1
fi

mkdir -p build-wasm
cd build-wasm

echo "Configuring WASM build using emcmake..."
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release

echo "Building WASM module..."
cmake --build . --parallel 4

echo "Build complete. Check the build-wasm directory for the generated .html, .js, and .wasm files."
