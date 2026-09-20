#!/usr/bin/env bash
#
# run_experiments.sh
#
# Sweeps TCP congestion-control variant x AQM queue discipline x random
# seed for the tcp-aqm-comparison.cc NS-3 scenario, and collects every
# run's one-line summary into a single results.csv for analysis.
#
# Usage (run from the ns-3 top-level directory, e.g. ~/ns-allinone-3.48/ns-3.48):
#   cp /path/to/tcp-aqm-comparison.cc scratch/
#   cp /path/to/run_experiments.sh .
#   ./ns3 build            # make sure the scratch file compiles first
#   bash run_experiments.sh
#
# Adjust N_RUNS down to 1-2 for a fast smoke test before committing to
# the full sweep (4 TCP variants x 4 AQMs x N_RUNS runs).

set -euo pipefail

TCP_VARIANTS=("TcpNewReno" "TcpCubic" "TcpBbr" "TcpVegas")
QUEUE_DISCS=("PfifoFast" "Red" "CoDel" "FqCoDel")
N_RUNS=10          # independent repetitions per configuration (for 95% CIs)
N_FLOWS=3          # competing TCP flows sharing the bottleneck
SIM_TIME=60        # seconds of application traffic per run
RESULTS_DIR="results"
SUMMARY_CSV="${RESULTS_DIR}/results.csv"

mkdir -p "${RESULTS_DIR}"

# Write the CSV header once, up front. The .cc program only appends rows.
echo "tcpVariant,queueDisc,nFlows,bottleneckMbps,bottleneckDelayMs,errorRate,runNumber,tcpThroughputMbps,tcpDelayMs,tcpJitterMs,tcpPDR,jainFairness,udpThroughputMbps,udpDelayMs,udpJitterMs,udpPDR,avgQueueLenPkts,maxQueueLenPkts,tcpLossPct" > "${SUMMARY_CSV}"

TOTAL=$(( ${#TCP_VARIANTS[@]} * ${#QUEUE_DISCS[@]} * N_RUNS ))
COUNT=0

for tcp in "${TCP_VARIANTS[@]}"; do
  for qd in "${QUEUE_DISCS[@]}"; do
    for run in $(seq 1 "${N_RUNS}"); do
      COUNT=$((COUNT + 1))
      PREFIX="${RESULTS_DIR}/${tcp}_${qd}_run${run}"
      echo "[${COUNT}/${TOTAL}] tcpVariant=${tcp} queueDisc=${qd} run=${run}"
      ./ns3 run "scratch/tcp-aqm-comparison \
        --tcpVariant=${tcp} \
        --queueDisc=${qd} \
        --nFlows=${N_FLOWS} \
        --simTime=${SIM_TIME} \
        --runNumber=${run} \
        --outputPrefix=${PREFIX} \
        --summaryFile=${SUMMARY_CSV}" \
        > "${PREFIX}.log" 2>&1
    done
  done
done

echo ""
echo "Done. ${COUNT} runs completed."
echo "Summary CSV : ${SUMMARY_CSV}"
echo "Per-run logs, cwnd/queue traces and FlowMonitor XML are in ${RESULTS_DIR}/"
