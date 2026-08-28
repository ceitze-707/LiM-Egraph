// Minimal test: create tiny XMG, export, rebuild, compare
#include "utils.h"
#include "mockturtle/algorithms/simulation.hpp"
#include <cstdlib>

int main() {
    srand(42);

    // Create a tiny circuit: two XOR3 gates
    xmg_network xmg;
    auto pi0 = xmg.create_pi();
    auto pi1 = xmg.create_pi();
    auto pi2 = xmg.create_pi();
    auto g0 = xmg.create_xor3(pi0, pi1, pi2);      // XOR3(a, b, c)
    auto g1 = xmg.create_xor3(!pi0, pi1, g0);       // XOR3(!a, b, g0) — complemented fanin
    xmg.create_po(g0);
    xmg.create_po(g1);

    cout << "Original: " << xmg.num_gates() << " gates, " << xmg.num_pis() << " PIs, " << xmg.num_pos() << " POs\n";

    // Export
    string sf = "tiny_test.txt";
    ExportXmgForEgraphNumbered(xmg, sf);

    // Show exported file
    cout << "--- exported file ---\n";
    ifstream fin(sf);
    string line;
    while (getline(fin, line)) cout << line << "\n";
    fin.close();
    cout << "--- end ---\n";

    // Rebuild
    xmg_network rebuilt = RebuildXmgFromNumbered(sf);
    cout << "Rebuilt: " << rebuilt.num_gates() << " gates, " << rebuilt.num_pis() << " PIs, " << rebuilt.num_pos() << " POs\n";

    // Simulate ALL 8 input combos
    int nPI = 3;
    cout << "\nExhaustive simulation:\n";
    cout << "a b c | orig_PO0 orig_PO1 | reb_PO0 reb_PO1 | match?\n";
    bool all_ok = true;
    for (int pat = 0; pat < 8; pat++) {
        vector<bool> inputs(3);
        inputs[0] = (pat >> 2) & 1;
        inputs[1] = (pat >> 1) & 1;
        inputs[2] = pat & 1;

        auto out_ori = simulate<bool>(xmg, default_simulator<bool>(inputs));
        auto out_reb = simulate<bool>(rebuilt, default_simulator<bool>(inputs));
        bool ok = (out_ori == out_reb);
        if (!ok) all_ok = false;

        cout << inputs[0] << " " << inputs[1] << " " << inputs[2]
             << " | " << out_ori[0] << "      " << out_ori[1]
             << "       | " << out_reb[0] << "      " << out_reb[1]
             << "       | " << (ok ? "OK" : "FAIL") << "\n";
    }
    cout << (all_ok ? "\nALL MATCH" : "\nFAIL") << "\n";
    return 0;
}
