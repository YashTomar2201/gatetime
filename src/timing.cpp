#include "gatetime/timing.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gt {

double arrivalOf(const Netlist& nl, NodeId u, const std::vector<double>& arrival,
                 const std::vector<double>& delay, double inputArrival, NodeId& pred) {
    const auto& fanin = nl.gates[u].fanin;
    if (fanin.empty()) {
        pred = -1;
        // Startpoint: the launch edge plus the pin's own delay (0 for a primary input,
        // clock-to-Q for a flop output).
        return inputArrival + delay[u];
    }
    NodeId best = fanin.front();
    for (NodeId f : fanin) {
        if (arrival[f] > arrival[best] || (arrival[f] == arrival[best] && f < best)) best = f;
    }
    pred = best;
    return arrival[best] + delay[u];
}

double requiredOf(const Netlist& nl, NodeId u, const std::vector<double>& required,
                  const std::vector<double>& delay, const std::vector<double>& endpointRequired) {
    // A node that is an endpoint *and* has fanout gets the min of both constraints.
    // +inf - delay stays +inf, so unconstrained nodes propagate +inf naturally.
    double r = endpointRequired[u];
    for (NodeId v : nl.gates[u].fanout) r = std::min(r, required[v] - delay[v]);
    return r;
}

double slackTolerance(double period, const std::vector<NodeId>& endpoints,
                      const std::vector<double>& arrival) {
    double scale = std::max(1.0, std::fabs(period));
    for (NodeId e : endpoints) scale = std::max(scale, std::fabs(arrival[e]));
    return kTimeEps * scale;
}

void summarizeEndpoints(TimingResult& r) {
    r.criticalEndpoint = -1;
    r.criticalDelay = 0.0;
    r.worstSlack = kInf;
    r.violatingEndpoints = 0;
    r.tolerance = slackTolerance(r.period, r.endpoints, r.arrival);

    for (NodeId e : r.endpoints) {
        const double s = r.endpointRequired[e] - r.arrival[e];
        if (s < -r.tolerance) ++r.violatingEndpoints;
        if (s < r.worstSlack || (s == r.worstSlack && e < r.criticalEndpoint)) {
            r.worstSlack = s;
            r.criticalEndpoint = e;
            r.criticalDelay = r.arrival[e];
        }
    }
}

TimingResult analyzeTiming(const Netlist& nl, const std::vector<NodeId>& order,
                           const DelayModel& model, const TimingOptions& opt) {
    const std::size_t n = nl.gates.size();
    if (order.size() != n) {
        throw std::invalid_argument("analyzeTiming: topological order does not cover the netlist "
                                    "(combinational loop?)");
    }

    TimingResult r;
    r.delay.resize(n);
    r.arrival.assign(n, 0.0);
    r.required.assign(n, kInf);
    r.slack.assign(n, kInf);
    r.pred.assign(n, -1);
    r.endpointRequired.assign(n, kInf);
    r.endpoints = nl.endpoints();

    for (std::size_t i = 0; i < n; ++i) r.delay[i] = model.delay(nl, static_cast<NodeId>(i));

    // Forward pass: AT(u) = max over fanin f of AT(f), plus delay(u).
    for (NodeId u : order) r.arrival[u] = arrivalOf(nl, u, r.arrival, r.delay, opt.inputArrival, r.pred[u]);

    // Endpoint constraints. A primary output must arrive by T; a flop's D pin by T minus
    // the setup time. A net that is both takes the tighter one.
    const double setup = model.setupTime();
    std::vector<double> margin(n, 0.0);  // how much earlier than T the endpoint must arrive
    for (const Flop& f : nl.flops) margin[f.d] = setup;
    if (opt.period) {
        r.period = *opt.period;
    } else {
        // Smallest feasible period: every endpoint exactly meets its own requirement.
        double t = 0.0;
        bool any = false;
        for (NodeId e : r.endpoints) {
            const double need = r.arrival[e] + margin[e];
            t = any ? std::max(t, need) : need;
            any = true;
        }
        r.period = t;
    }
    for (NodeId e : r.endpoints) r.endpointRequired[e] = r.period - margin[e];

    // Backward pass: RAT(u) = min(endpoint requirement, min over fanout v of RAT(v) - delay(v)).
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const NodeId u = *it;
        r.required[u] = requiredOf(nl, u, r.required, r.delay, r.endpointRequired);
        r.slack[u] = r.required[u] - r.arrival[u];
    }

    for (std::size_t i = 0; i < n; ++i) {
        if (r.required[i] == kInf) ++r.unconstrained;
    }
    summarizeEndpoints(r);
    return r;
}

}  // namespace gt
