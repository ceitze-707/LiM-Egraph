// Standalone test: export -> rebuild -> simulate comparison
#include "utils.h"
#include "mockturtle/algorithms/simulation.hpp"
#include <cstdlib>
#include <ctime>

int main() {
    srand((unsigned)time(0));

    // Read a test benchmark
    string bench = "router.v";
    cout << "Reading " << bench << "..." << endl;
    xmg_network xmg;
    lorina::read_verilog(bench, verilog_reader(xmg));
    cout << "Original: " << xmg.num_gates() << " gates, " << xmg.num_pis() << " PIs, " << xmg.num_pos() << " POs" << endl;

    // Export
    string sf = "test_export.txt";
    ExportXmgForEgraphNumbered(xmg, sf);
    cout << "Exported to " << sf << endl;

    // Rebuild
    xmg_network rebuilt = RebuildXmgFromNumbered(sf);
    cout << "Rebuilt: " << rebuilt.num_gates() << " gates, " << rebuilt.num_pis() << " PIs, " << rebuilt.num_pos() << " POs" << endl;

    // Simulate and compare
    int nPI = (int)xmg.num_pis();
    int nPO = (int)xmg.num_pos();
    if (nPI != (int)rebuilt.num_pis() || nPO != (int)rebuilt.num_pos()) {
        cout << "PI/PO count mismatch!" << endl;
        return 1;
    }

    bool equiv = true;
    for (int p = 0; p < 1000 && equiv; p++) {
        vector<bool> inputs(nPI);
        for (int i = 0; i < nPI; i++) inputs[i] = rand() % 2;
        auto out_ori = simulate<bool>(xmg, default_simulator<bool>(inputs));
        auto out_reb = simulate<bool>(rebuilt, default_simulator<bool>(inputs));
        if (out_ori != out_reb) {
            equiv = false;
            cout << "MISMATCH at pattern " << p << endl;
            cout << "  inputs:  "; for (int k=0;k<min(5,nPI);k++) cout << inputs[k]; cout << endl;
            cout << "  orig PO: "; for (auto v:out_ori) cout << v; cout << endl;
            cout << "  reb  PO: "; for (auto v:out_reb) cout << v; cout << endl;
            cout << "  nPI=" << nPI << " nPO=" << nPO << endl;
        }
    }

    if (equiv)
        cout << "ALL 1000 PATTERNS MATCH -- pipeline is CORRECT" << endl;
    else
        cout << "PIPELINE IS BROKEN" << endl;

    return 0;
}
