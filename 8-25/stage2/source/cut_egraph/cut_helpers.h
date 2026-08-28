#pragma once
// ============================================================
// [05-ADD] cut-based e-graph 辅助函数
//   思路来源：eLogic (DATE 2026) 的 cut 内局部 e-graph。
//   与 04-egraph_v2 的区别：04 是"全电路一次 export→egg→rebuild"，
//   这里是"每个 cut 单独 export→egg→rebuild"，避开全电路 e-graph
//   的爆炸与 DAG 共享破坏（v5.2 的 Size=5 教训）。
// ============================================================

// [06-MOD] 与主编译器一致：只引入 cut helper 所需的 XMG 定义，避免
// mockturtle 聚合头把 percy/nauty 的遗留接口带入 IMCCompiler 编译单元。
#include "mockturtle/networks/xmg.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>

using namespace std;
using namespace mockturtle;

// ------------------------------------------------------------
// [05-ADD] 收集 root 的锥（cone）：从 root 往下走到 cut 的 leaves 为止。
//   返回锥内所有 gate 的 node index（不含 leaves、不含 PI/常量）。
//   leaves 存的是 node index。
// ------------------------------------------------------------
inline void CollectCone(xmg_network const& xmg, xmg_network::node root,
                        std::set<uint32_t> const& leaves, std::set<uint32_t>& cone)
{
    std::vector<xmg_network::node> stack{root};
    while (!stack.empty())
    {
        auto n = stack.back();
        stack.pop_back();
        uint32_t idx = xmg.node_to_index(n);
        if (leaves.count(idx)) continue;          // 遇到 cut 边界（leaf）→ 停止
        if (xmg.is_pi(n) || xmg.is_constant(n)) continue; // 防御：不应出现
        if (cone.count(idx)) continue;             // 已访问
        cone.insert(idx);
        xmg.foreach_fanin(n, [&](auto const& f) {
            stack.push_back(xmg.get_node(f));
        });
    }
}

// ------------------------------------------------------------
// [05-ADD] 把一个 cut（root + leaves）导出成编号 DAG 文件。
//   格式与 04-egraph_v2 的 ExportXmgForEgraphNumbered 一致：
//     PI_COUNT / GATE_COUNT / PI 行 / C0 C1 / X|M|N 门行 / P 行
//   这样 04 的 RebuildXmgFromNumbered 能直接解析。
//   leaves 需按 node index 升序，保证导出/重建顺序稳定（v3 教训）。
// ------------------------------------------------------------
inline void ExportCutForEgraph(xmg_network const& xmg, xmg_network::node root,
                               std::vector<uint32_t> const& leaves, std::string const& filepath)
{
    std::set<uint32_t> leaf_set(leaves.begin(), leaves.end());
    std::set<uint32_t> cone;
    CollectCone(xmg, root, leaf_set, cone);

    ofstream fout(filepath);
    fout << "PI_COUNT " << leaves.size() << "\n";
    fout << "GATE_COUNT " << cone.size() << "\n";

    // flat index 分配（必须与 Rebuild 的 nodes 向量下标一致）
    std::map<uint32_t, int> fidx;  // node index -> flat index
    int next = 0;

    // leaves -> PI（flat 0..L-1，顺序 = leaves 的顺序）
    // [05-MOD] 关键修复：第二个字段必须写 leaf 序号 0..L-1，不是原电路 node index！
    //   因为 Rust 端用这个字段生成 leaf 名 "pi_N"，tree_to_lines 里 N 是 0..L-1 的序号。
    //   原 baseline 导出写 node index，导致 Rust 端把 N 当序号截断(min(pi-1))，
    //   所有叶子映射到最后一个 leaf → 优化结果功能错乱（FAIL）。
    for (size_t i = 0; i < leaves.size(); i++) {
        fout << "PI " << i << "\n";
        fidx[leaves[i]] = next++;
    }
    // 常量（独立 flat index，避免 C0/C1 共享节点覆盖 —— 04 CHANGELOG Bug2）
    int c0 = next++; fout << "C0\n";
    int c1 = next++; fout << "C1\n";

    // 锥内门：set 遍历天然按 node index 升序 = 拓扑序
    for (uint32_t g : cone) {
        auto n = xmg.index_to_node(g);
        std::vector<int> children;
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto cn = xmg.get_node(f);
            if (xmg.is_constant(cn)) {
                // 常量：互补=1，非互补=0（XMG 常量共享节点，靠相位区分）
                children.push_back(xmg.is_complemented(f) ? c1 : c0);
            } else {
                int cfi = fidx[xmg.node_to_index(cn)];
                if (xmg.is_complemented(f)) {
                    // 互补边 → 先写 inline NOT，再引用它
                    fout << "N " << cfi << "\n";
                    children.push_back(next++);
                } else {
                    children.push_back(cfi);
                }
            }
        });
        if (xmg.is_xor3(n))
            fout << "X " << children[0] << " " << children[1] << " " << children[2] << "\n";
        else
            fout << "M " << children[0] << " " << children[1] << " " << children[2] << "\n";
        fidx[g] = next++;
    }

    // PO = root 的 flat index
    fout << "P " << fidx[xmg.node_to_index(root)] << "\n";
    fout.close();
}

// ------------------------------------------------------------
// [05-ADD] 从编号 DAG 重建 xmg_network。
//   与 04-egraph_v2 的 RebuildXmgFromNumbered 完全一致（已验证 2000/2000）。
// ------------------------------------------------------------
inline xmg_network RebuildXmgFromNumbered(string const& filepath)
{
    xmg_network xmg;
    ifstream fin(filepath);
    string line;
    vector<xmg_network::signal> nodes; // flat index → signal

    while (getline(fin, line)) {
        if (line.empty()) continue;
        char first = line[0];

        if (first == 'P' && line.rfind("PI ", 0) == 0) {
            // PI 行（必须放在 P 行判断之前）
            nodes.push_back(xmg.create_pi());
        }
        else if (first == 'P' && line.rfind("PI_COUNT", 0) != 0 && line.rfind("PI_MAP", 0) != 0) {
            // PO 行
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
            vector<int> kids;
            istringstream iss(line);
            string tok;
            iss >> tok; // 跳过类型
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
        // 其余（PI_COUNT/GATE_COUNT/PI_MAP）跳过
    }
    fin.close();
    return xmg;
}

// ------------------------------------------------------------
// [05-ADD] 提取 root 的最大扇出自由锥（MFFC）：锥里所有"fanout 全在锥内"的门。
//   这些门替换后不会影响锥外任何门，可安全删除。
//   返回 mffc（门集合，含 root）+ mffc_leaves（MFFC 的边界，非常量节点）。
// ------------------------------------------------------------
inline void CollectMFFC(xmg_network const& xmg, xmg_network::node root,
                        std::set<uint32_t> const& cut_leaves,
                        std::set<uint32_t>& mffc, std::vector<uint32_t>& mffc_leaves)
{
    std::set<uint32_t> cone;
    CollectCone(xmg, root, cut_leaves, cone);
    uint32_t root_idx = xmg.node_to_index(root);

    // 统计每个锥内门被"锥内其他门"引用的次数 + 锥内 fanout 列表
    std::map<uint32_t, int> in_cone_fanout;
    std::map<uint32_t, std::vector<uint32_t>> cone_fanout; // g -> 锥内引用 g 的门
    for (uint32_t g : cone) {
        auto n = xmg.index_to_node(g);
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto fn = xmg.get_node(f);
            uint32_t fi = xmg.node_to_index(fn);
            if (cone.count(fi)) {
                in_cone_fanout[fi]++;
                cone_fanout[fi].push_back(g);
            }
        });
    }

    // MFFC 迭代收敛：root + 无锥外 fanout 且所有锥内 fanout 都在 MFFC 里的门。
    // [05-MOD] 关键：之前用"fanout 全在锥内"近似，错把"fanout 指向多扇出门"的
    //   门也当 MFFC（例：64 被 66 引用，66 是多扇出 leaf，64 不应删却进了 MFFC）。
    mffc.clear();
    mffc.insert(root_idx);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t g : cone) {
            if (g == root_idx || mffc.count(g)) continue;
            auto n = xmg.index_to_node(g);
            if ((int)xmg.fanout_size(n) != in_cone_fanout[g]) continue; // 有锥外 fanout
            bool all_in_mffc = true;
            for (uint32_t fo : cone_fanout[g]) {
                if (!mffc.count(fo)) { all_in_mffc = false; break; }
            }
            if (all_in_mffc) { mffc.insert(g); changed = true; }
        }
    }

    // MFFC 边界 = MFFC 里门的 fanin 中，非 MFFC 且非常量 的节点
    std::set<uint32_t> boundary;
    for (uint32_t g : mffc) {
        auto n = xmg.index_to_node(g);
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto fn = xmg.get_node(f);
            uint32_t fi = xmg.node_to_index(fn);
            if (!mffc.count(fi) && !xmg.is_constant(fn)) boundary.insert(fi);
        });
    }
    mffc_leaves.assign(boundary.begin(), boundary.end());
    std::sort(mffc_leaves.begin(), mffc_leaves.end());
}

// ------------------------------------------------------------
// [05-ADD] 把 MFFC（门集合）+ 边界 leaves 导出成编号 DAG（与 ExportCutForEgraph 同格式）。
//   常量 fanin 走 C0/C1，非常量 fanin 走 fidx。
// ------------------------------------------------------------
inline void ExportMFFCForEgraph(xmg_network const& xmg, xmg_network::node root,
                                std::set<uint32_t> const& mffc,
                                std::vector<uint32_t> const& leaves,
                                std::ostream& fout)
{
    fout << "PI_COUNT " << leaves.size() << "\n";
    fout << "GATE_COUNT " << mffc.size() << "\n";

    std::map<uint32_t, int> fidx;
    int next = 0;
    for (size_t i = 0; i < leaves.size(); i++) {
        fout << "PI " << i << "\n";
        fidx[leaves[i]] = next++;
    }
    int c0 = next++; fout << "C0\n";
    int c1 = next++; fout << "C1\n";

    for (uint32_t g : mffc) {  // set 遍历升序 = 拓扑序
        auto n = xmg.index_to_node(g);
        std::vector<int> children;
        xmg.foreach_fanin(n, [&](auto const& f) {
            auto cn = xmg.get_node(f);
            if (xmg.is_constant(cn)) {
                children.push_back(xmg.is_complemented(f) ? c1 : c0);
            } else {
                int cfi = fidx[xmg.node_to_index(cn)];
                if (xmg.is_complemented(f)) {
                    fout << "N " << cfi << "\n";
                    children.push_back(next++);
                } else {
                    children.push_back(cfi);
                }
            }
        });
        if (xmg.is_xor3(n))
            fout << "X " << children[0] << " " << children[1] << " " << children[2] << "\n";
        else
            fout << "M " << children[0] << " " << children[1] << " " << children[2] << "\n";
        fidx[g] = next++;
    }
    fout << "P " << fidx[xmg.node_to_index(root)] << "\n";
}

// ------------------------------------------------------------
// [05-ADD] 读优化后的编号 DAG，在**原网络 xmg 上**创建结构（leaves 用原网络信号），
//   返回优化后 root 的 signal。用于 substitute_node 替换。
// ------------------------------------------------------------
inline xmg_network::signal RebuildInNetwork(xmg_network& xmg,
                                            std::vector<uint32_t> const& leaves,
                                            std::string const& filepath,
                                            bool* parse_ok = nullptr)
{
    // [06-ADD][correctness] 调用方只有在完整、合法地解析了 Rust 返回 DAG
    // 后才允许替换原节点。不能把解析失败悄悄降级成常量 0 后继续执行。
    if (parse_ok != nullptr) *parse_ok = false;
    ifstream fin(filepath);
    if (!fin) return xmg.get_constant(false);
    string line;
    vector<xmg_network::signal> nodes;
    int root_flat = -1;
    bool malformed = false;

    while (getline(fin, line)) {
        if (line.empty()) continue;
        char first = line[0];

        if (first == 'P' && line.rfind("PI ", 0) == 0) {
            // [06-FIX][audit] PI 行必须严格按导出端口顺序编号。若接受任意
            // "PI ..." 文本，损坏或重排的 Rust 输出会把错误的 leaf 接到门上。
            istringstream iss(line);
            string keyword, extra;
            size_t declared_index = 0;
            const size_t expected_index = nodes.size();
            if (!(iss >> keyword >> declared_index) || keyword != "PI"
                || declared_index != expected_index || (iss >> extra)
                || expected_index >= leaves.size()) { malformed = true; break; }
            nodes.push_back(xmg.make_signal(xmg.index_to_node(leaves[expected_index])));
        }
        else if (line.rfind("PI_COUNT ", 0) == 0 || line.rfind("GATE_COUNT ", 0) == 0) {
            // 元数据由 batch 外层和实际解析结果交叉约束；此处只允许已知头部类型。
            istringstream iss(line);
            string keyword, extra;
            int count = -1;
            if (!(iss >> keyword >> count) || count < 0 || (iss >> extra)) { malformed = true; break; }
        }
        else if (first == 'P') {
            // 输出行只能出现一次，且必须精确写成 "P <flat-index>"。
            istringstream iss(line);
            string keyword, extra;
            if (root_flat >= 0 || !(iss >> keyword >> root_flat) || keyword != "P" || (iss >> extra))
                { malformed = true; break; }
        }
        else if (line == "C0") nodes.push_back(xmg.get_constant(false));
        else if (line == "C1") nodes.push_back(xmg.get_constant(true));
        else if (first == 'X' || first == 'M' || first == 'N') {
            vector<int> kids;
            istringstream iss(line);
            string tok;
            iss >> tok;
            try {
                while (iss >> tok) kids.push_back(stoi(tok));
            } catch (...) {
                malformed = true;
                break;
            }

            xmg_network::signal sig;
            // [06-ADD][correctness] 下标必须同时满足非负和已定义；原 05
            // 版本只检查上界，-1 会造成 nodes[-1] 的未定义行为。
            auto valid = [&](int index) { return index >= 0 && index < static_cast<int>(nodes.size()); };
            if (first == 'N' && kids.size() == 1 && valid(kids[0]))
                sig = !nodes[kids[0]];
            else if (first == 'X' && kids.size() == 3
                     && valid(kids[0]) && valid(kids[1]) && valid(kids[2]))
                sig = xmg.create_xor3(nodes[kids[0]], nodes[kids[1]], nodes[kids[2]]);
            else if (first == 'M' && kids.size() == 3
                     && valid(kids[0]) && valid(kids[1]) && valid(kids[2]))
                sig = xmg.create_maj(nodes[kids[0]], nodes[kids[1]], nodes[kids[2]]);
            else { malformed = true; break; }

            nodes.push_back(sig);
        }
        else {
            // 未知行不能静默跳过：这条文件来自外部进程，必须采用白名单解析。
            malformed = true;
            break;
        }
    }
    fin.close();
    if (malformed || root_flat < 0 || root_flat >= (int)nodes.size())
        return xmg.get_constant(false); // 防御：退化情况
    if (parse_ok != nullptr) *parse_ok = true;
    return nodes[root_flat];
}
