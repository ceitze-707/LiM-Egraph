// ============================================================
// E-graph integration helpers — numbered DAG format
// ============================================================

/// Export xmg_network as numbered DAG.
/// foreach_gate is topological: children always before parents.
/// Complemented edges become inline NOT gates.
inline void ExportXmgForEgraphNumbered(xmg_network& xmg, const string& filepath) {
    ofstream fout(filepath);
    int nPI = (int)xmg.num_pis();
    int nGate = (int)xmg.num_gates();

    // Header
    fout << "PI_COUNT " << nPI << "\n";
    fout << "GATE_COUNT " << nGate << "\n";
    fout << "PI_MAP";
    int _pi_cnt = 0;
    xmg.foreach_pi([&](auto const& n) {
        fout << " " << _pi_cnt << ":" << xmg.node_to_index(n);
        _pi_cnt++;
    });
    fout << "\n";

    // Build flat index map (assigned as we write)
    std::map<xmg_network::node, int> fidx;
    int next_idx = 0;

    // Write PIs
    int _pi_cnt2 = 0;
    xmg.foreach_pi([&](auto const& n) {
        fout << "PI " << _pi_cnt2 << "\n";
        fidx[n] = next_idx++;
        _pi_cnt2++;
    });
    // Constants (C0 and C1 share the same node in XMG, use separate indices)
    int c0_fidx = next_idx++; fout << "C0\n";
    int c1_fidx = next_idx++; fout << "C1\n";

    // Write gates in foreach_gate order (topological)
    // For complemented fanins: write NOT first, then reference the NOT
    vector<string> gate_lines;
    xmg.foreach_gate([&](auto const& n) {
        // Build children list, writing inline NOTs as needed
        vector<int> children;
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto child_node = xmg.get_node(f);
            int child_fidx = xmg.is_constant(child_node) ? c0_fidx : fidx[child_node];
            if (xmg.is_complemented(f)) {
                // Write inline NOT gate
                fout << "N " << child_fidx << "\n";
                int not_fidx = next_idx++;
                children.push_back(not_fidx);
            } else {
                children.push_back(child_fidx);
            }
        });
        // Write this gate
        if (xmg.is_xor3(n))
            fout << "X " << children[0] << " " << children[1] << " " << children[2] << "\n";
        else
            fout << "M " << children[0] << " " << children[1] << " " << children[2] << "\n";
        fidx[n] = next_idx++;
    });

    // Write POs
    xmg.foreach_po([&](auto const& f) {
        auto po_node = xmg.get_node(f);
        int po_fidx = xmg.is_constant(po_node) ? c0_fidx : fidx[po_node];
        if (xmg.is_complemented(f)) {
            fout << "N " << po_fidx << "\n";
            fout << "P " << next_idx << "\n";
            next_idx++;
        } else {
            fout << "P " << po_fidx << "\n";
        }
    });

    fout.close();
    cout << "  [E-graph] exported " << nPI << " PIs, " << nGate << " gates\n";
}

/// Call Rust E-graph optimizer
inline vector<string> CallEgraphOptimizer(const string& input_file, const string& output_file) {
    string cmd = "D:/DESKBOOK/0809/xmg_egraph/target/release/xmg_egraph.exe "
               + input_file + " " + output_file;
    cout << "  [E-graph] running\n";
    int ret = system(cmd.c_str());
    if (ret != 0) {
        cout << "  [E-graph] ERROR: exit code " << ret << "\n";
        return {};
    }
    vector<string> results;
    ifstream fin(output_file);
    string line;
    while (getline(fin, line)) {
        if (!line.empty()) results.push_back(line);
    }
    fin.close();
    return results;
}
