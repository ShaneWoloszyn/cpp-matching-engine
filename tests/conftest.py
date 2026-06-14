"""
Auto-discover the compiled orderbook extension module.

Build the project first:
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release
    cmake --install build   # copies orderbook.pyd next to this file

Or set PYTHONPATH to the build output directory manually.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent

# Candidates in priority order (Release build first, then Debug, then bare build).
candidates = [
    ROOT / "tests",           # after `cmake --install build`
    ROOT / "build" / "Release",
    ROOT / "build" / "Debug",
    ROOT / "build",
]

def _has_module(p: Path) -> bool:
    return any(p.glob("orderbook*.pyd")) or any(p.glob("orderbook*.so"))

for _p in candidates:
    if _p.exists() and _has_module(_p):
        sys.path.insert(0, str(_p))
        break
