#include "gatetime/incremental.hpp"

#include <cmath>
#include <functional>
#include <queue>
#include <stdexcept>

namespace gt {

IncrementalTimer::IncrementalTimer(const Netlist& nl, const std::vector<NodeId>& order,
                                   const DelayModel& model, const TimingOptions& opt)
    : nl_(&nl), order_(&order), pos_(nl.size()), inputArrival_(opt.inputArrival) {
    res_ = analyzeTiming(nl, order, model, opt);  // throws if `order` does not cover nl
    for (std::size_t i = 0; i < order.size(); ++i) pos_[order[i]] = static_cast<int>(i);
    fwdQueued_.assign(nl.size(), 0);
    bwdQueued_.assign(nl.size(), 0);
    touched_.assign(nl.size(), 0);
}

void IncrementalTimer::setDelay(NodeId node, double delay) {
    if (node < 0 || static_cast<std::size_t>(node) >= nl_->size()) {
        throw std::invalid_argument("IncrementalTimer::setDelay: node out of range");
    }
    if (!std::isfinite(delay) || delay < 0.0) {
        throw std::invalid_argument("IncrementalTimer::setDelay: delay must be finite and >= 0");
    }
    if (res_.delay[node] == delay) return;
    res_.delay[node] = delay;
    dirty_.push_back(node);
}

std::size_t IncrementalTimer::update() {
    if (dirty_.empty()) return 0;

    // Forward wave goes earliest-position first, backward wave latest-position first, so
    // a node is evaluated only after everything it depends on has settled.
    std::priority_queue<int, std::vector<int>, std::greater<int>> fwd;
    std::priority_queue<int> bwd;
    std::vector<NodeId> changed;  // nodes whose arrival or required time moved

    auto markChanged = [&](NodeId u) {
        if (!touched_[u]) {
            touched_[u] = 1;
            changed.push_back(u);
        }
    };
    auto queueFwd = [&](NodeId u) {
        if (!fwdQueued_[u]) {
            fwdQueued_[u] = 1;
            fwd.push(pos_[u]);
        }
    };
    auto queueBwd = [&](NodeId u) {
        if (!bwdQueued_[u]) {
            bwdQueued_[u] = 1;
            bwd.push(pos_[u]);
        }
    };

    // A changed delay at u changes u's own arrival, and the required time of u's fanins
    // (RAT(f) = min over fanout v of RAT(v) - delay(v)).
    for (NodeId u : dirty_) {
        queueFwd(u);
        for (NodeId f : nl_->gates[u].fanin) queueBwd(f);
    }
    dirty_.clear();

    std::size_t visited = 0;

    while (!fwd.empty()) {
        const NodeId u = (*order_)[fwd.top()];
        fwd.pop();
        fwdQueued_[u] = 0;
        ++visited;

        NodeId pred;
        const double at = arrivalOf(*nl_, u, res_.arrival, res_.delay, inputArrival_, pred);
        res_.pred[u] = pred;
        if (at != res_.arrival[u]) {  // unchanged arrival: nothing downstream can change
            res_.arrival[u] = at;
            markChanged(u);
            for (NodeId v : nl_->gates[u].fanout) queueFwd(v);
        }
    }

    while (!bwd.empty()) {
        const NodeId u = (*order_)[bwd.top()];
        bwd.pop();
        bwdQueued_[u] = 0;
        ++visited;

        const double rat = requiredOf(*nl_, u, res_.required, res_.delay, res_.endpointRequired);
        if (rat != res_.required[u]) {
            res_.required[u] = rat;
            markChanged(u);
            for (NodeId f : nl_->gates[u].fanin) queueBwd(f);
        }
    }

    for (NodeId u : changed) {
        res_.slack[u] = res_.required[u] - res_.arrival[u];
        touched_[u] = 0;
    }
    summarizeEndpoints(res_);
    return visited;
}

}  // namespace gt
