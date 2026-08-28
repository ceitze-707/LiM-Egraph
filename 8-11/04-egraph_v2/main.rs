//! XMG E-graph — per-PO extraction
use egg::{*, rewrite as rw};
use std::env; use std::fs; use std::io; use std::collections::HashMap;

fn rules() -> Vec<Rewrite<SymbolLang, ()>> { vec![] }
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

struct OpCount;
impl CostFunction<SymbolLang> for OpCount {
    type Cost=usize;
    fn cost<C>(&mut self, enode: &SymbolLang, mut costs: C)->usize
    where C:FnMut(Id)->usize {
        let s:usize=enode.children().iter().map(|&id|costs(id)).sum();
        if enode.is_leaf(){s}else{1+s}
    }
}

/// Convert egg RecExpr tree to numbered format lines with dedup
fn tree_to_lines(rec:&RecExpr<SymbolLang>, pi:usize) -> Vec<String> {
    let mut dedup:HashMap<String,usize>=HashMap::new();
    let mut n2f:Vec<usize>=vec![0;rec.as_ref().len()];
    let mut lines:Vec<String>=Vec::new();
    let mut next=pi+2;
    for (j,node) in rec.as_ref().iter().enumerate() {
        if node.is_leaf() {
            let s=node.to_string();
            if s=="0"{n2f[j]=pi;}
            else if s=="1"{n2f[j]=pi+1;}
            else if s.starts_with("pi_"){n2f[j]=s[3..].parse().unwrap_or(0).min(pi-1);}
            else{n2f[j]=pi;}
        } else {
            let ch:Vec<usize>=node.children().iter().map(|&c|n2f[usize::from(c)]).collect();
            let key=format!("{} {:?}",node.op.to_string(),ch);
            if let Some(&ex)=dedup.get(&key) { n2f[j]=ex; }
            else {
                let fidx=next;next+=1;n2f[j]=fidx;dedup.insert(key,fidx);
                match node.op.to_string().as_str() {
                    "not" if ch.len()>=1=>lines.push(format!("N {}",ch[0])),
                    "xor3" if ch.len()>=3=>lines.push(format!("X {} {} {}",ch[0],ch[1],ch[2])),
                    "maj3" if ch.len()>=3=>lines.push(format!("M {} {} {}",ch[0],ch[1],ch[2])),
                    _=>{}
                }
            }
        }
    }
    lines
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
    let po_idxs:Vec<usize>=input.lines()
        .filter(|l|{let t=l.trim();t.starts_with("P ")&&!t.starts_with("PI_COUNT")&&!t.starts_with("PI_MAP")&&!t.starts_with("PI ")})
        .flat_map(|l|l.split_whitespace().nth(1).map(|s|s.parse().unwrap_or(0))).collect();

    let (expr,ids)=match parse(&body){Some(x)=>x,None=>{eprintln!("Parse err");std::process::exit(1);}};
    let runner=Runner::default().with_expr(&expr).run(&rules());
    let extractor=Extractor::new(&runner.egraph, OpCount);
    println!("=== XMG E-graph ===\nPI={} PO={} e-class={} e-node={}",pi,po_idxs.len(),
             runner.egraph.number_of_classes(),runner.egraph.total_number_of_nodes());

    // Per-PO extraction: each PO gets its own gate tree
    let mut all_lines:Vec<String>=Vec::new();
    let mut po_new:Vec<usize>=Vec::new();
    let base=pi+2; // offset for PIs+C0/C1
    for &p in &po_idxs {
        if p>=ids.len() { po_new.push(base); continue; }
        if p>=ids.len(){po_new.push(base);continue;}
        let ec=runner.egraph.find(ids[p]);
        eprintln!("PO p={} ec_ok", p);
        let (_,best)=extractor.find_best(ec);
        let rec:RecExpr<SymbolLang>=match best.to_string().parse(){Ok(r)=>r,Err(_)=>{po_new.push(base);continue;}};
        if rec.as_ref().is_empty() { po_new.push(base); continue; }
        let lines=tree_to_lines(&rec,pi);
        let root=base+all_lines.len()+lines.len()-1;
        po_new.push(root);
        all_lines.extend(lines);
    }

    let gate_count=all_lines.len();
    println!("   gates: {} (orig body: {})",gate_count,body.lines().filter(|l|{let t=l.trim();t.starts_with("X ")||t.starts_with("M ")||t.starts_with("N ")}).count()-pi-2);

    let mut out=String::new();
    out.push_str(&format!("PI_COUNT {}\nGATE_COUNT {}\n",pi,gate_count));
    for l in input.lines(){if l.trim().starts_with("PI_MAP"){out.push_str(l);out.push('\n');}}
    for i in 0..pi{out.push_str(&format!("PI {}\n",i));}
    out.push_str("C0\nC1\n");
    for l in &all_lines{out.push_str(l);out.push('\n');}
    for &p in &po_new{out.push_str(&format!("P {}\n",p));}
    if args.len()>=3{fs::write(&args[2],&out)?;}
    Ok(())
}
