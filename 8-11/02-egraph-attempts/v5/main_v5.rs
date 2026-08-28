//! XMG E-graph — structural variants via commutativity
use egg::{*, rewrite as rw};
use std::env; use std::fs; use std::io; use std::collections::HashMap;

fn rules() -> Vec<Rewrite<SymbolLang, ()>> { vec![
    rw!("xor-comm-12"; "(xor3 ?a ?b ?c)" => "(xor3 ?b ?a ?c)"),
    rw!("xor-comm-23"; "(xor3 ?a ?b ?c)" => "(xor3 ?a ?c ?b)"),
    rw!("maj-comm-12"; "(maj3 ?a ?b ?c)" => "(maj3 ?b ?a ?c)"),
    rw!("maj-comm-23"; "(maj3 ?a ?b ?c)" => "(maj3 ?a ?c ?b)"),
    rw!("xor-idem";    "(xor3 ?a ?a ?b)" => "?b"),
    rw!("maj-idem";    "(maj3 ?a ?a ?b)" => "?a"),
    rw!("not-not";     "(not (not ?x))" => "?x"),
    rw!("xor-zero-id"; "(xor3 0 0 ?a)" => "?a"),
    rw!("maj-zero-one";"(maj3 0 1 ?a)" => "?a"),
    rw!("maj-zero-zero";"(maj3 0 0 ?a)" => "0"),
    rw!("maj-one-one"; "(maj3 1 1 ?a)" => "1"),
    rw!("maj-not-self";"(maj3 ?a (not ?a) ?b)" => "?b"),
]}

fn parse(input:&str)->Option<(RecExpr<SymbolLang>,Vec<Id>)>{
    let mut e=RecExpr::default();let mut ids=Vec::new();
    for l in input.lines(){
        let p:Vec<&str>=l.trim().split_whitespace().collect();
        if p.is_empty()||p[0]=="P"{continue}
        match p[0]{
            "PI"=>ids.push(e.add(SymbolLang::leaf(&format!("pi_{}",p.get(1).unwrap_or(&"?"))))),
            "C0"=>ids.push(e.add(SymbolLang::leaf("0"))),"C1"=>ids.push(e.add(SymbolLang::leaf("1"))),
            "X"|"M" if p.len()>=4=>{
                let a:usize=p[1].parse().ok()?;let b:usize=p[2].parse().ok()?;let c:usize=p[3].parse().ok()?;
                if a>=ids.len()||b>=ids.len()||c>=ids.len(){return None}
                ids.push(e.add(SymbolLang::new(if p[0]=="X"{"xor3"}else{"maj3"},vec![ids[a],ids[b],ids[c]])));
            }
            "N" if p.len()>=2=>{
                let a:usize=p[1].parse().ok()?;if a>=ids.len(){return None}
                ids.push(e.add(SymbolLang::new("not",vec![ids[a]])));
            }
            _=>{}
        }
    }
    Some((e,ids))
}

fn main()->io::Result<()>{
    let args:Vec<String>=env::args().collect();
    if args.len()<2{eprintln!("xmg_egraph <in> [out]");std::process::exit(1);}
    let input=fs::read_to_string(&args[1])?;
    let pi:usize=input.lines().filter(|l|l.trim().starts_with("PI_COUNT"))
        .flat_map(|l|l.split_whitespace().nth(1).map(|s|s.parse().unwrap_or(0))).next().unwrap_or(0);
    let body:String=input.lines()
        .filter(|l|{let t=l.trim();t.starts_with("PI ")||t.starts_with("C0")||t.starts_with("C1")||t.starts_with("X ")||t.starts_with("M ")||t.starts_with("N ")})
        .collect::<Vec<_>>().join("\n");
    let po_lines:Vec<String>=input.lines()
        .filter(|l|{let t=l.trim();t.starts_with("P ")&&!t.starts_with("PI_COUNT")&&!t.starts_with("PI_MAP")&&!t.starts_with("PI ")})
        .map(|l|l.to_string()).collect();

    let (expr,ids)=match parse(&body){Some(x)=>x,None=>{eprintln!("Parse err");std::process::exit(1);}};
    let runner=Runner::default().with_expr(&expr).run(&rules());
    println!("=== XMG E-graph ===\nPI={} e-class={} e-node={}",pi,
             runner.egraph.number_of_classes(),runner.egraph.total_number_of_nodes());

    // Build Id → flat-index reverse map (each original gate/PI has a unique Id)
    let mut id2flat:HashMap<Id,usize>=HashMap::new();
    let mut fidx=0usize;
    for (i,_line) in body.lines().enumerate(){
        id2flat.insert(ids[i], fidx);
        fidx+=1;
    }

    // Replace each gate with its commutativity variant if children order differs
    let mut lines:Vec<String>=Vec::new();
    let mut variants=0usize;
    for (i,line) in body.lines().enumerate(){
        let t=line.trim();
        if t.starts_with("PI ")||t.starts_with("C0")||t.starts_with("C1"){lines.push(line.to_string());continue;}
        let eclass=runner.egraph.find(ids[i]);
        let node=&runner.egraph[eclass].nodes[0];
        if !node.is_leaf()&&(node.op.to_string()=="xor3"||node.op.to_string()=="maj3"){
            let ch:Vec<usize>=node.children().iter()
                .filter_map(|&c|id2flat.get(&c).copied())
                .collect();
            if ch.len()>=3{
                let prefix=if node.op.to_string()=="xor3"{"X"}else{"M"};
                let nl=format!("{} {} {} {}",prefix,ch[0],ch[1],ch[2]);
                if nl!=t{variants+=1;lines.push(nl);continue;}
            }
        }
        lines.push(line.to_string());
    }
    let gate_count=lines.iter().filter(|l|{let t=l.trim();t.starts_with("X ")||t.starts_with("M ")||t.starts_with("N ")}).count();
    println!("   gates: {} (variants: {})",gate_count,variants);

    let mut out=String::new();
    out.push_str(&format!("PI_COUNT {}\nGATE_COUNT {}\n",pi,gate_count));
    for l in input.lines(){if l.trim().starts_with("PI_MAP"){out.push_str(l);out.push('\n');}}
    for l in &lines{out.push_str(l);out.push('\n');}
    for l in &po_lines{out.push_str(l);out.push('\n');}
    if args.len()>=3{fs::write(&args[2],&out)?;}
    Ok(())
}
