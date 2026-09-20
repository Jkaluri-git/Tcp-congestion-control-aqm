# TCP Congestion Control × AQM Comparison — NS-3 Project

Compares four TCP congestion-control algorithms (NewReno, Cubic, BBR, Vegas)
across four Active Queue Management disciplines (PfifoFast/DropTail, RED,
CoDel, FqCoDel) on a dumbbell topology, with a competing UDP CBR flow used
to check whether each AQM protects real-time traffic from TCP-induced
bufferbloat.

## Files

| File | Purpose |
|---|---|
| `tcp-aqm-comparison.cc` | The NS-3 simulation. Every parameter (TCP variant, queue discipline, number of flows, link rates, loss rate, seed, …) is a `--command-line` argument. |
| `run_experiments.sh` | Runs the full sweep (4 TCP × 4 AQM × N repetitions) and appends one CSV row per run to `results/results.csv`. |
| `analyze_results.py` | Aggregates `results.csv` with 95% confidence intervals and saves comparison bar charts to `figures/`. |
| `plot_cwnd.py` | Plots the congestion-window trace for one specific run (sawtooth/slow-start behaviour figure). |

## 1. One-time setup

Inside your NS-3 source tree (e.g. `~/ns-allinone-3.48/ns-3.48`):

```bash
cp tcp-aqm-comparison.cc scratch/
./ns3 configure --enable-examples --build-profile=optimized
./ns3 build
```

## 2. Smoke test (do this before the full sweep)

```bash
./ns3 run "scratch/tcp-aqm-comparison --tcpVariant=TcpCubic --queueDisc=FqCoDel --runNumber=1"
```

You should see a console summary block (throughput, delay, jitter, PDR,
fairness, queue occupancy) and three new files in the working directory:
`tcp-aqm-cwnd-flow0.tr` (and `flow1`, `flow2`), `tcp-aqm-queue.tr`,
`tcp-aqm-flowmon.xml`, and one row appended to `results.csv`.

If this fails to compile, re-check that `scratch/tcp-aqm-comparison.cc` is
in place and re-run `./ns3 build`; the error message will point at the
exact line.

## 3. Full parameter sweep

```bash
cp run_experiments.sh .
bash run_experiments.sh
```

Default settings run 4 TCP variants × 4 AQMs × 10 repetitions = 160 runs,
each simulating 60 s of traffic. Expect roughly 15–40 minutes total wall
time depending on your machine; reduce `N_RUNS` in the script for a
quicker pass while you're still checking things work.

Everything lands in `results/`:
- `results.csv` — one row per run, this is what the analysis script reads
- `<tcp>_<aqm>_run<N>.log` — console output for that run
- `<tcp>_<aqm>_run<N>-cwnd-flow*.tr` — per-flow congestion-window traces
- `<tcp>_<aqm>_run<N>-queue.tr` — bottleneck queue-length trace
- `<tcp>_<aqm>_run<N>-flowmon.xml` — full FlowMonitor detail for that run

## 4. Analysis and figures

```bash
pip install pandas numpy matplotlib scipy --break-system-packages
python3 analyze_results.py results/results.csv
```

This prints a mean ± 95% CI table per metric to the console and writes
seven grouped bar charts to `figures/`: TCP throughput, delay, jitter,
PDR, Jain's fairness index, UDP (bufferbloat) delay, and average
bottleneck queue occupancy — each broken down by TCP variant × AQM.

For a congestion-window figure (useful in the report to show the
qualitative sawtooth behaviour of each algorithm):

```bash
python3 plot_cwnd.py results/TcpCubic_FqCoDel_run1
```

## 5. Mapping to the report

| Report section | Where it comes from |
|---|---|
| Problem statement / objectives | Compare CC × AQM combinations on throughput, delay, jitter, PDR and fairness — see the header comment in `tcp-aqm-comparison.cc` |
| Methodology | Dumbbell topology, link parameters, and metric definitions — see the header comment and Section 1 default values |
| Implementation | The `.cc` file itself, included in full as an appendix |
| Results | The seven figures in `figures/`, plus the printed summary table |
| Analysis | Explain *why*: e.g. Cubic's window-growth shape vs. NewReno under DropTail vs. CoDel; whether FqCoDel's per-flow fairness queuing improves Jain's index; whether the UDP flow's delay collapses under CoDel/FqCoDel relative to plain DropTail (the bufferbloat story) |
| Conclusion | Which TCP × AQM combination gave the best throughput/latency/fairness trade-off in your results |

## 6. Extending the sweep (optional, for a stronger paper)

To add a packet-loss dimension, add an outer loop over `--errorRate` in
`run_experiments.sh` (e.g. `0 0.001 0.01`) — the simulation already
accepts this parameter and applies it as random loss on the bottleneck
link, independent of congestion-induced loss.

To test a different contention level, vary `--nFlows` (e.g. 2, 5, 10) —
useful for showing how fairness and per-flow throughput degrade as more
TCP flows share the same bottleneck.
