#!/bin/bash
# Test rapide sur un PC personnel (Linux, ou macOS avec gcc de Homebrew).
#   ./run_local.sh            -> 1024 et 4096
#   ./run_local.sh 8000       -> une seule taille
set -e
cd "$(dirname "$0")"
make stencil
export OMP_PROC_BIND=${OMP_PROC_BIND:-close}
PY=$(command -v python3 || command -v python)
sizes=${*:-1024 4096}
for size in $sizes; do
  echo "=== ${size}x${size}, 100 itérations ==="
  ./stencil $size $size 100
  if [ -f stencil_${size}_${size}_100.pgm ]; then
    $PY check.py --ref-stencil-file stencil_${size}_${size}_100.pgm --stencil-file stencil.pgm
  fi
done
