#!/usr/bin/env python3
"""
analyze_results.py

Reads results/results.csv (produced by run_experiments.sh calling
tcp-aqm-comparison.cc once per configuration/run), aggregates each
(tcpVariant, queueDisc) configuration over its repeated runs with a
mean +/- 95% confidence interval, prints a summary table, and saves a
set of grouped-bar comparison figures as PNG files.

Usage:
    pip install pandas numpy matplotlib scipy --break-system-packages
    python3 analyze_results.py results/results.csv
"""
import os
import sys

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from scipy import stats


def mean_ci95(x):
    """Return (mean, half-width of the 95% CI) for a 1-D array-like."""
    x = np.asarray(x, dtype=float)
    n = len(x)
    m = x.mean()
    if n < 2:
        return m, 0.0
    se = x.std(ddof=1) / np.sqrt(n)
    h = se * stats.t.ppf(0.975, n - 1)
    return m, h


def summarize(df, metric):
    rows = []
    for (tcp, qd), g in df.groupby(["tcpVariant", "queueDisc"]):
        m, h = mean_ci95(g[metric])
        rows.append({"tcpVariant": tcp, "queueDisc": qd, "mean": m, "ci95": h})
    return pd.DataFrame(rows)


def grouped_bar(df, metric, ylabel, title, outfile):
    tcps = sorted(df["tcpVariant"].unique())
    qds = sorted(df["queueDisc"].unique())
    summary = summarize(df, metric)

    x = np.arange(len(qds))
    width = 0.8 / max(len(tcps), 1)
    fig, ax = plt.subplots(figsize=(8, 5))

    for i, tcp in enumerate(tcps):
        means, errs = [], []
        for qd in qds:
            row = summary[(summary.tcpVariant == tcp) & (summary.queueDisc == qd)]
            means.append(row["mean"].values[0] if len(row) else 0)
            errs.append(row["ci95"].values[0] if len(row) else 0)
        ax.bar(x + i * width, means, width, yerr=errs, capsize=3, label=tcp)

    ax.set_xticks(x + width * (len(tcps) - 1) / 2)
    ax.set_xticklabels(qds)
    ax.set_xlabel("AQM / queue discipline")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(title="TCP variant")
    fig.tight_layout()
    fig.savefig(outfile, dpi=150)
    plt.close(fig)
    print(f"Saved {outfile}")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "results/results.csv"
    df = pd.read_csv(path)

    metrics = [
        ("tcpThroughputMbps", "Throughput (Mbps)", "Average TCP throughput"),
        ("tcpDelayMs", "Delay (ms)", "Average TCP end-to-end delay"),
        ("tcpJitterMs", "Jitter (ms)", "Average TCP jitter"),
        ("tcpPDR", "PDR (%)", "TCP packet delivery ratio"),
        ("jainFairness", "Jain's fairness index", "Fairness across competing TCP flows"),
        ("udpDelayMs", "Delay (ms)", "Competing UDP flow delay (bufferbloat check)"),
        ("avgQueueLenPkts", "Packets", "Average bottleneck queue occupancy"),
    ]

    os.makedirs("figures", exist_ok=True)
    for metric, ylabel, title in metrics:
        grouped_bar(df, metric, ylabel, title, f"figures/{metric}.png")

    print("\n=== Overall summary (mean +/- 95% CI, across runNumber repetitions) ===")
    for metric, _, _ in metrics:
        print(f"\n-- {metric} --")
        print(summarize(df, metric).round(3).to_string(index=False))


if __name__ == "__main__":
    main()
