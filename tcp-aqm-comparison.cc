/*
 * tcp-aqm-comparison.cc
 *
 * NS-3 simulation comparing TCP congestion-control algorithms
 * (NewReno, Cubic, BBR, Vegas) under different Active Queue
 * Management (AQM) disciplines (PfifoFast, RED, CoDel, FqCoDel) on a
 * dumbbell topology, with a competing UDP CBR "real-time" cross flow
 * used to check whether the AQM protects latency-sensitive traffic
 * from TCP-induced bufferbloat.
 *
 * Topology (dumbbell):
 *
 *   TCP sender 0   \                                     / TCP sink 0
 *   TCP sender 1    \                                   /  TCP sink 1
 *      ...            R0 ===== bottleneck link ===== R1        ...
 *   TCP sender N-1  /    (AQM queue disc applied here)   \  TCP sink N-1
 *   UDP sender      /                                     \  UDP sink
 *
 *   Access links:    accessBw / accessDelay    (default 100 Mbps / 1 ms)
 *   Bottleneck link: bottleneckBw / bottleneckDelay (default 10 Mbps / 20 ms)
 *
 * Metrics collected (via ns-3 FlowMonitor + custom trace sources):
 *   - Per-flow throughput, end-to-end delay, jitter, packet delivery
 *     ratio (PDR) for the TCP flows and for the UDP cross flow
 *   - Jain's fairness index across the competing TCP flows
 *   - Congestion-window evolution for every TCP flow
 *   - Bottleneck queue occupancy over time (time-weighted average + peak)
 *
 * Every parameter is exposed on the command line so this single
 * script can be driven by an external parameter sweep -- see
 * run_experiments.sh.
 *
 * Example:
 *   ./ns3 run "scratch/tcp-aqm-comparison --tcpVariant=TcpCubic \
 *              --queueDisc=FqCoDel --nFlows=4 --runNumber=3"
 *
 * Output files (written into the current working directory, or
 * wherever --outputPrefix points):
 *   <prefix>-cwnd-flowN.tr   congestion window trace for TCP flow N
 *   <prefix>-queue.tr        bottleneck queue-length trace over time
 *   <prefix>-flowmon.xml     full per-flow ns-3 FlowMonitor statistics
 *   <summaryFile>            one appended CSV row per run (default results.csv)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/error-model.h"

#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("TcpAqmComparison");

// ---------------------------------------------------------------------
// Globals used only for tracing. Kept small and explicit on purpose --
// this is a self-contained simulation script, not a reusable library.
// ---------------------------------------------------------------------
static std::vector<Ptr<OutputStreamWrapper>> g_cwndStreams;

static double            g_queueAreaPacketSeconds = 0.0;
static double            g_lastQueueVal           = 0.0;
static Time              g_lastQueueChangeTime    = Seconds (0.0);
static uint32_t          g_maxQueueLenPackets     = 0;
static Ptr<OutputStreamWrapper> g_queueStream;

// ---------------------------------------------------------------------
// Trace callbacks
// ---------------------------------------------------------------------
static void
CwndChange (Ptr<OutputStreamWrapper> stream, uint32_t oldCwnd, uint32_t newCwnd)
{
  *stream->GetStream () << Simulator::Now ().GetSeconds () << "\t" << newCwnd << std::endl;
}

static void
ConnectCwndTrace (uint32_t nodeId, Ptr<OutputStreamWrapper> stream)
{
  Config::ConnectWithoutContext (
      "/NodeList/" + std::to_string (nodeId) +
          "/$ns3::TcpL4Protocol/SocketList/0/CongestionWindow",
      MakeBoundCallback (&CwndChange, stream));
}

static void
QueueLengthChange (uint32_t oldVal, uint32_t newVal)
{
  Time now = Simulator::Now ();
  g_queueAreaPacketSeconds += g_lastQueueVal * (now - g_lastQueueChangeTime).GetSeconds ();
  g_lastQueueChangeTime = now;
  g_lastQueueVal = static_cast<double> (newVal);
  if (newVal > g_maxQueueLenPackets)
    {
      g_maxQueueLenPackets = newVal;
    }
  if (g_queueStream)
    {
      *g_queueStream->GetStream () << now.GetSeconds () << "\t" << newVal << std::endl;
    }
}

// ---------------------------------------------------------------------
// Jain's fairness index: (sum x_i)^2 / (n * sum x_i^2)
// ---------------------------------------------------------------------
static double
JainFairnessIndex (const std::vector<double> &x)
{
  if (x.empty ())
    {
      return 0.0;
    }
  double sum = 0.0, sumSq = 0.0;
  for (double v : x)
    {
      sum += v;
      sumSq += v * v;
    }
  if (sumSq == 0.0)
    {
      return 0.0;
    }
  return (sum * sum) / (static_cast<double> (x.size ()) * sumSq);
}

int
main (int argc, char *argv[])
{
  // -------------------------------------------------------------
  // 1. Parameters (all overridable from the command line)
  // -------------------------------------------------------------
  std::string tcpVariant      = "TcpNewReno";   // TcpNewReno | TcpCubic | TcpBbr | TcpVegas
  std::string queueDisc       = "PfifoFast";    // PfifoFast | Red | CoDel | FqCoDel
  uint32_t    nFlows          = 3;              // number of competing TCP bulk-send flows
  std::string accessBw        = "100Mbps";
  std::string accessDelay     = "1ms";
  std::string bottleneckBw    = "10Mbps";
  std::string bottleneckDelay = "20ms";
  uint32_t    queueSizePkts   = 100;            // bottleneck AQM queue capacity, in packets
  double      errorRate       = 0.0;            // random bottleneck packet-loss rate [0,1]
  std::string udpRate         = "500kbps";      // competing real-time-like UDP CBR flow
  uint32_t    udpPacketSize   = 200;            // bytes
  uint32_t    tcpPacketSize   = 1448;           // bytes (leaves room for headers under 1500 MTU)
  double      simTime         = 60.0;           // seconds of application traffic
  uint32_t    runNumber       = 1;              // RNG "Run" -- vary this for independent repeats
  bool        tracePcap       = false;
  std::string outputPrefix    = "tcp-aqm";
  std::string summaryFile     = "results.csv";

  CommandLine cmd (__FILE__);
  cmd.AddValue ("tcpVariant", "TcpNewReno|TcpCubic|TcpBbr|TcpVegas", tcpVariant);
  cmd.AddValue ("queueDisc", "PfifoFast|Red|CoDel|FqCoDel", queueDisc);
  cmd.AddValue ("nFlows", "Number of competing TCP bulk-send flows", nFlows);
  cmd.AddValue ("accessBw", "Access link data rate", accessBw);
  cmd.AddValue ("accessDelay", "Access link one-way delay", accessDelay);
  cmd.AddValue ("bottleneckBw", "Bottleneck link data rate", bottleneckBw);
  cmd.AddValue ("bottleneckDelay", "Bottleneck link one-way delay", bottleneckDelay);
  cmd.AddValue ("queueSizePkts", "Bottleneck AQM queue capacity (packets)", queueSizePkts);
  cmd.AddValue ("errorRate", "Random packet loss rate on the bottleneck [0,1]", errorRate);
  cmd.AddValue ("udpRate", "Competing UDP CBR flow rate", udpRate);
  cmd.AddValue ("simTime", "Application traffic duration (s)", simTime);
  cmd.AddValue ("runNumber", "RNG run number (vary for independent repeats)", runNumber);
  cmd.AddValue ("tracePcap", "Enable pcap capture on the bottleneck link", tracePcap);
  cmd.AddValue ("outputPrefix", "Prefix for this run's trace/FlowMonitor files", outputPrefix);
  cmd.AddValue ("summaryFile", "CSV file to append this run's summary row to", summaryFile);
  cmd.Parse (argc, argv);

  // -------------------------------------------------------------
  // 2. Random number configuration for independent repetitions.
  //    Same global seed, different "Run" -> statistically
  //    independent substreams (the standard ns-3 idiom).
  // -------------------------------------------------------------
  RngSeedManager::SetSeed (1);
  RngSeedManager::SetRun (runNumber);

  // -------------------------------------------------------------
  // 3. Validate and apply the TCP congestion-control algorithm
  // -------------------------------------------------------------
  std::map<std::string, std::string> tcpMap = {
      {"TcpNewReno", "ns3::TcpNewReno"},
      {"TcpCubic", "ns3::TcpCubic"},
      {"TcpBbr", "ns3::TcpBbr"},
      {"TcpVegas", "ns3::TcpVegas"},
  };
  auto tcpIt = tcpMap.find (tcpVariant);
  NS_ABORT_MSG_IF (tcpIt == tcpMap.end (),
                    "Unknown --tcpVariant '" << tcpVariant
                                              << "'. Valid: TcpNewReno, TcpCubic, TcpBbr, TcpVegas");
  Config::SetDefault ("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue (TypeId::LookupByName (tcpIt->second)));

  // Segment size and generous send/receive buffers so the socket
  // itself is never the bottleneck -- only the network should be.
  Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (tcpPacketSize));
  Config::SetDefault ("ns3::TcpSocket::SndBufSize", UintegerValue (1 << 22));
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize", UintegerValue (1 << 22));

  // -------------------------------------------------------------
  // 4. Nodes: 2 routers + (nFlows + 1) leaves on each side.
  //    The "+1" leaf pair carries the competing UDP flow.
  // -------------------------------------------------------------
  uint32_t nLeaves = nFlows + 1;

  NodeContainer routers;
  routers.Create (2);
  Ptr<Node> r0 = routers.Get (0);
  Ptr<Node> r1 = routers.Get (1);

  NodeContainer leftLeaves, rightLeaves;
  leftLeaves.Create (nLeaves);
  rightLeaves.Create (nLeaves);

  // -------------------------------------------------------------
  // 5. Links
  // -------------------------------------------------------------
  PointToPointHelper accessLink;
  accessLink.SetDeviceAttribute ("DataRate", StringValue (accessBw));
  accessLink.SetChannelAttribute ("Delay", StringValue (accessDelay));
  accessLink.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("1000p"));

  PointToPointHelper bottleneckLink;
  bottleneckLink.SetDeviceAttribute ("DataRate", StringValue (bottleneckBw));
  bottleneckLink.SetChannelAttribute ("Delay", StringValue (bottleneckDelay));
  // The NetDevice's own queue is kept minimal; the *effective*
  // buffering at the bottleneck is provided by the AQM queue disc
  // installed below (step 7), which is the thing we actually vary.
  bottleneckLink.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("10p"));

  std::vector<NetDeviceContainer> leftAccessDevices (nLeaves), rightAccessDevices (nLeaves);
  for (uint32_t i = 0; i < nLeaves; ++i)
    {
      leftAccessDevices[i] = accessLink.Install (leftLeaves.Get (i), r0);
      rightAccessDevices[i] = accessLink.Install (r1, rightLeaves.Get (i));
    }
  NetDeviceContainer bottleneckDevices = bottleneckLink.Install (r0, r1);

  // -------------------------------------------------------------
  // 6. Internet stack
  // -------------------------------------------------------------
  InternetStackHelper stack;
  stack.Install (routers);
  stack.Install (leftLeaves);
  stack.Install (rightLeaves);

  // -------------------------------------------------------------
  // 7. Active Queue Management on the bottleneck link
  // -------------------------------------------------------------
  std::string qs = std::to_string (queueSizePkts) + "p";
  TrafficControlHelper tch;
  if (queueDisc == "PfifoFast")
    {
      tch.SetRootQueueDisc ("ns3::PfifoFastQueueDisc", "MaxSize", StringValue (qs));
    }
  else if (queueDisc == "Red")
    {
      tch.SetRootQueueDisc ("ns3::RedQueueDisc");
      Config::SetDefault ("ns3::RedQueueDisc::MaxSize", StringValue (qs));
      Config::SetDefault ("ns3::RedQueueDisc::MinTh", DoubleValue (queueSizePkts * 0.2));
      Config::SetDefault ("ns3::RedQueueDisc::MaxTh", DoubleValue (queueSizePkts * 0.6));
    }
  else if (queueDisc == "CoDel")
    {
      tch.SetRootQueueDisc ("ns3::CoDelQueueDisc");
      Config::SetDefault ("ns3::CoDelQueueDisc::MaxSize", StringValue (qs));
    }
  else if (queueDisc == "FqCoDel")
    {
      tch.SetRootQueueDisc ("ns3::FqCoDelQueueDisc");
      Config::SetDefault ("ns3::FqCoDelQueueDisc::MaxSize", StringValue (qs));
    }
  else
    {
      NS_ABORT_MSG ("Unknown --queueDisc '" << queueDisc
                                             << "'. Valid: PfifoFast, Red, CoDel, FqCoDel");
    }
  QueueDiscContainer qdiscs = tch.Install (bottleneckDevices);

  // -------------------------------------------------------------
  // 8. Optional random loss on the bottleneck -- models a lossy WAN
  //    hop / radio link, orthogonal to congestion-induced loss.
  // -------------------------------------------------------------
  if (errorRate > 0.0)
    {
      Ptr<RateErrorModel> em0 = CreateObject<RateErrorModel> ();
      em0->SetAttribute ("ErrorUnit", EnumValue (RateErrorModel::ERROR_UNIT_PACKET));
      em0->SetAttribute ("ErrorRate", DoubleValue (errorRate));
      bottleneckDevices.Get (1)->SetAttribute ("ReceiveErrorModel", PointerValue (em0));

      Ptr<RateErrorModel> em1 = CreateObject<RateErrorModel> ();
      em1->SetAttribute ("ErrorUnit", EnumValue (RateErrorModel::ERROR_UNIT_PACKET));
      em1->SetAttribute ("ErrorRate", DoubleValue (errorRate));
      bottleneckDevices.Get (0)->SetAttribute ("ReceiveErrorModel", PointerValue (em1));
    }

  // -------------------------------------------------------------
  // 9. IP addressing -- one /24 per access link, one for the
  //    bottleneck link.
  // -------------------------------------------------------------
  Ipv4AddressHelper addr;
  std::vector<Ipv4InterfaceContainer> leftIfaces (nLeaves), rightIfaces (nLeaves);

  for (uint32_t i = 0; i < nLeaves; ++i)
    {
      std::ostringstream leftNet, rightNet;
      leftNet << "10.1." << i << ".0";
      rightNet << "10.2." << i << ".0";
      addr.SetBase (leftNet.str ().c_str (), "255.255.255.0");
      leftIfaces[i] = addr.Assign (leftAccessDevices[i]);
      addr.SetBase (rightNet.str ().c_str (), "255.255.255.0");
      rightIfaces[i] = addr.Assign (rightAccessDevices[i]);
    }
  addr.SetBase ("10.3.0.0", "255.255.255.0");
  addr.Assign (bottleneckDevices);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // -------------------------------------------------------------
  // 10. Applications
  //     Flows 0..nFlows-1 : long-lived TCP bulk transfers (the flows
  //                         under study).
  //     Flow  nFlows      : a UDP CBR "real-time" cross-traffic flow,
  //                         used to check whether the chosen AQM
  //                         protects latency-sensitive traffic from
  //                         the queueing built up by the TCP flows.
  // -------------------------------------------------------------
  ApplicationContainer sinkApps, sourceApps;
  const uint16_t tcpBasePort = 9000;
  const uint16_t udpPort = 8000;
  double appStart = 1.0;

  AsciiTraceHelper asciiTraceHelper;
  g_cwndStreams.resize (nFlows);

  for (uint32_t i = 0; i < nFlows; ++i)
    {
      uint16_t port = tcpBasePort + i;
      Address sinkAddr (InetSocketAddress (rightIfaces[i].GetAddress (1), port));

      PacketSinkHelper sinkHelper ("ns3::TcpSocketFactory",
                                    InetSocketAddress (Ipv4Address::GetAny (), port));
      sinkApps.Add (sinkHelper.Install (rightLeaves.Get (i)));

      BulkSendHelper bulkHelper ("ns3::TcpSocketFactory", sinkAddr);
      bulkHelper.SetAttribute ("MaxBytes", UintegerValue (0)); // 0 = send for the app's lifetime
      bulkHelper.SetAttribute ("SendSize", UintegerValue (tcpPacketSize));
      ApplicationContainer app = bulkHelper.Install (leftLeaves.Get (i));
      double start = appStart + 0.01 * i; // small stagger to desynchronise SYNs
      app.Start (Seconds (start));
      app.Stop (Seconds (appStart + simTime));
      sourceApps.Add (app);

      // Congestion-window tracing for this flow
      std::string cwndFile = outputPrefix + "-cwnd-flow" + std::to_string (i) + ".tr";
      g_cwndStreams[i] = asciiTraceHelper.CreateFileStream (cwndFile);
      Simulator::Schedule (Seconds (start + 0.05), &ConnectCwndTrace,
                            leftLeaves.Get (i)->GetId (), g_cwndStreams[i]);
    }

  // Competing UDP CBR flow
  {
    Address udpSinkAddr (InetSocketAddress (rightIfaces[nFlows].GetAddress (1), udpPort));
    PacketSinkHelper udpSinkHelper ("ns3::UdpSocketFactory",
                                     InetSocketAddress (Ipv4Address::GetAny (), udpPort));
    sinkApps.Add (udpSinkHelper.Install (rightLeaves.Get (nFlows)));

    OnOffHelper onoff ("ns3::UdpSocketFactory", udpSinkAddr);
    onoff.SetAttribute ("DataRate", StringValue (udpRate));
    onoff.SetAttribute ("PacketSize", UintegerValue (udpPacketSize));
    // "OnTime" far longer than the simulation -> continuous CBR traffic
    onoff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1e6]"));
    onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer app = onoff.Install (leftLeaves.Get (nFlows));
    app.Start (Seconds (appStart));
    app.Stop (Seconds (appStart + simTime));
    sourceApps.Add (app);
  }

  sinkApps.Start (Seconds (0.5));
  sinkApps.Stop (Seconds (appStart + simTime + 5.0));

  // -------------------------------------------------------------
  // 11. Bottleneck queue-occupancy tracing (time-weighted average
  //     and peak, in packets)
  // -------------------------------------------------------------
  g_queueStream = asciiTraceHelper.CreateFileStream (outputPrefix + "-queue.tr");
  g_lastQueueChangeTime = Seconds (appStart);
  qdiscs.Get (0)->TraceConnectWithoutContext ("PacketsInQueue", MakeCallback (&QueueLengthChange));

  // -------------------------------------------------------------
  // 12. Optional pcap capture on the bottleneck (off by default --
  //     leave this off during large automated sweeps: it is slow
  //     and produces large files).
  // -------------------------------------------------------------
  if (tracePcap)
    {
      bottleneckLink.EnablePcapAll (outputPrefix + "-bottleneck");
    }

  // -------------------------------------------------------------
  // 13. FlowMonitor
  // -------------------------------------------------------------
  FlowMonitorHelper flowmonHelper;
  Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll ();

  double stopTime = appStart + simTime + 5.0;
  Simulator::Stop (Seconds (stopTime));
  Simulator::Run ();

  // Finalise the time-weighted queue-occupancy integral up to stopTime.
  g_queueAreaPacketSeconds +=
      g_lastQueueVal * (Seconds (stopTime) - g_lastQueueChangeTime).GetSeconds ();
  double avgQueueLenPkts = g_queueAreaPacketSeconds / simTime;

  // -------------------------------------------------------------
  // 14. Metrics from FlowMonitor
  // -------------------------------------------------------------
  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier =
      DynamicCast<Ipv4FlowClassifier> (flowmonHelper.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

  std::vector<double> tcpThroughputs; // for Jain's fairness index
  double tcpDelaySum = 0, tcpJitterSum = 0, tcpThroughputSum = 0;
  uint64_t tcpTxTotal = 0, tcpRxTotal = 0;
  uint32_t tcpFlowCount = 0;

  double udpThroughput = 0, udpDelay = 0, udpJitter = 0, udpPdr = 0;

  for (const auto &kv : stats)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (kv.first);
      const FlowMonitor::FlowStats &s = kv.second;
      double duration = (s.timeLastRxPacket - s.timeFirstTxPacket).GetSeconds ();
      double throughputMbps = duration > 0 ? (s.rxBytes * 8.0 / duration / 1e6) : 0.0;

      bool isTcp = (t.destinationPort >= tcpBasePort && t.destinationPort < tcpBasePort + nFlows);
      bool isUdp = (t.destinationPort == udpPort);

      if (isTcp)
        {
          tcpFlowCount++;
          tcpThroughputs.push_back (throughputMbps);
          tcpThroughputSum += throughputMbps;
          tcpTxTotal += s.txPackets;
          tcpRxTotal += s.rxPackets;
          if (s.rxPackets > 0)
            {
              tcpDelaySum += (s.delaySum.GetSeconds () / s.rxPackets) * 1000.0; // ms
            }
          if (s.rxPackets > 1)
            {
              tcpJitterSum += (s.jitterSum.GetSeconds () / (s.rxPackets - 1)) * 1000.0; // ms
            }
        }
      else if (isUdp)
        {
          udpThroughput = throughputMbps;
          udpPdr = s.txPackets > 0 ? (100.0 * s.rxPackets / s.txPackets) : 0.0;
          if (s.rxPackets > 0)
            {
              udpDelay = (s.delaySum.GetSeconds () / s.rxPackets) * 1000.0;
            }
          if (s.rxPackets > 1)
            {
              udpJitter = (s.jitterSum.GetSeconds () / (s.rxPackets - 1)) * 1000.0;
            }
        }
    }

  double tcpAvgThroughput = tcpFlowCount ? tcpThroughputSum / tcpFlowCount : 0.0;
  double tcpAvgDelay = tcpFlowCount ? tcpDelaySum / tcpFlowCount : 0.0;
  double tcpAvgJitter = tcpFlowCount ? tcpJitterSum / tcpFlowCount : 0.0;
  double tcpPdr = tcpTxTotal ? (100.0 * static_cast<double> (tcpRxTotal) / tcpTxTotal) : 0.0;
  double tcpLossPct = tcpTxTotal ? (100.0 - tcpPdr) : 0.0;
  double fairness = JainFairnessIndex (tcpThroughputs);

  // -------------------------------------------------------------
  // 15. Console summary (useful for a quick sanity check)
  // -------------------------------------------------------------
  std::cout << std::fixed << std::setprecision (3);
  std::cout << "=== " << tcpVariant << " / " << queueDisc << " / nFlows=" << nFlows
            << " / run=" << runNumber << " ===\n";
  std::cout << "TCP  avg throughput : " << tcpAvgThroughput << " Mbps\n";
  std::cout << "TCP  avg delay      : " << tcpAvgDelay << " ms\n";
  std::cout << "TCP  avg jitter     : " << tcpAvgJitter << " ms\n";
  std::cout << "TCP  PDR            : " << tcpPdr << " %\n";
  std::cout << "TCP  Jain fairness  : " << fairness << "\n";
  std::cout << "UDP  throughput     : " << udpThroughput << " Mbps\n";
  std::cout << "UDP  delay          : " << udpDelay << " ms\n";
  std::cout << "UDP  jitter         : " << udpJitter << " ms\n";
  std::cout << "UDP  PDR            : " << udpPdr << " %\n";
  std::cout << "Bottleneck avg queue: " << avgQueueLenPkts << " pkts (peak " << g_maxQueueLenPackets
             << ")\n";

  // -------------------------------------------------------------
  // 16. Per-run FlowMonitor XML (detailed, per-flow) + a single
  //     appended row to the sweep-wide summary CSV.
  // -------------------------------------------------------------
  monitor->SerializeToXmlFile (outputPrefix + "-flowmon.xml", true, true);

  {
    std::ofstream csv (summaryFile, std::ios::app);
    csv << tcpVariant << "," << queueDisc << "," << nFlows << ","
        << DataRate (bottleneckBw).GetBitRate () / 1e6 << "," << Time (bottleneckDelay).GetMilliSeconds ()
        << "," << errorRate << "," << runNumber << "," << tcpAvgThroughput << "," << tcpAvgDelay
        << "," << tcpAvgJitter << "," << tcpPdr << "," << fairness << "," << udpThroughput << ","
        << udpDelay << "," << udpJitter << "," << udpPdr << "," << avgQueueLenPkts << ","
        << g_maxQueueLenPackets << "," << tcpLossPct << std::endl;
  }

  Simulator::Destroy ();
  return 0;
}
