//! XMG E-graph — v5.3: DAG-preserving + full rule set
//!
//! Strategy (v5 architecture, v5.2 rules):
//!   Parse full sub-netlist → e-graph saturation → for each gate in
//!   topological order pick the cheapest equivalent e-node whose
//!   children are already mapped → output remapped numbered DAG.
//!
//! Gates that saturate to a leaf (PI/constant) are dropped and
//! downstream references are remapped to the leaf's index.

use egg::{*, rewrite as rw};
use std::env;
use std::fs;
use std::io;
use std::collections::HashMap;

// ── 18 XMG rewrite rules ──────────────────────────────────
fn rules() -> Vec<Rewrite<SymbolLang, ()>> {
    vec![
        rw!("xor-comm-12"; "(xor3 ?a ?b ?c)" => "(xor3 ?b ?a ?c)"),
        rw!("xor-comm-23"; "(xor3 ?a ?b ?c)" => "(xor3 ?a ?c ?b)"),
        rw!("xor-idem";    "(xor3 ?a ?a ?b)" => "?b"),
        rw!("maj-comm-12"; "(maj3 ?a ?b ?c)" => "(maj3 ?b ?a ?c)"),
        rw!("maj-comm-23"; "(maj3 ?a ?b ?c)" => "(maj3 ?a ?c ?b)"),
        rw!("maj-idem";    "(maj3 ?a ?a ?b)" => "?a"),
        rw!("not-not";     "(not (not ?x))" => "?x"),
        rw!("xor-zero-id"; "(xor3 0 0 ?a)" => "?a"),
        rw!("xor-one-flip";"(xor3 1 0 ?a)" => "(not ?a)"),
        rw!("xor-one-one"; "(xor3 1 1 ?a)" => "?a"),
        rw!("maj-zero-one";"(maj3 0 1 ?a)" => "?a"),
        rw!("maj-zero-zero";"(maj3 0 0 ?a)" => "0"),
        rw!("maj-one-one"; "(maj3 1 1 ?a)" => "1"),
        rw!("maj-not-self";"(maj3 ?a (not ?a) ?b)" => "?b"),
        rw!("not-xor";     "(not (xor3 ?a ?b ?c))" => "(xor3 (not ?a) ?b ?c)"),
        rw!("not-maj";     "(not (maj3 ?a ?b ?c))" => "(maj3 (not ?a) (not ?b) (not ?c))"),
        rw!("xor-absorb";  "(xor3 (xor3 ?a ?b ?c) ?b ?c)" => "?a"),
    ]
}

// ── Cost model: count operators ───────────────────────────
struct OpCount;
impl CostFunction<SymbolLang> for OpCount {
    type Cost = usize;
    fn cost<C>(&mut self, enode: &SymbolLang, mut costs: C) -> usize
    where
        C: FnMut(Id) -> usize,
    {
        let child_sum: usize = enode.children().iter().map(|&id| costs(id)).sum();
        if enode.is_leaf() {
            child_sum
        } else {
            1 + child_sum
        }
    }
}

// ── Parser: numbered-DAG body → egg RecExpr + Id vector ───
fn parse(input: &str) -> Option<(RecExpr<SymbolLang>, Vec<Id>)> {
    let mut e = RecExpr::default();
    let mut ids: Vec<Id> = Vec::new();

    for l in input.lines() {
        let p: Vec<&str> = l.trim().split_whitespace().collect();
        if p.is_empty() || p[0] == "P" {
            continue;
        }
        match p[0] {
            "PI" => {
                let name = format!("pi_{}", p.get(1).unwrap_or(&"?"));
                ids.push(e.add(SymbolLang::leaf(&name)));
            }
            "C0" => ids.push(e.add(SymbolLang::leaf("0"))),
            "C1" => ids.push(e.add(SymbolLang::leaf("1"))),
            "X" | "M" if p.len() >= 4 => {
                let a: usize = p[1].parse().ok()?;
                let b: usize = p[2].parse().ok()?;
                let c: usize = p[3].parse().ok()?;
                if a >= ids.len() || b >= ids.len() || c >= ids.len() {
                    return None;
                }
                let op = if p[0] == "X" { "xor3" } else { "maj3" };
                ids.push(e.add(SymbolLang::new(op, vec![ids[a], ids[b], ids[c]])));
            }
            "N" if p.len() >= 2 => {
                let a: usize = p[1].parse().ok()?;
                if a >= ids.len() {
                    return None;
                }
                ids.push(e.add(SymbolLang::new("not", vec![ids[a]])));
            }
            _ => {}
        }
    }
    Some((e, ids))
}

// ── leaf e-class → canonical flat index ───────────────────
fn leaf_fidx(egraph: &EGraph<SymbolLang, ()>, ec_id: Id, pi: usize) -> Option<usize> {
    // Search the e-class for a leaf matching a known constant/PI.
    for node in &egraph[ec_id].nodes {
        if node.is_leaf() {
            let s = node.to_string();
            if s == "0" {
                return Some(pi); // C0
            } else if s == "1" {
                return Some(pi + 1); // C1
            } else if s.starts_with("pi_") {
                let n: usize = s[3..].parse().ok()?;
                if n < pi {
                    return Some(n);
                }
            }
        }
    }
    None
}

// ── Main ──────────────────────────────────────────────────
fn main() -> io::Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("xmg_egraph <in> [out]");
        std::process::exit(1);
    }
    let input = fs::read_to_string(&args[1])?;

    // --- parse header ---
    let pi: usize = input
        .lines()
        .filter(|l| l.trim().starts_with("PI_COUNT"))
        .flat_map(|l| l.split_whitespace().nth(1).map(|s| s.parse().unwrap_or(0)))
        .next()
        .unwrap_or(0);

    let body: String = input
        .lines()
        .filter(|l| {
            let t = l.trim();
            t.starts_with("PI ")
                || t.starts_with("C0")
                || t.starts_with("C1")
                || t.starts_with("X ")
                || t.starts_with("M ")
                || t.starts_with("N ")
        })
        .collect::<Vec<_>>()
        .join("\n");

    let po_lines: Vec<String> = input
        .lines()
        .filter(|l| {
            let t = l.trim();
            t.starts_with("P ")
                && !t.starts_with("PI_COUNT")
                && !t.starts_with("PI_MAP")
                && !t.starts_with("PI ")
        })
        .map(|l| l.to_string())
        .collect();

    let po_idxs: Vec<usize> = po_lines
        .iter()
        .flat_map(|l| l.split_whitespace().nth(1).map(|s| s.parse().unwrap_or(0)))
        .collect();

    // --- parse & saturate ---
    let (expr, ids) = match parse(&body) {
        Some(x) => x,
        None => {
            eprintln!("Parse err");
            std::process::exit(1);
        }
    };
    let runner = Runner::default().with_expr(&expr).run(&rules());
    let extractor = Extractor::new(&runner.egraph, OpCount);
    let n_parsed = ids.len(); // PI*N + C0 + C1 + gates
    let orig_gates = body
        .lines()
        .filter(|l| {
            let t = l.trim();
            t.starts_with("X ") || t.starts_with("M ") || t.starts_with("N ")
        })
        .count();

    println!(
        "=== XMG E-graph v5.3 ===\nPI={}  PO={}  orig-gates={}  e-class={}  e-node={}",
        pi,
        po_idxs.len(),
        orig_gates,
        runner.egraph.number_of_classes(),
        runner.egraph.total_number_of_nodes()
    );

    // --- fidx2new[orig_flat_idx] = new_flat_idx ---
    let mut fidx2new: Vec<usize> = vec![usize::MAX; n_parsed];

    // --- id2fidx: canonical e-class Id → best new flat index ---
    let mut id2fidx: HashMap<Id, usize> = HashMap::new();

    // --- output lines ---
    let mut out_lines: Vec<String> = Vec::new();
    let mut next_fidx: usize = 0;

    // PIs: orig flat 0..pi-1 → new flat 0..pi-1
    for i in 0..pi {
        out_lines.push(format!("PI {}", i));
        fidx2new[i] = next_fidx;
        id2fidx.insert(runner.egraph.find(ids[i]), next_fidx);
        next_fidx += 1;
    }
    // C0: orig flat `pi` → `pi`
    out_lines.push("C0".to_string());
    fidx2new[pi] = next_fidx;
    id2fidx.insert(runner.egraph.find(ids[pi]), next_fidx);
    next_fidx += 1;
    // C1: orig flat `pi+1` → `pi+1`
    out_lines.push("C1".to_string());
    fidx2new[pi + 1] = next_fidx;
    id2fidx.insert(runner.egraph.find(ids[pi + 1]), next_fidx);
    next_fidx += 1;

    // --- process gates in body (topological) order ---
    let mut skipped = 0usize; // gates dropped (leaf equivalent)
    let mut merged = 0usize;  // gates merged into already-mapped e-class
    let mut improved = 0usize; // gates where we picked a different e-node

    for (i, raw) in body.lines().enumerate() {
        let t = raw.trim();
        if t.starts_with("PI ") || t.starts_with("C0") || t.starts_with("C1") {
            continue; // already handled
        }

        let ec = runner.egraph.find(ids[i]);

        // --- dedup check: if this e-class already has a flat index, remap and skip ---
        if let Some(&existing_fidx) = id2fidx.get(&ec) {
            fidx2new[i] = existing_fidx;
            merged += 1;
            improved += 1;
            continue;
        }

        // --- leaf check: is this gate equivalent to a PI or constant? ---
        if let Some(lf) = leaf_fidx(&runner.egraph, ec, pi) {
            // Drop this gate: remap it to the leaf's flat index
            fidx2new[i] = lf;
            id2fidx.insert(ec, lf);
            skipped += 1;
            improved += 1;
            continue; // no output line for this gate
        }

        // --- find best operator e-node whose children are all mapped ---
        let mut best_line: Option<String> = None;
        let mut best_cost: usize = usize::MAX;

        for node in &runner.egraph[ec].nodes {
            if node.is_leaf() {
                continue; // leaf already handled above; shouldn't reach here
            }
            // Get canonical child e-classes
            let child_ecs: Vec<Id> = node.children().iter().map(|&c| runner.egraph.find(c)).collect();

            // All children must be already mapped
            if !child_ecs.iter().all(|cid| id2fidx.contains_key(cid)) {
                continue;
            }

            let child_fidxs: Vec<usize> = child_ecs.iter().map(|cid| id2fidx[cid]).collect();
            let child_cost: usize = child_ecs.iter().map(|cid| extractor.find_best(*cid).0).sum();
            let total_cost = 1 + child_cost;

            // Build output line
            let line = match node.op.to_string().as_str() {
                "xor3" if child_fidxs.len() >= 3 => {
                    format!("X {} {} {}", child_fidxs[0], child_fidxs[1], child_fidxs[2])
                }
                "maj3" if child_fidxs.len() >= 3 => {
                    format!("M {} {} {}", child_fidxs[0], child_fidxs[1], child_fidxs[2])
                }
                "not" if child_fidxs.len() >= 1 => {
                    format!("N {}", child_fidxs[0])
                }
                _ => continue,
            };

            if total_cost < best_cost {
                best_cost = total_cost;
                best_line = Some(line);
            }
        }

        match best_line {
            Some(line) => {
                if line != t {
                    improved += 1;
                }
                out_lines.push(line);
                fidx2new[i] = next_fidx;
                id2fidx.insert(ec, next_fidx);
                next_fidx += 1;
            }
            None => {
                // Fallback: no mapped-children e-node found → keep original
                // (remap child indices through fidx2new)
                let p: Vec<&str> = t.split_whitespace().collect();
                if p.len() >= 2 {
                    let remapped: Vec<String> = p
                        .iter()
                        .enumerate()
                        .map(|(j, &tok)| {
                            if j == 0 {
                                tok.to_string()
                            } else {
                                let orig: usize = tok.parse().unwrap_or(0);
                                if orig < fidx2new.len() && fidx2new[orig] != usize::MAX {
                                    fidx2new[orig].to_string()
                                } else {
                                    tok.to_string()
                                }
                            }
                        })
                        .collect();
                    let fallback = remapped.join(" ");
                    if fallback != t {
                        improved += 1;
                    }
                    out_lines.push(fallback);
                } else {
                    out_lines.push(t.to_string());
                }
                fidx2new[i] = next_fidx;
                id2fidx.insert(ec, next_fidx);
                next_fidx += 1;
            }
        }
    }

    // --- remap PO lines ---
    let mut new_po_lines: Vec<String> = Vec::new();
    for (pi_idx, &orig_fidx) in po_idxs.iter().enumerate() {
        if orig_fidx < fidx2new.len() && fidx2new[orig_fidx] != usize::MAX {
            new_po_lines.push(format!("P {}", fidx2new[orig_fidx]));
        } else {
            // keep original as-is (shouldn't happen)
            new_po_lines.push(po_lines[pi_idx].clone());
        }
    }

    let gate_count = out_lines
        .iter()
        .filter(|l| {
            let t = l.trim();
            t.starts_with("X ") || t.starts_with("M ") || t.starts_with("N ")
        })
        .count();
    println!(
        "   out-gates: {} (orig: {})  skipped: {}  merged: {}  improved: {}",
        gate_count, orig_gates, skipped, merged, improved
    );

    // --- write output ---
    let mut out = String::new();
    out.push_str(&format!("PI_COUNT {}\nGATE_COUNT {}\n", pi, gate_count));
    for l in input.lines() {
        if l.trim().starts_with("PI_MAP") {
            out.push_str(l);
            out.push('\n');
        }
    }
    for l in &out_lines {
        out.push_str(l);
        out.push('\n');
    }
    for l in &new_po_lines {
        out.push_str(l);
        out.push('\n');
    }
    if args.len() >= 3 {
        fs::write(&args[2], &out)?;
    }
    Ok(())
}
