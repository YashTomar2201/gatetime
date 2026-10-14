#pragma once
#include <limits>
#include <optional>
#include <vector>

#include "gatetime/delay_model.hpp"
#include "gatetime/netlist.hpp"
#include "gatetime/topo.hpp"

namespace gt {

constexpr double kInf = std::numeric_limits<double>::infinity();

// Relative tolerance for "is this slack zero / negative". Forward and backward passes
// add and subtract the same delays in different orders, so a path that is exactly
// critical can show slack of ~1e-16 * magnitude. The absolute tolerance used is
// kTimeEps * max(1, |T|, largest endpoint arrival).
constexpr double kTimeEps = 1e-9;

struct TimingOptions {
    // Clock period T. If unset, T is the smallest period that meets timing: the largest
    // (arrival + required margin) over all endpoints, so worst slack is exactly 0.
    std::optional<double> period;
    double inputArrival = 0.0;  // arrival time at startpoints (primary inputs, flop clocks)
};

struct TimingResult {
    double period = 0.0;

    std::vector<double> delay;     // per node, from the delay model
    std::vector<double> arrival;   // AT: latest arrival (startpoints: inputArrival + delay)
    std::vector<double> required;  // RAT; +inf if no path leads to an endpoint
    std::vector<double> slack;     // RAT - AT; +inf for the same nodes
    std::vector<NodeId> pred;      // fanin on the worst path to this node; -1 at startpoints

    // Timing endpoints (primary outputs, then flop D nets) and the required time each
    // one imposes on itself: T for outputs, T - setup for flop D pins, +inf for nodes
    // that are not endpoints.
    std::vector<NodeId> endpoints;
    std::vector<double> endpointRequired;  // per node

    // Worst endpoint: smallest slack against its own constraint (lowest id on ties).
    // -1 if there are no endpoints.
    NodeId criticalEndpoint = -1;
    double criticalDelay = 0.0;          // arrival[criticalEndpoint]
    double worstSlack = kInf;            // min endpoint slack
    std::size_t violatingEndpoints = 0;  // endpoints with slack < -tolerance
    std::size_t unconstrained = 0;       // nodes with no path to any endpoint
    double tolerance = 0.0;              // absolute slack tolerance used for the counts
};

// `order` must be a valid topological order of every node (TopoResult::order of an
// acyclic netlist). Throws std::invalid_argument if it does not cover the netlist
// (e.g. a combinational loop).
TimingResult analyzeTiming(const Netlist& nl, const std::vector<NodeId>& order,
                           const DelayModel& model, const TimingOptions& opt);

// ---- Building blocks shared by the full and the incremental analysis ---------------
// Both use exactly these functions, which is why incremental updates reproduce a full
// recomputation bit for bit.

// Arrival time of `u` given final arrivals of its fanin; sets `pred` (-1 at startpoints).
// Ties between equally late fanins go to the lowest NodeId.
double arrivalOf(const Netlist& nl, NodeId u, const std::vector<double>& arrival,
                 const std::vector<double>& delay, double inputArrival, NodeId& pred);

// Required time of `u` given final required times of its fanout.
double requiredOf(const Netlist& nl, NodeId u, const std::vector<double>& required,
                  const std::vector<double>& delay, const std::vector<double>& endpointRequired);

// Fills criticalEndpoint, criticalDelay, worstSlack and violatingEndpoints from
// arrival / endpointRequired / endpoints / tolerance.
void summarizeEndpoints(TimingResult& r);

// Absolute tolerance for a given period and set of arrivals (see kTimeEps).
double slackTolerance(double period, const std::vector<NodeId>& endpoints,
                      const std::vector<double>& arrival);

}  // namespace gt
