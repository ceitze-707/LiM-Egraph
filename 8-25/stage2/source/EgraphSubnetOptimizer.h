#pragma once
// ============================================================
// [06-ADD] IMCCompiler 内嵌 e-graph 的“子网候选生成器”。
//
// 它只处理 NetList::ExtractSub 得到的独立 XMG，不直接重建或替换全局
// NetList。完成后仍由 Scheduler::NewDSE 调度、SubstituteSub 回填，并以
// 全局 Size/MF 的 Pareto 判断决定候选是否保留。
// ============================================================

#include "cut_egraph/cut_helpers.h" // DAG 导入导出、MFFC 收集和局部重建辅助函数。
#include "mockturtle/algorithms/cut_enumeration.hpp" // 枚举每个门周围的有界输入 cut。
#include <cstdlib> // getenv、atoi、system。
#include <cstdio> // sscanf，用于严格解析 Rust batch 输出头。

struct EgraphSubnetStats
{
    int candidates = 0; // 已导出给 Rust e-graph 的局部候选数量。
    int replacements = 0; // 最终完成局部证明并实际写入子网的替换次数。
    int gates_before = 0; // 进入函数时整个子网的 XMG 门数。
    int gates_after = 0; // 返回前整个子网的 XMG 门数。
    int cut_size = 6; // 本轮 cut 枚举输入上限；写入日志以区分搜索空间。
    int oversized_mffc_skipped = 0; // 因 MFFC 内部门数超过验证预算而跳过的候选数。
    int oversized_boundary_skipped = 0; // 因边界输入过多、无法穷尽证明而跳过的候选数。
    int max_depth_before = 0; // 变换前最大逻辑深度。
    int max_fanout_before = 0; // 变换前最大扇出。
    int max_depth_after = 0; // 变换后最大逻辑深度。
    int max_fanout_after = 0; // 变换后最大扇出。
    int selected_mffc_gates_total = 0; // 所有实际选中 MFFC 的门数总和。
    int selected_boundary_inputs_total = 0; // 所有实际选中 MFFC 边界输入数总和。
    int selected_boundary_inputs_max = 0; // 实际选中候选中最大的边界输入数。
    int selected_local_gain_total = 0; // 选中候选在局部门数上的总收益（仅诊断，不是最终目标）。
    int zero_gain_replacements = 0; // 门数不变、但仍通过证明并写回的结构候选数。
    // [06-ADD][audit] 每一次连续替换的结构指纹；k=2/3 诊断需要知道
    // “少了哪一个 MFFC”，不能只保留累计门数。
    std::vector<uint32_t> selected_root_indices;
    std::vector<int> selected_mffc_gate_counts;
    std::vector<int> selected_boundary_input_counts;
    std::vector<int> selected_local_gains;
    bool batch_complete = false; // Rust 输出是否完整且与输入候选一一对应。
    bool equivalence_proven = false; // 是否至少有一次替换通过完整穷尽等价证明。
};

// [06-ADD][diagnostic] 门数不是唯一会影响 IMC 调度的结构属性。这个
// 轻量统计不参与接受决策，只记录候选的拓扑形状，为后续解释/筛选 k=3
// 这类“更小但难调度”的网络提供可复查证据。
inline void DescribeEgraphTopology( xmg_network const& xmg,
                                    int& max_depth,
                                    int& max_fanout )
{
    // depth 用 XMG 节点编号索引；因 XMG 按拓扑顺序遍历，前驱深度先于当前门可得。
    std::vector<int> depth( xmg.size(), 0 );
    max_depth = 0; // 每次统计前清零输出参数。
    max_fanout = 0; // 每次统计前清零输出参数。
    xmg.foreach_gate( [&]( auto const& node ) {
        int node_depth = 0; // 当前门的深度等于所有输入深度最大值加一。
        xmg.foreach_fanin( node, [&]( auto const& fanin ) {
            node_depth = std::max( node_depth,
                depth[xmg.node_to_index( xmg.get_node( fanin ) )] ); // 反相不改变逻辑层数。
        } );
        const auto index = xmg.node_to_index( node ); // 取得当前门在数组中的编号。
        depth[index] = node_depth + 1; // 门自身形成新的一层。
        max_depth = std::max( max_depth, depth[index] ); // 更新子网最大深度。
        max_fanout = std::max( max_fanout, static_cast<int>( xmg.fanout_size( node ) ) ); // 更新最大扇出。
    } );
}

// [06-ADD] 对一个已提取子网生成一个 e-graph 候选。
// max_replacements=1 是刻意的：DSE 每轮已经会反复产生子网；先用一个
// 低风险局部变换验证 IMC Size/MF 是否受益，避免把 e-graph 的门数目标
// 变成主导整个搜索过程的目标。
inline EgraphSubnetStats TryEgraphOnSubnetwork( xmg_network& xmg,
                                                 int max_replacements = 1 )
{
    EgraphSubnetStats stats; // 所有字段按结构体定义的默认值清零。
    stats.gates_before = static_cast<int>( xmg.num_gates() ); // 记录整个待优化子网的起始门数。
    // [06-ADD][measurement] 与变换后的拓扑使用同一算法统计变换前结构，
    // 以便后续分析“深度/扇出变化”是否与调度成本或 Pareto 结果相关。
    // 这些字段仅记录，不参与候选接受，避免在证据不足时引入武断筛选。
    DescribeEgraphTopology( xmg, stats.max_depth_before, stats.max_fanout_before );

    mockturtle::cut_enumeration_params ps; // 配置 cut 枚举器。
    ps.cut_size = 6; // 默认保持历史实验的 6 输入 cut。
    if ( const char* configured = std::getenv( "IMC_EGRAPH_CUT_SIZE" ) )
    {
        const int requested = std::atoi( configured );
        // 本轮 sweep 只允许 2..12：每个候选仍能使用 2^n 的完备穷举证明，
        // 不会因扩大 cut 而悄悄降级为未验证或 SAT 验证。
        if ( requested >= 2 && requested <= 12 ) ps.cut_size = requested;
    }
    stats.cut_size = ps.cut_size;
    ps.cut_limit = 25; // 每个节点最多保留 25 个 cut，限制枚举与 Rust batch 规模。

    // [06-ADD][correctness] 每次真正替换都要有精确 SAT 证明。过大的
    // MFFC 会让验证成本失控，因而宁可跳过，也不接受未经证明的优化。
    // 可通过环境变量调整，但默认上限保证 DSE 不会被一次证明卡住。
    int max_verified_mffc_gates = 32; // 默认只对不超过 32 门的 MFFC 做精确验证。
    if ( const char* configured = std::getenv( "IMC_EGRAPH_VERIFY_MAX_GATES" ) )
    {
        const int requested = std::atoi( configured ); // 将环境变量文本转换为整数。
        if ( requested > 0 ) max_verified_mffc_gates = requested; // 忽略 0、负数及非数字输入。
    }
    constexpr int max_exhaustive_boundary_inputs = 12; // 最多枚举 2^12=4096 组边界赋值。
    // [06-ADD][scalability] 每个 pass 的 Rust batch 可单独限流；默认保留
    // 原来的全部候选行为。该限制只缩小搜索空间，不会跳过任何正确性检查。
    int max_candidates_per_pass = 1000; // 单轮调用 Rust 的候选数默认上限。
    if ( const char* configured = std::getenv( "IMC_EGRAPH_MAX_CANDIDATES" ) )
    {
        const int requested = std::atoi( configured ); // 读取可选的用户限流值。
        if ( requested > 0 ) max_candidates_per_pass = requested; // 仅接受正整数。
    }

    struct candidate
    {
        xmg_network::node gate; // 原子网中将要被替换的 MFFC 根门。
        std::set<uint32_t> mffc; // MFFC 内部所有节点的编号集合。
        std::vector<uint32_t> leaves; // 该 MFFC 的外部边界输入，顺序也是重建时的端口顺序。
    };

    while ( stats.replacements < max_replacements ) // 默认最多接受一次替换；调用者可提高该上限。
    {
        auto cuts = mockturtle::cut_enumeration( xmg, ps ); // 为当前（可能已变换的）子网重新枚举 cut。
        std::vector<candidate> candidates; // 保存与 batch 输出记录严格按位置对应的候选元数据。
        std::ofstream batch( "imc_egraph_batch_in.dag" ); // 写出 Rust 程序读取的多记录 DAG 文件。
        bool first = true; // 控制相邻 DAG 记录之间是否写入空行分隔符。

        xmg.foreach_gate( [&]( auto const& n ) {
            if ( candidates.size() >= static_cast<size_t>( max_candidates_per_pass ) )
                return; // 已到限流阈值，停止遍历后续门。
            const auto index = xmg.node_to_index( n ); // 当前根门的全网 XMG 编号。
            for ( auto const& cut : cuts.cuts( index ) )
            {
                if ( candidates.size() >= static_cast<size_t>( max_candidates_per_pass ) )
                    return; // 同样在每个 cut 内检查全局候选上限。
                if ( cut->size() == 1 && *cut->begin() == index )
                    continue; // 单元素自 cut 不包含任何可替换逻辑，跳过。

                std::vector<uint32_t> cut_leaves( cut->begin(), cut->end() ); // 复制 cut 叶子到可排序的顺序容器。
                std::sort( cut_leaves.begin(), cut_leaves.end() ); // 固化端口顺序，保证导出/导入一致。
                std::set<uint32_t> leaf_set( cut_leaves.begin(), cut_leaves.end() ); // CollectMFFC 使用集合判断边界。
                std::set<uint32_t> mffc; // 收集到的最大扇出自由锥内部节点。
                std::vector<uint32_t> mffc_leaves; // 实际 MFFC 边界；可能与原 cut 叶子略有不同。
                CollectMFFC( xmg, n, leaf_set, mffc, mffc_leaves ); // 从根向前扩张，直到 fanout/边界阻止扩张。
                if ( mffc.size() < 2 )
                    continue; // 单门/空锥没有足够的 e-graph 重写空间。
                if ( mffc.size() > static_cast<size_t>( max_verified_mffc_gates ) )
                {
                    ++stats.oversized_mffc_skipped; // 记录因证明成本被安全跳过的候选。
                    continue; // 不生成未经验证的大候选。
                }
                // [06-ADD][correctness] 后续以穷尽真值表证明等价；输入超过
                // 12 个会使每次验证超过 4096 组，故安全跳过而不猜测。
                if ( mffc_leaves.size() > max_exhaustive_boundary_inputs )
                {
                    ++stats.oversized_boundary_skipped; // 记录边界输入过多的候选。
                    continue; // 避免指数枚举超时。
                }

                candidates.push_back( {n, mffc, mffc_leaves} ); // 保存后续解析、验证和替换所需的所有元数据。
                if ( !first )
                    batch << "\n"; // Rust 输入格式以空行分隔不同候选。
                first = false; // 此后写入的记录前都必须添加分隔符。
                ExportMFFCForEgraph( xmg, n, mffc, mffc_leaves, batch ); // 写出带编号、端口稳定的局部 DAG。
            }
        } );
        batch.close(); // 确保 Rust 启动前所有候选都已落盘。
        stats.candidates += static_cast<int>( candidates.size() ); // 累积所有 pass 的尝试数。
        if ( candidates.empty() )
            break; // 没有任何满足验证预算的候选，结束本子网处理。

        // [06-ADD] Rust 非零退出时不读取旧输出；与 05/v7 的 batch guard 一致。
        const int rust_status = std::system(
            "cut_egraph\\rust\\target\\release\\xmg_egraph_cut.exe "
            "imc_egraph_batch_in.dag imc_egraph_batch_out.dag" ); // 将全部局部 DAG 交给 egg/Rust 执行等价饱和与提取。
        if ( rust_status != 0 )
            break; // 外部优化器失败时绝不读取可能是旧版本的输出文件。

        std::ifstream result_file( "imc_egraph_batch_out.dag" ); // 打开 Rust 批处理结果。
        if ( !result_file )
            break; // 输出文件不存在，视为本轮无候选。

        // 每个 record 的下标必须与 candidates 下标一致；否则不能安全将 DAG 写回原 MFFC。
        struct result { int gates = -1; std::string dag; bool seen = false; };
        std::vector<result> results( candidates.size() ); // 预留每个输入候选的输出槽。
        int current = -1; // 当前正在读取的 Rust record 的原候选下标。
        int optimized_gates = -1; // 当前 record 报告的优化后门数。
        std::string dag; // 累积当前 record 的 DAG 文本。
        bool malformed = false; // 解析到格式错误、重复或越界记录时置位。
        auto finish_record = [&]() {
            if ( current < 0 )
                return; // 空记录/开始前空行无需处理。
            if ( current >= static_cast<int>( results.size() ) || results[current].seen )
            {
                malformed = true; // 下标越界或同一输入被返回两次，都破坏一一对应关系。
            }
            else
            {
                results[current] = {optimized_gates, dag, true}; // 以 Rust 输出的索引写入对应槽。
            }
            current = -1; // 清空 record 状态，准备读取下一个。
            optimized_gates = -1; // 清空门数占位。
            dag.clear(); // 清空上一段 DAG 文本。
        };

        std::string line; // 逐行读取 batch 文件。
        while ( std::getline( result_file, line ) )
        {
            if ( line.empty() )
            {
                finish_record(); // 空行表示一个 DAG record 结束。
            }
            else if ( current < 0 )
            {
                char trailing = 0; // 用于拒绝头部多出的非空字段。
                if ( std::sscanf( line.c_str(), "%d %d %c", &current, &optimized_gates, &trailing ) != 2 )
                    malformed = true; // 第一行必须严格是“候选下标 优化后门数”。
            }
            else
            {
                dag += line + "\n"; // 非头部行原样保留，稍后交给 DAG 解析器。
            }
        }
        finish_record(); // 文件末尾没有空行时，提交最后一个 record。

        for ( auto const& r : results )
            if ( !r.seen ) malformed = true; // 每个输入候选都必须恰好得到一个输出记录。
        if ( malformed )
            break; // 遇到任何格式异常，整批放弃以保持安全性。
        stats.batch_complete = true; // 此批输出完整且可被安全索引。

        // 严格减门候选优先；实验开关允许将一个零门数收益候选也送入完整
        // IMC 管道，检验“门数不变但生命周期更好”的假设。它绝不绕过后续
        // 等价证明、调度、回填或全局 Pareto 判断。
        const bool allow_zero_gain = std::getenv( "IMC_EGRAPH_ALLOW_ZERO_GAIN" ) != nullptr;
        int selected = -1; // 暂无局部门数严格减少的候选。
        int best_local_gain = 0; // 当前最大的 MFFC 门数减少量。
        int first_zero_gain = -1; // 固定遍历序中的第一个零收益候选，保证可复现。
        for ( int i = 0; i < static_cast<int>( candidates.size() ); ++i )
        {
            const int local_gain = static_cast<int>( candidates[i].mffc.size() ) - results[i].gates; // 原 MFFC 门数减 Rust 结果门数。
            if ( results[i].gates >= 0 && local_gain > best_local_gain )
            {
                selected = i; // 记录当前局部收益最大的候选位置。
                best_local_gain = local_gain; // 更新最大收益。
            }
            else if ( allow_zero_gain && results[i].gates >= 0 && local_gain == 0 && first_zero_gain < 0 )
                first_zero_gain = i;
        }
        if ( selected < 0 && first_zero_gain >= 0 )
            selected = first_zero_gain; // 无减门候选时，才测试确定性的零收益候选。
        if ( selected < 0 )
            break; // 无可提交候选；默认仍只接受严格减门实现。

        std::ofstream optimized_file( "imc_egraph_tmp_opt.dag" ); // 写出唯一选中的 e-graph DAG，供两个重建阶段复用。
        optimized_file << results[selected].dag; // 原样保存 Rust 提取的局部实现。
        optimized_file.close(); // 刷盘后再启动解析。

        // [06-ADD][correctness] 对“原 MFFC 函数 = Rust 返回函数”做局部
        // 穷尽等价证明。MFFC 边界最多 12 输入，枚举全部 2^n 组合；这是
        // 确定性的完整证明，不依赖可能长时间不返回的 SAT 求解器。
        std::ofstream original_file( "imc_egraph_tmp_orig.dag" ); // 同样导出选中候选的原 MFFC，建立证明用独立网络。
        ExportMFFCForEgraph( xmg, candidates[selected].gate,
                             candidates[selected].mffc,
                             candidates[selected].leaves, original_file );
        original_file.close(); // 确保原 DAG 文件完整。
        xmg_network proof_network = RebuildXmgFromNumbered( "imc_egraph_tmp_orig.dag" ); // 将原 MFFC 重建为小型可独立求值的 XMG。
        std::vector<uint32_t> proof_leaves; // 存放证明网络 PI 的稳定编号。
        proof_network.foreach_pi( [&]( auto const& pi ) {
            proof_leaves.push_back( proof_network.node_to_index( pi ) ); // 顺序即模式位 pattern 的位顺序。
        } );
        xmg_network::signal original_root = proof_network.get_constant( false ); // 初始化为占位值。
        bool found_proof_output = false; // 检查导出 DAG 是否确实带有输出。
        proof_network.foreach_po( [&]( auto const& po ) {
            if ( !found_proof_output )
            {
                original_root = po; // MFFC 导出仅有一个根输出，取第一个即可。
                found_proof_output = true; // 记住已经找到输出。
            }
        } );
        bool proof_parsed = false; // RebuildInNetwork 通过此标记报告格式是否合法。
        const auto proof_replacement = RebuildInNetwork(
            proof_network, proof_leaves, "imc_egraph_tmp_opt.dag", &proof_parsed );
        if ( !found_proof_output || !proof_parsed )
            break; // 原 DAG 无输出或优化 DAG 不可解析，均不能继续。
        const uint64_t assignments = uint64_t{ 1 } << proof_leaves.size(); // 计算全部 PI 赋值数量。
        bool equivalent = true; // 假设二者相等，发现任一反例即置 false。
        std::vector<bool> values( proof_network.size(), false ); // 每次赋值下每个 XMG 节点的布尔值缓存。
        for ( uint64_t pattern = 0; pattern < assignments && equivalent; ++pattern )
        {
            // [06-ADD][correctness] 直接按 XMG 拓扑求值，避免对每个模式
            // 调用通用 simulate 产生大量临时分配。constant 节点默认 false。
            std::fill( values.begin(), values.end(), false ); // 清空上一组赋值的节点值。
            size_t input_index = 0; // pattern 的 bit 位置。
            proof_network.foreach_pi( [&]( auto const& pi ) {
                values[proof_network.node_to_index( pi )] =
                    ( ( pattern >> input_index++ ) & 1 ) != 0; // 将 pattern 的下一位赋给对应 PI。
            } );
            bool valid_xmg = true; // 防御式检查重建网络是否仍符合三输入 XMG 假设。
            proof_network.foreach_gate( [&]( auto const& node ) {
                bool fanins[3] = { false, false, false };
                int fanin_count = 0;
                proof_network.foreach_fanin( node, [&]( auto const& f ) {
                    if ( fanin_count >= 3 ) { valid_xmg = false; return; } // XMG 门不应多于三个 fanin。
                    bool value = values[proof_network.node_to_index( proof_network.get_node( f ) )];
                    fanins[fanin_count++] = proof_network.is_complemented( f ) ? !value : value; // 将边反相纳入输入真值。
                } );
                if ( fanin_count != 3 ) { valid_xmg = false; return; } // XMG 门必须恰有三个 fanin。
                values[proof_network.node_to_index( node )] = proof_network.is_xor3( node )
                    ? ( fanins[0] ^ fanins[1] ^ fanins[2] )
                    : ( ( fanins[0] && fanins[1] ) || ( fanins[0] && fanins[2] ) || ( fanins[1] && fanins[2] ) );
            } );
            auto signal_value = [&]( auto const& signal ) { // 统一读取任意 XMG signal（含可能的反相位）。
                bool value = values[proof_network.node_to_index( proof_network.get_node( signal ) )]; // 获取底层节点值。
                return proof_network.is_complemented( signal ) ? !value : value; // signal 被反相时返回补值。
            };
            if ( !valid_xmg || signal_value( original_root ) != signal_value( proof_replacement ) )
                equivalent = false; // 找到结构非法或函数不等价的反例。
        }
        if ( !equivalent ) break; // 证明未通过时保持原 xmg 完全不变。

        // [06-ADD][correctness] 完整枚举证明通过后，才在完整子网的克隆
        // 中重建。解析或等价检查失败时，绝不修改真实候选。
        // cleanup_dangling 会重建网络并改变节点编号；先冻结“替换前根”的
        // 编号，保证 k=2/3 的审计日志指向本 pass 真正选择的候选。
        const uint32_t selected_root_index = xmg.node_to_index( candidates[selected].gate );
        xmg_network trial = xmg.clone(); // 所有真正写回前的修改先作用于克隆。
        bool parsed = false; // 记录优化 DAG 是否能在完整子网的边界上重建。
        const auto replacement = RebuildInNetwork(
            trial, candidates[selected].leaves, "imc_egraph_tmp_opt.dag", &parsed );
        if ( !parsed )
            break; // 端口/编号不一致时安全退出。

        trial.substitute_node( candidates[selected].gate, replacement ); // 用已证明等价的 signal 替换原 MFFC 根。
        xmg = mockturtle::cleanup_dangling( trial ); // 清除旧 MFFC 留下的无用户节点，并提交到真实子网。
        // [06-ADD][measurement] 只记录最终实际采用的替换特征，而非把 batch
        // 中未验证/未选中的候选混入统计；后续可与全局调度结果逐条对应。
        stats.selected_mffc_gates_total += static_cast<int>( candidates[selected].mffc.size() ); // 累计实际替换的内部规模。
        stats.selected_boundary_inputs_total += static_cast<int>( candidates[selected].leaves.size() ); // 累计实际替换的边界宽度。
        stats.selected_boundary_inputs_max = std::max(
            stats.selected_boundary_inputs_max, static_cast<int>( candidates[selected].leaves.size() ) );
        stats.selected_local_gain_total += best_local_gain; // 累计局部门数收益。
        stats.selected_root_indices.push_back( selected_root_index ); // 使用 cleanup 前冻结的编号。
        stats.selected_mffc_gate_counts.push_back( static_cast<int>( candidates[selected].mffc.size() ) );
        stats.selected_boundary_input_counts.push_back( static_cast<int>( candidates[selected].leaves.size() ) );
        stats.selected_local_gains.push_back( best_local_gain );
        if ( best_local_gain == 0 )
            ++stats.zero_gain_replacements;
        stats.equivalence_proven = true; // 至少一次替换已通过完整证明。
        ++stats.replacements; // 计入本函数允许的替换次数。
    }

    stats.gates_after = static_cast<int>( xmg.num_gates() ); // 记录最终子网门数。
    DescribeEgraphTopology( xmg, stats.max_depth_after, stats.max_fanout_after ); // 对最终实现做同口径拓扑统计。
    return stats; // 调用者据此记录实验并决定是否开展后续 IMC A/B 比较。
}
