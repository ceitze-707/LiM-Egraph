#pragma once

// ============================================================
// Numbered-format DAG parser — rebuild xmg_network
// Matches ExportXmgForEgraphNumbered output exactly
// ============================================================

inline xmg_network RebuildXmgFromNumbered(const string& filepath) {
    xmg_network xmg;
    ifstream fin(filepath);
    string line;
    vector<xmg_network::signal> nodes; // flat index → signal

    while (getline(fin, line)) {
        if (line.empty()) continue;
        char first = line[0];

        if (first == 'P' && line.rfind("PI ", 0) == 0) {
            // PI line: "PI num" — must check BEFORE general P check
            nodes.push_back(xmg.create_pi());
        }
        else if (first == 'P' && line.rfind("PI_COUNT", 0) != 0 && line.rfind("PI_MAP", 0) != 0) {
            // PO line: "P idx"
            int idx = atoi(line.c_str() + 2);
            if (idx >= 0 && idx < (int)nodes.size())
                xmg.create_po(nodes[idx]);
        }
        else if (line.rfind("C0", 0) == 0) {
            nodes.push_back(xmg.get_constant(false));
        }
        else if (line.rfind("C1", 0) == 0) {
            nodes.push_back(xmg.get_constant(true));
        }
        else if (first == 'X' || first == 'M' || first == 'N') {
            // Gate line: "X a b c" or "M a b c" or "N a"
            vector<int> kids;
            istringstream iss(line);
            string tok;
            iss >> tok; // skip type
            while (iss >> tok) kids.push_back(stoi(tok));

            xmg_network::signal sig;
            if (first == 'N' && kids.size() >= 1 && kids[0] < (int)nodes.size())
                sig = !nodes[kids[0]];
            else if (first == 'X' && kids.size() >= 3
                     && kids[0] < (int)nodes.size() && kids[1] < (int)nodes.size() && kids[2] < (int)nodes.size())
                sig = xmg.create_xor3(nodes[kids[0]], nodes[kids[1]], nodes[kids[2]]);
            else if (first == 'M' && kids.size() >= 3
                     && kids[0] < (int)nodes.size() && kids[1] < (int)nodes.size() && kids[2] < (int)nodes.size())
                sig = xmg.create_maj(nodes[kids[0]], nodes[kids[1]], nodes[kids[2]]);
            else continue;

            nodes.push_back(sig);
        }
        // else skip: headers (PI_COUNT, GATE_COUNT, PI_MAP)
    }
    fin.close();
    return xmg;
}
