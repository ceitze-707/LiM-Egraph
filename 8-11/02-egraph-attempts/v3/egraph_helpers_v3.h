
// ============================================================
// E-graph integration helpers
// ============================================================

inline string XmgNode2SExpr(xmg_network& xmg, xmg_network::node n,
                            std::map<xmg_network::node, string>& cache) {
    auto it = cache.find(n);
    if (it != cache.end()) return it->second;

    if (xmg.is_constant(n)) {
        string val = xmg.is_complemented(n) ? "1" : "0";
        cache[n] = val;
        return val;
    }
    if (xmg.is_pi(n)) {
        string name = "pi_" + to_string(xmg.node_to_index(n));
        cache[n] = name;
        return name;
    }

    string op = xmg.is_xor3(n) ? "xor3" : "maj3";
    string expr = "(" + op;

    xmg.foreach_fanin(n, [&](auto const& f) {
        auto child = xmg.get_node(f);
        string child_expr = XmgNode2SExpr(xmg, child, cache);
        if (xmg.is_complemented(f))
            child_expr = "(not " + child_expr + ")";
        expr += " " + child_expr;
    });

    expr += ")";
    cache[n] = expr;  // cache intermediate nodes to avoid exponential blowup
    return expr;
}

/// Export all POs of xmg_network as S-expressions (one per line)
/// First line is PI count: "NUM_PI 42"
inline void ExportXmgForEgraph(xmg_network& xmg, const string& filepath) {
    ofstream fout(filepath);
    fout << "NUM_PI " << xmg.num_pis() << "\n";
    std::map<xmg_network::node, string> cache;
    int nPO = 0;
    xmg.foreach_po([&](auto const& f) {
        auto po_node = xmg.get_node(f);
        string expr = XmgNode2SExpr(xmg, po_node, cache);
        if (xmg.is_complemented(f)) expr = "(not " + expr + ")";
        fout << expr << "\n";
        nPO++;
    });
    fout.close();
    cout << "  [E-graph] exported " << nPO << " PO(s) with " << xmg.num_pis() << " PIs to " << filepath << "\n";
}

inline vector<string> CallEgraphOptimizer(const string& input_file, const string& output_file) {
    string cmd = "D:/DESKBOOK/0809/xmg_egraph/target/release/xmg_egraph.exe "
               + input_file + " " + output_file;
    cout << "  [E-graph] running: " << cmd << "\n";
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
    cout << "  [E-graph] optimized: " << results.size() << " expression(s)\n";
    for (auto& r : results)
        cout << "    -> " << (r.size() > 80 ? r.substr(0, 80) + "..." : r) << "\n";
    return results;
}

