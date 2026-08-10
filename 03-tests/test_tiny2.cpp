// Debug: does Mockturtle push complements to output?
#include "utils.h"
#include <cstdlib>

int main() {
    xmg_network xmg;
    auto pi0 = xmg.create_pi();
    auto pi1 = xmg.create_pi();
    auto pi2 = xmg.create_pi();
    auto g0 = xmg.create_xor3(!pi0, pi1, pi2);  // XOR3(!a, b, c)

    cout << "=== Gate g0 = create_xor3(!pi0, pi1, pi2) ===" << endl;
    cout << "num_gates=" << xmg.num_gates() << " num_pis=" << xmg.num_pis() << endl;
    cout << "g0 complemented=" << xmg.is_complemented(g0) << endl;

    xmg.foreach_gate([&](auto const& n) {
        cout << "Gate node idx=" << xmg.node_to_index(n) << endl;
        cout << "  is_xor3=" << xmg.is_xor3(n) << " is_maj=" << xmg.is_maj(n) << endl;
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto child = xmg.get_node(f);
            cout << "  fanin: node=" << xmg.node_to_index(child)
                 << " complemented=" << xmg.is_complemented(f)
                 << " is_pi=" << xmg.is_pi(child) << endl;
        });
    });

    // Create PO
    xmg.create_po(g0);

    // Simulate
    #include "mockturtle/algorithms/simulation.hpp"
    // XOR3(!a,b,c) = !a xor b xor c
    // a=0,b=0,c=0 → 1 xor 0 xor 0 = 1
    // a=0,b=0,c=1 → 1 xor 0 xor 1 = 0
    auto out = simulate<bool>(xmg, default_simulator<bool>({false, false, false}));
    cout << "sim(0,0,0)=" << out[0] << " (expect 1)" << endl;
    out = simulate<bool>(xmg, default_simulator<bool>({false, false, true}));
    cout << "sim(0,0,1)=" << out[0] << " (expect 0)" << endl;

    // Export
    string sf = "tiny2_test.txt";
    ExportXmgForEgraphNumbered(xmg, sf);
    cout << "\n--- export ---" << endl;
    ifstream fin(sf);
    string line;
    while (getline(fin, line)) cout << line << endl;

    // Rebuild
    xmg_network reb = RebuildXmgFromNumbered(sf);
    cout << "\nRebuilt: gates=" << reb.num_gates() << " pis=" << reb.num_pis() << " pos=" << reb.num_pos() << endl;

    reb.foreach_gate([&](auto const& n) {
        cout << "Reb gate node idx=" << reb.node_to_index(n) << endl;
        reb.foreach_fanin(n, [&](auto const& f) {
            auto child = reb.get_node(f);
            cout << "  fanin: node=" << reb.node_to_index(child)
                 << " complemented=" << reb.is_complemented(f) << endl;
        });
    });

    // Compare
    cout << "\nComparison:" << endl;
    for (int pat = 0; pat < 8; pat++) {
        vector<bool> inputs = {(bool)(pat>>2), (bool)((pat>>1)&1), (bool)(pat&1)};
        auto o1 = simulate<bool>(xmg, default_simulator<bool>(inputs));
        auto o2 = simulate<bool>(reb, default_simulator<bool>(inputs));
        cout << "pat " << inputs[0] << inputs[1] << inputs[2]
             << ": orig=" << o1[0] << " reb=" << o2[0]
             << (o1==o2 ? " OK" : " FAIL") << endl;
    }
    return 0;
}
