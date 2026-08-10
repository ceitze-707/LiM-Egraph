// Debug two-gate case that fails
#include "utils.h"
#include "mockturtle/algorithms/simulation.hpp"
#include <cstdlib>

int main() {
    xmg_network xmg;
    auto pi0 = xmg.create_pi();
    auto pi1 = xmg.create_pi();
    auto pi2 = xmg.create_pi();
    auto g0 = xmg.create_xor3(pi0, pi1, pi2);       // XOR3(a, b, c)
    auto g1 = xmg.create_xor3(!pi0, pi1, g0);        // XOR3(!a, b, g0)

    cout << "num_gates=" << xmg.num_gates() << " num_pis=" << xmg.num_pis() << endl;
    cout << "g0 complemented=" << xmg.is_complemented(g0) << endl;
    cout << "g1 complemented=" << xmg.is_complemented(g1) << endl;

    cout << "\n=== Original gate dump ===" << endl;
    xmg.foreach_gate([&](auto const& n) {
        cout << "Gate idx=" << xmg.node_to_index(n)
             << " xor3=" << xmg.is_xor3(n) << " maj=" << xmg.is_maj(n) << endl;
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto child = xmg.get_node(f);
            cout << "  fanin: node=" << xmg.node_to_index(child)
                 << " comp=" << xmg.is_complemented(f)
                 << " isPI=" << xmg.is_pi(child)
                 << " isConst=" << xmg.is_constant(child) << endl;
        });
    });

    // Create POs
    xmg.create_po(g0);
    xmg.create_po(g1);

    // Export
    string sf = "two_test.txt";
    ExportXmgForEgraphNumbered(xmg, sf);
    cout << "\n--- exported ---" << endl;
    ifstream fin(sf);
    string line;
    while (getline(fin, line)) cout << line << endl;

    // Rebuild
    xmg_network reb = RebuildXmgFromNumbered(sf);
    cout << "\n=== Rebuilt gate dump ===" << endl;
    reb.foreach_gate([&](auto const& n) {
        cout << "Gate idx=" << reb.node_to_index(n) << endl;
        reb.foreach_fanin(n, [&](auto const& f) {
            auto child = reb.get_node(f);
            cout << "  fanin: node=" << reb.node_to_index(child)
                 << " comp=" << reb.is_complemented(f) << endl;
        });
    });

    // Compare
    cout << "\n--- Comparison ---" << endl;
    int nPI = 3;
    bool all_ok = true;
    for (int pat = 0; pat < 8; pat++) {
        vector<bool> inputs(3);
        inputs[0] = (pat>>2)&1; inputs[1] = (pat>>1)&1; inputs[2] = pat&1;
        auto o1 = simulate<bool>(xmg, default_simulator<bool>(inputs));
        auto o2 = simulate<bool>(reb, default_simulator<bool>(inputs));
        bool ok = o1 == o2;
        if (!ok) all_ok = false;
        cout << inputs[0] << inputs[1] << inputs[2]
             << " orig:" << o1[0] << o1[1] << " reb:" << o2[0] << o2[1]
             << (ok ? " OK" : " FAIL") << endl;
    }
    cout << (all_ok ? "ALL OK" : "BROKEN") << endl;
    return 0;
}
