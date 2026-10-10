#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Compila e empacota o BBPort dentro de um container Ubuntu 22.04 LTS (glibc 2.35)
# Isso garante compatibilidade máxima com quase todas as distribuições Linux modernas.
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="bbport-builder:ubuntu22"

echo "======================================================================"
echo "    BBPort - Docker Build (Ubuntu 22.04 LTS / glibc 2.35)"
echo "======================================================================"

echo -e "\n[*] [1/2] Construindo/Verificando imagem Docker: $IMAGE_NAME..."
docker build -t "$IMAGE_NAME" -f "$ROOT/packaging/Dockerfile.ubuntu22" "$ROOT"

# Se houver um CMakeCache gerado fora do container (com caminhos do host), limpa-o
if [[ -f "$ROOT/out/gpu/CMakeCache.txt" ]] && ! grep -q "/bbport" "$ROOT/out/gpu/CMakeCache.txt" 2>/dev/null; then
    rm -rf "$ROOT/out/gpu/CMakeCache.txt" "$ROOT/out/gpu/CMakeFiles"
fi

echo -e "\n[*] [2/2] Compilando e gerando release dentro do container..."
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$ROOT:/bbport" \
    -w /bbport \
    "$IMAGE_NAME" \
    bash -c "bash build.sh && python3 package.py --no-build \"\$@\"" -- "$@"

echo -e "\n======================================================================"
echo "    BUILD CONCLUÍDO COM SUCESSO!"
echo "    Os binários compatíveis e o pacote .tar.gz estão em: dist/"
echo "======================================================================"
