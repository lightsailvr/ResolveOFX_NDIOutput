# Windows / CUDA — status

**The Windows port is in progress on branch `windows-port` and does not ship yet.**

An earlier version of this file documented CUDA features, automatic fallback behavior, and performance benchmarks for an implementation that was never completed — it described *intended* behavior, not code that ever ran. It has been cut down to this pointer so nobody is misled until the port actually lands.

Where the real information lives:

- **Plan & decisions:** [docs/windows-port-spec.md](docs/windows-port-spec.md)
- **Research behind them:** [docs/2026-08-30-windows-port-feasibility.md](docs/2026-08-30-windows-port-feasibility.md)
- **Build status & toolchain:** [BUILD.md](BUILD.md), Windows section
- **Work items:** this repo's GitHub issues labeled `windows`, each referencing the `windows-port` branch

macOS remains the released platform — see [README.md](README.md).
