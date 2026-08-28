// ============================================================
// [05-ADD] XMG e-graph —— per-cut 优化引擎（里程碑 2）
//   与 04-egraph_v2 的区别：
//   04 是"全电路一次饱和 + Per-PO 提取"（有 DAG 共享破坏问题）；
//   这里是"单个 cut 的小 DAG 饱和 + 提取唯一 root"，避开全电路问题。
//
//   输入：单个 cut 的编号 DAG（PI 行 = leaves, C0/C1, X/M/N 门, P 行 = root）
//   输出：优化后的编号 DAG（同一格式，供 C++ 端 RebuildXmgFromNumbered 解析）
// ============================================================

use egg::{rewrite as rw, *};
use std::collections::HashMap;
use std::env;
use std::fs;

// ------------------------------------------------------------
// 重写规则集（安全子集，先验证管道，后续逐步加 distri/associ 等"新基"规则）
// 符号：maj3 = 三输入多数门, xor3 = 三输入异或门, not = 取反
// 叶子：0/1 = 常量, pi_N = 第 N 个 leaf（cut 的边界）
// ------------------------------------------------------------
fn rules() -> Vec<Rewrite<SymbolLang, ()>> {
    vec![
        // --- 交换律（换序不减门，但让其他规则命中）---
        rw!("maj-comm-01"; "(maj3 ?a ?b ?c)" => "(maj3 ?b ?a ?c)"),
        rw!("maj-comm-12"; "(maj3 ?a ?b ?c)" => "(maj3 ?a ?c ?b)"),
        rw!("xor-comm-01"; "(xor3 ?a ?b ?c)" => "(xor3 ?b ?a ?c)"),
        rw!("xor-comm-12"; "(xor3 ?a ?b ?c)" => "(xor3 ?a ?c ?b)"),

        // --- 非门 ---
        rw!("not-not"; "(not (not ?a))" => "?a"),
        rw!("not-0";    "(not 0)" => "1"),
        rw!("not-1";    "(not 1)" => "0"),
        // 非门分配进 maj（maj 取反 = 三输入全取反）
        rw!("not-maj"; "(not (maj3 ?a ?b ?c))" => "(maj3 (not ?a) (not ?b) (not ?c))"),
        // 非门分配进 xor（xor 取反 = 只取反一个输入，取反一个即可）
        rw!("not-xor"; "(not (xor3 ?a ?b ?c))" => "(xor3 (not ?a) ?b ?c)"),

        // --- maj3 常量折叠 / 幂等 / 互补 ---
        rw!("maj-000"; "(maj3 0 0 ?x)" => "0"),
        rw!("maj-111"; "(maj3 1 1 ?x)" => "1"),
        rw!("maj-dup";  "(maj3 ?a ?a ?b)" => "?a"),
        rw!("maj-comp"; "(maj3 ?a (not ?a) ?b)" => "?b"),

        // --- xor3 常量折叠 / 幂等 / 互补 ---
        rw!("xor-000"; "(xor3 0 0 ?x)" => "?x"),
        rw!("xor-111"; "(xor3 1 1 ?x)" => "?x"),
        rw!("xor-dup";  "(xor3 ?a ?a ?b)" => "?b"),
        rw!("xor-comp"; "(xor3 ?a (not ?a) ?b)" => "(not ?b)"),

        // --- "新基"规则（distri/associ/relevance，来自 eLogic DATE'26，真正改变结构）---
        rw!("distri"; "(maj3 ?a ?b (maj3 ?c ?d ?e))" => "(maj3 (maj3 ?a ?b ?c) (maj3 ?a ?b ?d) ?e)"),
        rw!("distri-flip"; "(maj3 (maj3 ?a ?b ?c) (maj3 ?a ?b ?d) ?e)" => "(maj3 ?a ?b (maj3 ?c ?d ?e))"),
        rw!("com-associ"; "(maj3 ?a ?b (maj3 ?c (not ?b) ?d))" => "(maj3 ?a ?b (maj3 ?c ?a ?d))"),
        rw!("com-associ-flip"; "(maj3 ?a ?b (maj3 ?c ?a ?d))" => "(maj3 ?a ?b (maj3 ?c (not ?b) ?d))"),
        rw!("associ"; "(maj3 ?a ?b (maj3 ?c ?b ?d))" => "(maj3 ?d ?b (maj3 ?c ?b ?a))"),
        rw!("relevance"; "(maj3 ?a ?b (maj3 ?c ?d (maj3 ?a ?b ?e)))" => "(maj3 ?a ?b (maj3 ?c ?d (maj3 (not ?b) ?b ?e)))"),
        rw!("maj-com-equ"; "(maj3 ?a ?b (not ?b))" => "(maj3 ?a ?b ?a)"),
        rw!("maj-com-equ-flip"; "(maj3 ?a ?b ?a)" => "(maj3 ?a ?b (not ?b))"),

        // --- XMG 特有 MAJ-XOR 恒等式（Structural Rewriting in XMG, ASP-DAC 2019）---
        //   这是 XMG 相对 MIG（eLogic）的独特优势，2 输入 XOR 用 xor3(x,u,0) 表达。
        // Theorem 1 (Eqn 10): XOR 分配 over MAJ。RL 方向 4门→2门（减门关键）。
        //   maj3(x⊕u, y⊕u, z⊕u) = maj3(x,y,z) ⊕ u
        rw!("maj-xor-distri"; "(maj3 (xor3 ?x ?u 0) (xor3 ?y ?u 0) (xor3 ?z ?u 0))" => "(xor3 (maj3 ?x ?y ?z) ?u 0)"),
        rw!("maj-xor-distri-flip"; "(xor3 (maj3 ?x ?y ?z) ?u 0)" => "(maj3 (xor3 ?x ?u 0) (xor3 ?y ?u 0) (xor3 ?z ?u 0))"),
        // Theorem 2 (Eqn 14): 互补结合律。处理 y/ȳ 重汇聚，移除反相器。
        //   maj3(x, y, ȳ⊕z) = maj3(x, y, x⊕z)
        rw!("maj-xor-assoc"; "(maj3 ?x ?y (xor3 (not ?y) ?z 0))" => "(maj3 ?x ?y (xor3 ?x ?z 0))"),
        rw!("maj-xor-assoc-flip"; "(maj3 ?x ?y (xor3 ?x ?z 0))" => "(maj3 ?x ?y (xor3 (not ?y) ?z 0))"),
    ]
}

// 门数代价：非叶子节点计 1，叶子计 0
struct OpCount;
impl CostFunction<SymbolLang> for OpCount {
    type Cost = usize;
    fn cost<C>(&mut self, enode: &SymbolLang, mut costs: C) -> usize
    where
        C: FnMut(Id) -> usize,
    {
        let s: usize = enode.children().iter().map(|&id| costs(id)).sum();
        if enode.is_leaf() {
            s
        } else {
            1 + s
        }
    }
}

// 解析编号 DAG，返回 (RecExpr, 每个 flat index 的 Id, root 的 flat index)
fn parse(input: &str) -> Option<(RecExpr<SymbolLang>, Vec<Id>, usize)> {
    let mut e = RecExpr::default();
    let mut ids: Vec<Id> = Vec::new();
    let mut dedup: HashMap<SymbolLang, Id> = HashMap::new();
    let mut root: usize = 0;
    // [05-MOD] 对相同 e-node 去重（如两个 "N 0" 都是 not(pi_0)），
    //   否则 egg 0.9.5 在 RecExpr 有重复节点时 UnionFind 越界崩溃。
    let mut add = |enode: SymbolLang| -> Id {
        if let Some(&id) = dedup.get(&enode) {
            return id;
        }
        let id = e.add(enode.clone());
        dedup.insert(enode, id);
        id
    };
    for l in input.lines() {
        let p: Vec<&str> = l.trim().split_whitespace().collect();
        if p.is_empty() {
            continue;
        }
        match p[0] {
            "P" => {
                root = p.get(1)?.parse().ok()?; // P <flat_index>
            }
            "PI" => ids.push(add(SymbolLang::leaf(&format!("pi_{}", p.get(1).unwrap_or(&"?"))))),
            "C0" => ids.push(add(SymbolLang::leaf("0"))),
            "C1" => ids.push(add(SymbolLang::leaf("1"))),
            "X" | "M" if p.len() >= 4 => {
                let a: usize = p[1].parse().ok()?;
                let b: usize = p[2].parse().ok()?;
                let c: usize = p[3].parse().ok()?;
                if a >= ids.len() || b >= ids.len() || c >= ids.len() {
                    return None;
                }
                let op = if p[0] == "X" { "xor3" } else { "maj3" };
                ids.push(add(SymbolLang::new(op, vec![ids[a], ids[b], ids[c]])));
            }
            "N" if p.len() >= 2 => {
                let a: usize = p[1].parse().ok()?;
                if a >= ids.len() {
                    return None;
                }
                ids.push(add(SymbolLang::new("not", vec![ids[a]])));
            }
            _ => {} // 跳过 PI_COUNT / GATE_COUNT / PI_MAP 等头
        }
    }
    Some((e, ids, root))
}

// 把提取出的 RecExpr 树转成编号行（X/M/N），去重；返回 (门行列表, root flat index)
fn tree_to_lines(rec: &RecExpr<SymbolLang>, pi: usize) -> (Vec<String>, usize) {
    let mut dedup: HashMap<String, usize> = HashMap::new();
    let mut n2f: Vec<usize> = vec![0; rec.as_ref().len()];
    let mut lines: Vec<String> = Vec::new();
    let mut next = pi + 2; // PIs 占 0..pi, C0=pi, C1=pi+1
    let mut root_fidx = pi; // 默认（若 root 退化成常量）

    for (j, node) in rec.as_ref().iter().enumerate() {
        if node.is_leaf() {
            let s = node.to_string();
            n2f[j] = if s == "0" {
                pi
            } else if s == "1" {
                pi + 1
            } else if s.starts_with("pi_") {
                s[3..].parse().unwrap_or(0).min(pi - 1)
            } else {
                pi
            };
        } else {
            let ch: Vec<usize> = node.children().iter().map(|&c| n2f[usize::from(c)]).collect();
            let key = format!("{} {:?}", node.op.to_string(), ch);
            if let Some(&ex) = dedup.get(&key) {
                n2f[j] = ex;
            } else {
                let fidx = next;
                next += 1;
                n2f[j] = fidx;
                dedup.insert(key, fidx);
                match node.op.to_string().as_str() {
                    "not" if ch.len() >= 1 => lines.push(format!("N {}", ch[0])),
                    "xor3" if ch.len() >= 3 => lines.push(format!("X {} {} {}", ch[0], ch[1], ch[2])),
                    "maj3" if ch.len() >= 3 => lines.push(format!("M {} {} {}", ch[0], ch[1], ch[2])),
                    _ => {}
                }
            }
        }
    }
    // 最后处理到的节点即 root（RecExpr 根在最后）
    if let Some(last) = rec.as_ref().last() {
        let j = rec.as_ref().len() - 1;
        root_fidx = n2f[j];
    }
    (lines, root_fidx)
}

// 优化单个 cut 的编号 DAG，返回 (优化后 X/M 门数, 优化后完整 DAG)。
//   门数 < 0 表示失败：-1=parse失败，-2=达到node_limit。
fn optimize_one(input: &str) -> (isize, String) {
    let pi: usize = input
        .lines()
        .filter(|l| l.trim().starts_with("PI_COUNT"))
        .flat_map(|l| l.split_whitespace().nth(1).map(|s| s.parse().unwrap_or(0)))
        .next()
        .unwrap_or(0);

    let (expr, ids, root) = match parse(input) {
        Some(x) => x,
        None => return (-1, String::new()),
    };
    if root >= ids.len() {
        return (-1, String::new());
    }

    // [05-MOD] 限制 e-node 数量与迭代次数，防止 distri/associ 双向规则在
    //   某些 cut 上导致 e-class 爆炸（饱和极慢）。
    let runner = Runner::default()
        .with_expr(&expr)
        .with_node_limit(10000)
        .with_iter_limit(10)
        .run(&rules());
    // 达到 node limit 说明饱和不完整，结果不可靠，返回 -2（不参与省门统计）
    if matches!(runner.stop_reason, Some(egg::StopReason::NodeLimit(_))) {
        return (-2, String::new());
    }
    let extractor = Extractor::new(&runner.egraph, OpCount);
    let root_ec = runner.egraph.find(ids[root]);
    let (_cost, best) = extractor.find_best(root_ec);

    let (lines, root_fidx) = tree_to_lines(&best, pi);
    let opt_gates = lines.iter().filter(|l| l.starts_with("X ") || l.starts_with("M ")).count() as isize;

    // 构造完整 DAG
    let mut dag = String::new();
    dag.push_str(&format!("PI_COUNT {}\nGATE_COUNT {}\n", pi, lines.len()));
    for i in 0..pi {
        dag.push_str(&format!("PI {}\n", i));
    }
    dag.push_str("C0\nC1\n");
    for l in &lines {
        dag.push_str(l);
        dag.push('\n');
    }
    dag.push_str(&format!("P {}\n", root_fidx));
    (opt_gates, dag)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: xmg_egraph_cut <in.dag> <out.dag>");
        std::process::exit(1);
    }
    let input = fs::read_to_string(&args[1]).expect("read input");
    // [05-MOD] Windows 下 ofstream 把 \n 转成 \r\n，必须先统一换行符，
    //   否则 split("\n\n") 找不到分隔符，整个批处理文件被当成一个 DAG。
    let input = input.replace("\r\n", "\n");

    // 批处理：输入多个 DAG（空行分隔），逐个优化，输出用 ===RESULT i=== 分隔
    let dags: Vec<&str> = input.split("\n\n").filter(|d| !d.trim().is_empty()).collect();
    let mut output = String::new();
    for (i, dag) in dags.iter().enumerate() {
        if i % 100 == 0 {
            eprintln!("  ... processing cut {} / {}", i, dags.len());
        }
        let (opt_gates, opt_dag) = optimize_one(dag);
        output.push_str(&format!("{} {}\n", i, opt_gates));
        if opt_gates >= 0 {
            output.push_str(&opt_dag);
        }
        output.push('\n'); // 空行分隔（无论 opt_gates 正负，保证 C++ 解析一致）
    }
    fs::write(&args[2], &output).expect("write output");
    eprintln!("batch optimized {} cuts", dags.len());
}
