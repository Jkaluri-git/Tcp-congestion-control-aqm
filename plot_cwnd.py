#!/usr/bin/env python3
"""
plot_cwnd.py

Plots the congestion-window evolution traces produced by
tcp-aqm-comparison.cc (one <prefix>-cwnd-flowN.tr file per TCP flow,
each line "time_seconds  cwnd_bytes"). Useful for the report's
"congestion window over time" figure, which shows the qualitative
sawtooth/slow-start behaviour of each TCP variant under each AQM.

Usage:
    python3 plot_cwnd.py results/TcpCubic_FqCoDel_run1
"""
import sys
import glob

import matplotlib.pyplot as plt

SEGMENT_SIZE_BYTES = 1448  # must match --tcpPacketSize used for the run


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "results/TcpNewReno_PfifoFast_run1"
    files = sorted(glob.glob(f"{prefix}-cwnd-flow*.tr"))
    if not files:
        print(f"No cwnd trace files found matching {prefix}-cwnd-flow*.tr")
        return

    fig, ax = plt.subplots(figsize=(9, 5))
    for f in files:
        t, cwnd = [], []
        with open(f) as fh:
            for line in fh:
                parts = line.split()
                if len(parts) == 2:
                    t.append(float(parts[0]))
                    cwnd.append(int(parts[1]) / SEGMENT_SIZE_BYTES)
        flow_id = f.split("-cwnd-flow")[-1].replace(".tr", "")
        ax.step(t, cwnd, where="post", label=f"Flow {flow_id}")

    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Congestion window (segments)")
    ax.set_title(f"Congestion window evolution: {prefix.split('/')[-1]}")
    ax.legend()
    fig.tight_layout()
    outfile = f"{prefix}-cwnd.png"
    fig.savefig(outfile, dpi=150)
    print(f"Saved {outfile}")


if __name__ == "__main__":
    main()
