#pragma once

// S-expression -> xmg_network: replaces internal logic of an existing XMG
// Keeps the original PI structure intact to preserve indexing for SubstituteSub

inline string NextSExprToken(const string& s, size_t& pos) {
    while (pos < s.size() && s[pos] == ' ') pos++;
    if (pos >= s.size()) return "";
    if (s[pos] == '(') { pos++; return "("; }
    if (s[pos] == ')') { pos++; return ")"; }
    size_t start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '(' && s[pos] != ')') pos++;
    return s.substr(start, pos - start);
}

// Build optimized logic into an existing xmg_network, reusing its PI signals
inline xmg_network::signal ParseIntoXmg(
    xmg_network& xmg,
    const string& expr,
    size_t& pos,
    vector<xmg_network::signal>& pi_signals)
{
    string tok = NextSExprToken(expr, pos);
    if (tok != "(") {
        if (tok == "0") return xmg.get_constant(false);
        if (tok == "1") return xmg.get_constant(true);
        // PI: parse "pi_N" -> use pre-existing PI signal at index N
        if (tok.rfind("pi_", 0) == 0) {
            int idx = stoi(tok.substr(3));
            if (idx < (int)pi_signals.size()) return pi_signals[idx];
            // fallback: create new PI (shouldn't happen with correct NUM_PI)
            while ((int)pi_signals.size() <= idx) pi_signals.push_back(xmg.create_pi());
            return pi_signals.back();
        }
        return xmg.get_constant(false);
    }

    string op = NextSExprToken(expr, pos);

    if (op == "not") {
        auto child = ParseIntoXmg(xmg, expr, pos, pi_signals);
        NextSExprToken(expr, pos);
        return !child;
    }

    auto a = ParseIntoXmg(xmg, expr, pos, pi_signals);
    auto b = ParseIntoXmg(xmg, expr, pos, pi_signals);
    auto c = ParseIntoXmg(xmg, expr, pos, pi_signals);
    NextSExprToken(expr, pos);

    if (op == "xor3") return xmg.create_xor3(a, b, c);
    return xmg.create_maj(a, b, c);
}

// Read E-graph optimized output and replace internal logic of an existing xmg_network
// Returns new xmg with same PI structure, different internal gates
inline xmg_network ApplyEgraphResult(
    xmg_network& original,
    const string& opt_filepath)
{
    // Clone PI structure from original
    xmg_network xmg;
    int num_pi = original.num_pis();
    vector<xmg_network::signal> pi_signals;
    for (int i = 0; i < num_pi; i++) {
        pi_signals.push_back(xmg.create_pi());
    }

    // Read E-graph output and create new internal logic
    ifstream fin(opt_filepath);
    string line;
    while (getline(fin, line)) {
        if (line.empty()) continue;
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == string::npos || line[first] != '(') continue;
        size_t pos = 0;
        auto sig = ParseIntoXmg(xmg, line, pos, pi_signals);
        xmg.create_po(sig);
    }
    fin.close();
    return xmg;
}
