#pragma once
#include <cstddef>
#include <vector>

#include "gatetime/delay_model.hpp"
#include "gatetime/netlist.hpp"
#include "gatetime/timing.hpp"

namespace gt {

// Incremental static timing: change the delay of individual gates and re-time only
// the part of the circuit that can be affected, instead of the whole netlist.
//
// After setDelay(u, d):
//   * arrival times can only change in u's fanout cone; propagation stops at any node
//     whose arrival time comes out unchanged;
//   * required times can only change in u's fanin cone, with the same early exit;
//   * slack is refreshed only where arrival or required time moved.
// Results are bit-for-bit what a full analyzeTiming() with the new delays would give
// (both use the same arrivalOf / requiredOf), which the tests check on random edits.
//
// The clock period is fixed when the timer is built (the explicit period, or the
// initial smallest feasible period if none was given).
class IncrementalTimer {
public:
    // `order` is the topological order of an acyclic netlist. Both `nl` and `order`
    // must outlive the timer; the netlist structure must not change.
    IncrementalTimer(const Netlist& nl, const std::vector<NodeId>& order, const DelayModel& model,
                     const TimingOptions& opt);

    // Queues a new delay for `node`. Throws std::invalid_argument for an out-of-range
    // node or a delay that is negative or not finite. Nothing is recomputed until update().
    void setDelay(NodeId node, double delay);

    // Applies all queued changes. Returns the number of node evaluations performed
    // (forward + backward); a full analysis performs 2 x nl.size().
    std::size_t update();

    std::size_t pendingChanges() const { return dirty_.size(); }

    // Valid and consistent after construction and after every update().
    const TimingResult& result() const { return res_; }

private:
    const Netlist*             nl_;
    const std::vector<NodeId>* order_;
    std::vector<int>           pos_;  // NodeId -> index in order
    double                     inputArrival_;
    TimingResult               res_;

    std::vector<NodeId> dirty_;  // nodes whose delay was changed since the last update()
    // Scratch, all-zero between calls.
    std::vector<char> fwdQueued_, bwdQueued_, touched_;
};

}  // namespace gt
