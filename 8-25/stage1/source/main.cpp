// ============================================================
// [05-ADD] cut-based e-graph 优化工具（批处理 + 贪心替换版）
//   流程：读 Verilog → 循环{枚举 K-cut → 批处理调 Rust 优化所有候选 cut
//        → 找到第一个 gain 的 cut → substitute_node 替换}
//        → 每次替换后 cleanup_dangling（重新拓扑排序）→ 写 Verilog → 验证。
//
//   与纯统计版的区别：不只统计，而是真正替换（贪心 + 去重），得到实际省门数。
//   与 v3 替换版的区别：批处理一次调 Rust 处理所有候选 cut，快 ~100 倍。
// ============================================================

#include "cut_helpers.h"
#include "mockturtle/algorithms/cut_enumeration.hpp"
#include "mockturtle/algorithms/simulation.hpp"
#include "mockturtle/io/write_verilog.hpp"
#include "mockturtle/views/topo_view.hpp"
#include "lorina/verilog.hpp"
#include <cstdlib>
#include <ctime>

using namespace std;
using namespace mockturtle;

int main(int argc, char* argv[])
{
    srand((unsigned)time(0));
    string infile = (argc >= 2) ? argv[1] : "router.v";
    string outfile = (argc >= 3) ? argv[2] : "router_opt.v";
    int max_replace = (argc >= 4) ? atoi(argv[3]) : 1000000;

    xmg_network xmg;
    auto r = lorina::read_verilog(infile, verilog_reader(xmg));
    if (r != lorina::return_code::success) { cerr << "read fail\n"; return 1; }
    int n_gates_orig = (int)xmg.num_gates();
    cout << "[1] Read " << infile << ": " << n_gates_orig << " gates\n";

    xmg_network xmg_orig = xmg.clone();

    cut_enumeration_params ps;
    ps.cut_size = 6;
    ps.cut_limit = 25;

    struct Cand { xmg_network::node gate; vector<uint32_t> leaves; std::set<uint32_t> mffc; vector<uint32_t> mffc_leaves; };

    int replaced = 0, total_saved = 0;
    while (replaced < max_replace) {
        auto cuts = cut_enumeration(xmg, ps);

        // 收集候选 + 导出批处理
        vector<Cand> cands;
        ofstream batch("batch_in.dag");
        bool first = true;
        xmg.foreach_gate([&](auto const& n) {
            uint32_t idx = xmg.node_to_index(n);
            for (auto const& cut : cuts.cuts(idx)) {
                if (cut->size() == 1 && *cut->begin() == idx) continue;
                vector<uint32_t> leaves(cut->begin(), cut->end());
                sort(leaves.begin(), leaves.end());
                std::set<uint32_t> leaf_set(leaves.begin(), leaves.end());
                std::set<uint32_t> mffc;
                vector<uint32_t> mffc_leaves;
                CollectMFFC(xmg, n, leaf_set, mffc, mffc_leaves);
                if (mffc.size() < 2) continue;
                cands.push_back({n, leaves, mffc, mffc_leaves});
                if (!first) batch << "\n";
                first = false;
                ExportMFFCForEgraph(xmg, n, mffc, mffc_leaves, batch);
            }
        });
        batch.close();
        if (cands.empty()) break;

        // 批处理调 Rust
        string cmd = "rust\\target\\release\\xmg_egraph_cut.exe batch_in.dag batch_out.dag";
        // [05-MOD][batch-guard] Rust 失败时绝不读取可能残留的旧 batch_out.dag。
        int rust_status = system(cmd.c_str());
        if (rust_status != 0) {
            cerr << "Rust batch optimizer failed (status " << rust_status << "); abort\n";
            return 2;
        }

        // 读回，找第一个 gain，替换
        ifstream fin("batch_out.dag");
        if (!fin) {
            cerr << "Rust batch optimizer produced no output; abort\n";
            return 2;
        }
        string line;
        int ci = -1, optn = 0;
        string dag;
        bool changed = false;
        bool malformed = false;
        vector<bool> result_seen(cands.size(), false);
        int selected_c = -1, selected_optn = -1; // [05-MOD] 记录选中候选的 optn，避免用错
        string selected_d;
        auto try_replace = [&](int c, const string& d) {
            if (c < 0 || c >= (int)cands.size() || result_seen[c]) {
                malformed = true;
                return;
            }
            result_seen[c] = true;
            if (changed) return;
            int orig = (int)cands[c].mffc.size();
            if (optn < 0 || optn >= orig) return;
            // 先只记录第一个 gain 候选；全部结果验证完整后才真正改网络。
            selected_c = c;
            selected_optn = optn;
            selected_d = d;
            changed = true;
        };

        while (getline(fin, line)) {
            if (line.empty()) {
                try_replace(ci, dag);
                ci = -1; optn = 0; dag.clear();
            } else if (ci < 0) {
                sscanf(line.c_str(), "%d %d", &ci, &optn);
            } else {
                dag += line + "\n";
            }
        }
        // [05-MOD] 不再在循环外调 try_replace：Rust 每个候选后都有空行，
        //   循环内已处理全部；循环外 ci=-1 会误置 malformed。
        fin.close();

        // [05-MOD][batch-guard] 输出必须对每个候选恰有一条记录；否则不改网络。
        if (malformed) {
            cerr << "Malformed Rust batch output; abort\n";
            return 2;
        }
        for (int i = 0; i < (int)result_seen.size(); ++i) {
            if (!result_seen[i]) {
                cerr << "Incomplete Rust batch output: missing candidate " << i
                     << " of " << result_seen.size() << "; abort\n";
                return 2;
            }
        }
        if (!changed) break;

        // 真正替换选中的第一个 gain 候选
        ofstream tmp("tmp_opt.dag");
        tmp << selected_d;
        tmp.close();
        auto new_root = RebuildInNetwork(xmg, cands[selected_c].mffc_leaves, "tmp_opt.dag");
        xmg.substitute_node(cands[selected_c].gate, new_root);
        replaced++;
        total_saved += (int)cands[selected_c].mffc.size() - selected_optn;
        xmg = cleanup_dangling(xmg);
    }

    cout << "[2] 替换: " << replaced << " 个 gate, 实际省 " << (int)(n_gates_orig - xmg.num_gates()) << " 门"
         << " (理论累计 " << total_saved << ")\n";
    cout << "    gates: " << n_gates_orig << " -> " << xmg.num_gates() << "\n";

    // 等价验证
    int nPI = (int)xmg_orig.num_pis();
    bool equiv = true;
    for (int p = 0; p < 2000 && equiv; p++) {
        vector<bool> inputs(nPI);
        for (int i = 0; i < nPI; i++) inputs[i] = rand() % 2;
        auto o1 = simulate<bool>(xmg_orig, default_simulator<bool>(inputs));
        topo_view xmg_topo{xmg};
        auto o2 = simulate<bool>(xmg_topo, default_simulator<bool>(inputs));
        if (o1 != o2) { equiv = false; cout << "    MISMATCH at " << p << "\n"; }
    }
    cout << "[3] 等价验证: " << (equiv ? "PASS (2000/2000)" : "FAIL") << "\n";

    write_verilog(xmg, outfile);
    cout << "[4] 输出: " << outfile << "\n";

    return equiv ? 0 : 1;
}
