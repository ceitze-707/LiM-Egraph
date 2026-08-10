// egraph_v2 — Full-circuit e-graph optimization (Verilog in → Verilog out)
// Avoids sub-netlist extraction entirely. No TI/PI boundary issues.
#include "mockturtle/mockturtle.hpp"
#include "lorina/verilog.hpp"
#include "mockturtle/algorithms/simulation.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <ctime>
#include <string>
using namespace std;
using namespace mockturtle;
#include "egraph_helpers.h"
#include "sexpr_parser.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: egraph_opt <input.v> [output.v] [--rules=N]" << endl;
        cerr << "  --rules=0  pass-through (verify pipeline only)" << endl;
        cerr << "  --rules=4  commutativity only (safe)" << endl;
        cerr << "  --rules=18 full rules (default)" << endl;
        return 1;
    }

    string infile = argv[1];
    string outfile = (argc >= 3 && argv[2][0] != '-') ? argv[2] : (infile + ".opt.v");

    srand((unsigned)time(0));

    // 1. Read input Verilog
    cout << "[1] Reading " << infile << "..." << endl;
    xmg_network xmg;
    auto result = lorina::read_verilog(infile, mockturtle::verilog_reader(xmg));
    if (result != lorina::return_code::success) {
        cerr << "ERROR: Cannot read Verilog file" << endl;
        return 1;
    }
    cout << "     " << xmg.num_gates() << " gates, " << xmg.num_pis() << " PIs, " << xmg.num_pos() << " POs" << endl;

    // 2. Export as numbered DAG
    cout << "[2] Exporting to numbered DAG..." << endl;
    string dag_file = outfile + ".dag";
    ExportXmgForEgraphNumbered(xmg, dag_file);

    // 3. Run Rust e-graph optimizer
    cout << "[3] Running e-graph optimization..." << endl;
    string opt_dag = outfile + ".opt.dag";
    string cmd = "D:/DESKBOOK/0809/egraph_v2/target/release/xmg_egraph.exe " + dag_file + " " + opt_dag;
    int ret = system(cmd.c_str());
    if (ret != 0) {
        cerr << "ERROR: e-graph optimizer failed" << endl;
        return 1;
    }

    // 4. Rebuild XMG from optimized DAG
    cout << "[4] Rebuilding optimized XMG..." << endl;
    xmg_network xmg_opt = RebuildXmgFromNumbered(opt_dag);
    cout << "     " << xmg_opt.num_gates() << " gates, " << xmg_opt.num_pis() << " PIs, " << xmg_opt.num_pos() << " POs" << endl;

    // 5. Verify equivalence
    cout << "[5] Verifying equivalence..." << endl;
    int nPI = (int)xmg.num_pis();
    int nPO = (int)xmg.num_pos();
    if (nPI != (int)xmg_opt.num_pis() || nPO != (int)xmg_opt.num_pos()) {
        cerr << "ERROR: PI/PO count mismatch after optimization!" << endl;
        return 1;
    }

    const int N_PATTERNS = 2000;
    bool equiv = true;
    for (int p = 0; p < N_PATTERNS && equiv; p++) {
        vector<bool> inputs(nPI);
        for (int i = 0; i < nPI; i++) inputs[i] = rand() % 2;
        auto out_orig = simulate<bool>(xmg, default_simulator<bool>(inputs));
        auto out_opt  = simulate<bool>(xmg_opt, default_simulator<bool>(inputs));
        if (out_orig != out_opt) {
            equiv = false;
            cout << "     MISMATCH at pattern " << p << endl;
        }
    }

    if (!equiv) {
        cerr << "FAIL: optimized circuit is NOT functionally equivalent!" << endl;
        return 1;
    }
    cout << "     PASS — " << N_PATTERNS << "/" << N_PATTERNS << " patterns match" << endl;

    // 6. Write optimized Verilog
    cout << "[6] Writing " << outfile << "..." << endl;
    write_verilog(xmg_opt, outfile);

    // 7. Summary
    int saved = (int)xmg.num_gates() - (int)xmg_opt.num_gates();
    double pct = 100.0 * saved / xmg.num_gates();
    cout << "\n========================================" << endl;
    cout << "  Original: " << xmg.num_gates() << " gates" << endl;
    cout << "  Optimized: " << xmg_opt.num_gates() << " gates" << endl;
    cout << "  Saved: " << saved << " gates (" << (saved >= 0 ? "+" : "") << pct << "%)" << endl;
    cout << "  Equivalence: VERIFIED (" << N_PATTERNS << " patterns)" << endl;
    cout << "  Output: " << outfile << endl;
    cout << "========================================" << endl;

    return 0;
}
