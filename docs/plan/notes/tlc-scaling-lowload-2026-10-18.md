# TLC Performance Benchmark Report (nproc=8 across all runs)

### MC_interrupt.cfg (Safety)
| Workers | -Xmx | Load B/A | Retries | Wall (s) | CPU (s) | Max RSS | States | States/s |
|:---:|:---:|:---:|:---:|---:|---:|---:|---:|---:|
| 1 | none | 3.84 / 1.86 | 193 | 162.1 | 175.33 | 5587 MB | 547645 | 3378.4 |
| 2 | none | 1.58 / 2.33 | 0 | 91.26 | 194.32 | 5622 MB | 547645 | 6000.9 |
| 4 | none | 2.33 / 3.98 | 0 | 59.84 | 250.03 | 5602 MB | 547645 | 9151.8 |
| 8 | none | 3.66 / 7.85 | 0 | 51.28 | 349.2 | 5606 MB | 547645 | 10679.5 |
| 2 | 512m | 3.98 / 2.97 | 4 | 92.3 | 195.9 | 648 MB | 547645 | 5933.3 |
| 2 | 1g | 2.51 / 2.31 | 0 | 89.35 | 190.53 | 1174 MB | 547645 | 6129.2 |
| 2 | 2g | 2.13 / 2.81 | 0 | 89.55 | 191.13 | 1530 MB | 547645 | 6115.5 |
| 2 | 4g | 2.59 / 3.11 | 0 | 90.38 | 192.84 | 1951 MB | 547645 | 6059.4 |
| 8 | 512m | 3.00 / 7.43 | 0 | 50.83 | 342.28 | 673 MB | 547645 | 10774.1 |
| 8 | 1g | 3.65 / 6.91 | 4 | 51.0 | 340.04 | 1185 MB | 547645 | 10738.1 |
| 8 | 2g | 3.92 / 7.78 | 3 | 50.12 | 339.36 | 1550 MB | 547645 | 10926.7 |
| 8 | 4g | 3.89 / 7.44 | 4 | 50.5 | 340.77 | 1986 MB | 547645 | 10844.5 |

### MC_interrupt_liveness.cfg (Liveness)
| Workers | -Xmx | Load B/A | Retries | Wall (s) | CPU (s) | Max RSS | States | States/s |
|:---:|:---:|:---:|:---:|---:|---:|---:|---:|---:|
| 1 | none | 3.43 / 2.56 | 4 | 43.62 | 54.57 | 4306 MB | 65261 | 1496.1 |
| 2 | none | 2.35 / 3.12 | 0 | 27.31 | 60.85 | 4295 MB | 65261 | 2389.6 |
| 4 | none | 2.87 / 3.49 | 0 | 20.73 | 75.25 | 4294 MB | 65261 | 3148.1 |
| 8 | none | 3.21 / 4.44 | 0 | 22.03 | 91.86 | 4271 MB | 65261 | 2962.4 |
| 2 | 512m | 3.26 / 2.95 | 0 | 25.08 | 56.09 | 365 MB | 65261 | 2602.1 |
| 2 | 1g | 2.79 / 2.90 | 0 | 26.08 | 59.22 | 488 MB | 65261 | 2502.3 |
| 2 | 2g | 2.91 / 3.29 | 0 | 26.8 | 60.48 | 850 MB | 65261 | 2435.1 |
| 2 | 4g | 3.03 / 2.73 | 0 | 25.93 | 58.84 | 1534 MB | 65261 | 2516.8 |
| 4 | 512m | 2.31 / 3.05 | 0 | 19.96 | 73.73 | 385 MB | 65261 | 3269.6 |
| 4 | 1g | 2.59 / 2.66 | 0 | 19.9 | 72.88 | 502 MB | 65261 | 3279.4 |
| 4 | 2g | 2.45 / 3.41 | 0 | 19.89 | 73.25 | 831 MB | 65261 | 3281.1 |
| 4 | 4g | 3.14 / 3.59 | 0 | 20.35 | 74.62 | 1484 MB | 65261 | 3206.9 |

### Conclusions
1. **CPU per state vs workers:** Total CPU time per state increases steadily with workers (MC_interrupt: 0.32 ms/state at 1w -> 0.64 ms/state at 8w; liveness: 0.84 ms/state at 1w -> 1.41 ms/state at 8w) due to state queue synchronization and parallel coordination overhead, even though wall time improves.
2. **Impact of -Xmx:** No -Xmx setting tested (512m, 1g, 2g, 4g) slowed TLC down. In fact, -Xmx 512m/1g was marginally faster than default unbounded heap (`none`), while slashing Max RSS from ~5.6 GB to ~650 MB (safety) and ~4.3 GB to ~370 MB (liveness).
3. **Recommendations:**
   - **Safety / Large models:** **8 workers, -Xmx 1g** (or 2g) - yields maximum throughput (10,926 states/s, 3.2x speedup over 1w) while capping RSS under 1.2 GB.
   - **Liveness / Smaller models (<100k states):** **4 workers, -Xmx 512m** (or 1g) - 8 workers regresses wall time (22.0s vs 19.9s) due to thread contention; 4 workers achieves peak speed (3,281 states/s) with <400 MB RSS.
