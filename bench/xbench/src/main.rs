//! Load generator and Tantivy driver for the non-Postgres engines.
//!
//!   xbench http BASE QUERIES [opts]      queries: `qid \t path \t json`, POSTed to BASE+path
//!   xbench tantivy-index DIR DOCS        docs: NDJSON {id, body, created, category_id}
//!   xbench tantivy DIR QUERIES [opts]    queries: `qid \t kind \t mode \t cutoff \t terms...`
//!
//! Options: --clients N --warmup S|N --duration S | --repeat N (requests per client) --log FILE --took PTR (engine-reported ms)
//!          --header 'K: V' --writes FILE --write-rate R (lines `METHOD \t path \t json`, or
//!          `op \t json doc` for tantivy) --ids-out FILE --ids-ptr PTR --id-ptr PTR --count-ptr PTR
//!          --writers N (default 4) --threads N --mem-mb N (tantivy-index)
use std::collections::HashMap;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::ops::Bound;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use std::{fs, thread};

use serde_json::Value;
use tantivy::collector::{Count, TopDocs};
use tantivy::query::{BooleanQuery, FuzzyTermQuery, Occur, PhraseQuery, Query, RangeQuery, RegexQuery, TermQuery};
use tantivy::schema::{Field, IndexRecordOption, Schema, FAST, INDEXED, STORED, TEXT};
use tantivy::{Index, IndexReader, IndexWriter, ReloadPolicy, Searcher, TantivyDocument, Term};

type Res<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;

struct Opts(HashMap<String, String>);

impl Opts {
    fn get(&self, k: &str) -> Option<&str> { self.0.get(k).map(String::as_str) }
    fn num(&self, k: &str, d: f64) -> f64 { self.get(k).map_or(d, |v| v.parse().expect(k)) }
}

fn parse_args() -> (Vec<String>, Opts) {
    let (mut pos, mut opts, mut it) = (vec![], HashMap::new(), std::env::args().skip(1));
    while let Some(a) = it.next() {
        match a.strip_prefix("--") {
            Some(k) => { opts.insert(k.to_string(), it.next().expect("option value")); }
            None => pos.push(a),
        }
    }
    (pos, Opts(opts))
}

fn lines(path: &str) -> Vec<String> {
    BufReader::new(fs::File::open(path).expect(path)).lines().map(Result::unwrap).filter(|l| !l.is_empty()).collect()
}

struct Rng(u64);
impl Rng {
    fn next(&mut self, n: usize) -> usize {
        self.0 ^= self.0 << 13; self.0 ^= self.0 >> 7; self.0 ^= self.0 << 17;
        (self.0 % n as u64) as usize
    }
}

/// One timed sample: client latency and engine-reported time (both µs, took 0 if unknown).
type Sample = (u64, u64);

/// Closed-loop clients picking random queries (fixed per-client seeds). With `repeat`, each client
/// runs `warm` then `len` requests; otherwise `warm` then `len` seconds. Returns samples, errors and
/// the measured span in seconds.
fn drive<F>(clients: usize, n: usize, warm: f64, len: f64, repeat: bool, f: F) -> (Vec<Sample>, usize, f64)
where F: Fn(usize, usize) -> Res<u64> + Sync {
    let samples = Mutex::new(vec![]);
    let errors = Mutex::new((0usize, String::new()));
    let span = Mutex::new((None::<Instant>, None::<Instant>));
    let start = Instant::now();
    let measuring = |i: usize, t: Instant| if repeat { i as f64 >= warm } else { (t - start).as_secs_f64() >= warm };
    let done = |i: usize, t: Instant| if repeat { i as f64 >= warm + len } else { (t - start).as_secs_f64() >= warm + len };
    thread::scope(|s| {
        for c in 0..clients {
            let (f, samples, errors, span) = (&f, &samples, &errors, &span);
            s.spawn(move || {
                let (mut rng, mut mine, mut first) = (Rng(0x9E3779B97F4A7C15 ^ (c as u64 + 1) * 0x1000193), vec![], None);
                for i in 0.. {
                    let t0 = Instant::now();
                    if done(i, t0) { break; }
                    let r = f(c, rng.next(n));
                    if !measuring(i, t0) { continue; }
                    first.get_or_insert(t0);
                    match r {
                        Ok(took) => mine.push((t0.elapsed().as_micros() as u64, took)),
                        Err(e) => { let mut g = errors.lock().unwrap(); g.0 += 1; g.1 = e.to_string(); }
                    }
                }
                let mut g = span.lock().unwrap();
                if let Some(t) = first { g.0 = Some(g.0.map_or(t, |a: Instant| a.min(t))); }
                g.1 = Some(Instant::now());
                samples.lock().unwrap().extend(mine);
            });
        }
    });
    let (n_err, last) = errors.into_inner().unwrap();
    if n_err > 0 { eprintln!("errors={n_err} last={last}"); }
    let secs = match span.into_inner().unwrap() { (Some(a), Some(b)) => (b - a).as_secs_f64(), _ => len };
    (samples.into_inner().unwrap(), n_err, secs)
}

fn pct(v: &[u64], p: f64) -> f64 {
    if v.is_empty() { return 0.0; }
    v[((v.len() as f64 * p).ceil() as usize).clamp(1, v.len()) - 1] as f64 / 1000.0
}

fn report(name: &str, samples: &[Sample], errors: usize, secs: f64, log: Option<&str>) {
    let mut lat: Vec<u64> = samples.iter().map(|s| s.0).collect();
    let mut took: Vec<u64> = samples.iter().map(|s| s.1).collect();
    if let Some(path) = log {
        let mut w = BufWriter::new(fs::File::create(path).unwrap());
        samples.iter().for_each(|s| writeln!(w, "{}\t{}", s.0, s.1).unwrap());
    }
    lat.sort_unstable(); took.sort_unstable();
    let avg = |v: &[u64]| v.iter().sum::<u64>() as f64 / v.len().max(1) as f64 / 1000.0;
    println!("{name} qps={:.2} avg={:.2} p50={:.2} p95={:.2} p99={:.2} n={} errors={errors} took_avg={:.2} took_p99={:.2}",
             lat.len() as f64 / secs, avg(&lat), pct(&lat, 0.5), pct(&lat, 0.95), pct(&lat, 0.99), lat.len(), avg(&took), pct(&took, 0.99));
}

/// Issues write lines at a fixed rate (open loop) until `stop`; returns samples.
fn write_loop<F>(rate: f64, n: usize, stop: &AtomicBool, f: F) -> Vec<Sample>
where F: Fn(usize) -> Res<u64> {
    let (start, mut out, mut errors) = (Instant::now(), vec![], 0);
    for k in 0.. {
        if stop.load(Ordering::Relaxed) { break; }
        let due = start + Duration::from_secs_f64(k as f64 / rate);
        if let Some(d) = due.checked_duration_since(Instant::now()) { thread::sleep(d); }
        let t0 = Instant::now();
        match f(k % n) { Ok(_) => out.push((t0.elapsed().as_micros() as u64, 0)), Err(_) => errors += 1 }
    }
    if errors > 0 { eprintln!("write errors={errors}"); }
    out
}

/// `k` open-loop writer threads sharing `rate`; thread w issues lines w, w + k, w + 2k...
fn write_pool<F>(k: usize, rate: f64, n: usize, stop: &AtomicBool, f: F) -> Vec<Sample>
where F: Fn(usize) -> Res<u64> + Sync {
    thread::scope(|s| {
        let hs: Vec<_> = (0..k).map(|w| { let f = &f; s.spawn(move || write_loop(rate / k as f64, n / k, stop, |i| f(i * k + w))) }).collect();
        hs.into_iter().flat_map(|h| h.join().unwrap()).collect()
    })
}

fn ms_ptr(v: &Value, ptr: Option<&str>) -> u64 {
    ptr.and_then(|p| v.pointer(p)).and_then(Value::as_f64).map_or(0, |ms| (ms * 1000.0) as u64)
}

fn http(pos: &[String], o: &Opts) -> Res<()> {
    let base = pos[1].clone();
    let queries: Vec<Vec<String>> = lines(&pos[2]).iter().map(|l| l.splitn(3, '\t').map(String::from).collect()).collect();
    let header = o.get("header").map(|h| h.split_once(": ").map(|(k, v)| (k.to_string(), v.to_string())).expect("header"));
    let agent: ureq::Agent = ureq::Agent::config_builder().max_idle_connections_per_host(256).build().into();
    let call = |method: &str, path: &str, body: &str| -> Res<Value> {
        let url = format!("{base}{path}");
        let mut req = ureq::http::Request::builder().method(method).uri(url).header("Content-Type", "application/json");
        if let Some((k, v)) = &header { req = req.header(k.as_str(), v.as_str()); }
        let mut resp = agent.run(req.body(body.to_string())?)?;
        Ok(serde_json::from_str(&resp.body_mut().read_to_string()?)?)
    };
    if let Some(out) = o.get("ids-out") {
        let mut w = BufWriter::new(fs::File::create(out)?);
        for q in &queries {
            let v = call("POST", &q[1], &q[2])?;
            match o.get("count-ptr") {
                Some(p) => writeln!(w, "{}\t{}", q[0], v.pointer(p).ok_or("count missing")?)?,
                None => {
                    let hits = v.pointer(o.get("ids-ptr").unwrap()).and_then(Value::as_array).ok_or("hits missing")?;
                    let mut ids: Vec<i64> = hits.iter().map(|h| {
                        let id = h.pointer(o.get("id-ptr").unwrap()).expect("id");
                        id.as_i64().unwrap_or_else(|| id.as_str().unwrap().parse().unwrap())
                    }).collect();
                    ids.sort_unstable();
                    writeln!(w, "{}\t{{{}}}", q[0], ids.iter().map(i64::to_string).collect::<Vec<_>>().join(","))?;
                }
            }
        }
        return Ok(());
    }
    let stop = AtomicBool::new(false);
    let writes = o.get("writes").map(lines).unwrap_or_default();
    thread::scope(|s| -> Res<()> {
        let writer = (!writes.is_empty()).then(|| s.spawn(|| write_pool(o.num("writers", 4.0) as usize, o.num("write-rate", 1000.0), writes.len(), &stop, |i| {
            let w: Vec<&str> = writes[i].splitn(3, '\t').collect();
            call(w[0], w[1], w.get(2).copied().unwrap_or("")).map(|_| 0)
        })));
        let (samples, errors, secs) = drive(o.num("clients", 8.0) as usize, queries.len(), o.num("warmup", 5.0), o.num("repeat", o.num("duration", 30.0)), o.get("repeat").is_some(),
            |_, i| call("POST", &queries[i][1], &queries[i][2]).map(|v| ms_ptr(&v, o.get("took"))));
        stop.store(true, Ordering::Relaxed);
        report("read", &samples, errors, secs, o.get("log"));
        if let Some(h) = writer { report("write", &h.join().unwrap(), 0, secs + o.num("warmup", 5.0), None); }
        Ok(())
    })
}

struct Tv { body: Field, id: Field, created: Field, category_id: Field }

fn schema() -> (Schema, Tv) {
    let mut b = Schema::builder();
    let id = b.add_u64_field("id", INDEXED | STORED | FAST);
    let body = b.add_text_field("body", TEXT);
    let created = b.add_i64_field("created", INDEXED | FAST);
    let category_id = b.add_i64_field("category_id", INDEXED | FAST);
    (b.build(), Tv { body, id, created, category_id })
}

fn tv_doc(f: &Tv, v: &Value) -> TantivyDocument {
    let mut d = TantivyDocument::new();
    d.add_u64(f.id, v["id"].as_str().unwrap().parse().unwrap());
    d.add_text(f.body, v["body"].as_str().unwrap());
    if let Some(c) = v["created"].as_i64() { d.add_i64(f.created, c); }
    if let Some(p) = v["category_id"].as_i64() { d.add_i64(f.category_id, p); }
    d
}

fn tantivy_index(pos: &[String], o: &Opts) -> Res<()> {
    let (schema, f) = schema();
    fs::create_dir_all(&pos[1])?;
    let index = Index::create_in_dir(&pos[1], schema)?;
    let t0 = Instant::now();
    let mut w: IndexWriter = index.writer_with_num_threads(o.num("threads", 4.0) as usize, (o.num("mem-mb", 4000.0) as usize) << 20)?;
    for l in BufReader::new(fs::File::open(&pos[2])?).lines() {
        w.add_document(tv_doc(&f, &serde_json::from_str(&l?)?))?;
    }
    w.commit()?;
    let ids = index.searchable_segment_ids()?;
    if ids.len() > 1 { w.merge(&ids).wait()?; }
    w.wait_merging_threads()?;
    println!("build_secs={:.1}", t0.elapsed().as_secs_f64());
    Ok(())
}

fn tv_query(f: &Tv, q: &[String]) -> Box<dyn Query> {
    let terms: Vec<Term> = q[4].split(' ').map(|t| Term::from_field_text(f.body, t)).collect();
    let tq = |t: &Term| -> Box<dyn Query> { Box::new(TermQuery::new(t.clone(), IndexRecordOption::WithFreqs)) };
    let all = |occ: Occur, ts: &[Term]| -> Box<dyn Query> { Box::new(BooleanQuery::new(ts.iter().map(|t| (occ, tq(t))).collect())) };
    let text: Box<dyn Query> = match q[2].as_str() {
        "and" => all(Occur::Must, &terms),
        "or" => all(Occur::Should, &terms),
        "phrase" if terms.len() > 1 => Box::new(PhraseQuery::new(terms)),
        "phrase" => tq(&terms[0]),
        "prefix" => Box::new(RegexQuery::from_pattern(&format!("{}.*", q[4]), f.body).expect("prefix")),
        "fuzzy" => Box::new(FuzzyTermQuery::new(terms[0].clone(), 1, true)),
        "not" => {
            let (last, head) = terms.split_last().unwrap();
            Box::new(BooleanQuery::new(head.iter().map(|t| (Occur::Must, tq(t))).chain([(Occur::MustNot, tq(last))]).collect()))
        }
        m => panic!("mode {m}"),
    };
    match q[1].as_str() {
        "filter" => Box::new(BooleanQuery::new(vec![(Occur::Must, text), (Occur::Must, Box::new(RangeQuery::new(
            Bound::Included(Term::from_field_i64(f.created, q[3].parse().unwrap())), Bound::Unbounded)))])),
        _ => text,
    }
}

fn tv_run(s: &Searcher, f: &Tv, q: &[String]) -> Res<Vec<u64>> {
    let query = tv_query(f, q);
    if q[1] == "count" { return Ok(vec![s.search(&query, &Count)? as u64]); }
    s.search(&query, &TopDocs::with_limit(10).order_by_score())?.into_iter().map(|(_, a)| {
        Ok(s.segment_reader(a.segment_ord).fast_fields().u64("id")?.first(a.doc_id).ok_or("no id")?)
    }).collect()
}

fn tantivy(pos: &[String], o: &Opts) -> Res<()> {
    let index = Index::open_in_dir(&pos[1])?;
    let (_, f) = schema();
    let reader: IndexReader = index.reader_builder().reload_policy(ReloadPolicy::OnCommitWithDelay).try_into()?;
    let queries: Vec<Vec<String>> = lines(&pos[2]).iter().map(|l| l.splitn(5, '\t').map(String::from).collect()).collect();
    if let Some(out) = o.get("ids-out") {
        let mut w = BufWriter::new(fs::File::create(out)?);
        let s = reader.searcher();
        for q in &queries {
            let mut ids = tv_run(&s, &f, q)?;
            match q[1].as_str() {
                "count" => writeln!(w, "{}\t{}", q[0], ids[0])?,
                _ => { ids.sort_unstable(); writeln!(w, "{}\t{{{}}}", q[0], ids.iter().map(u64::to_string).collect::<Vec<_>>().join(","))? }
            }
        }
        return Ok(());
    }
    let stop = AtomicBool::new(false);
    let writes = o.get("writes").map(lines).unwrap_or_default();
    let (_, docs_f) = schema();
    thread::scope(|sc| -> Res<()> {
        let writer = (!writes.is_empty()).then(|| sc.spawn(|| {
            let mut iw: IndexWriter = index.writer_with_num_threads(1, 200 << 20).unwrap();
            let last = Mutex::new(Instant::now());
            let w = Mutex::new(&mut iw);
            // Commit once per second, like a 1s refresh interval.
            write_pool(o.num("writers", 4.0) as usize, o.num("write-rate", 1000.0), writes.len(), &stop, |i| {
                let (op, doc) = writes[i].split_once('\t').ok_or("write line")?;
                let doc: Value = serde_json::from_str(doc)?;
                let mut iw = w.lock().unwrap();
                if op != "ins" { iw.delete_term(Term::from_field_u64(docs_f.id, doc["id"].as_str().unwrap().parse()?)); }
                if op != "del" { iw.add_document(tv_doc(&docs_f, &doc))?; }
                let mut l = last.lock().unwrap();
                if l.elapsed() >= Duration::from_secs(1) { iw.commit()?; *l = Instant::now(); }
                Ok(0)
            })
        }));
        let (samples, errors, secs) = drive(o.num("clients", 8.0) as usize, queries.len(), o.num("warmup", 5.0), o.num("repeat", o.num("duration", 30.0)), o.get("repeat").is_some(),
            |_, i| tv_run(&reader.searcher(), &f, &queries[i]).map(|_| 0));
        stop.store(true, Ordering::Relaxed);
        let written = writer.map(|h| h.join().unwrap());
        // Bytes written by this process (index segments, deletes); index reads go through mmap.
        let io = fs::read_to_string("/proc/self/io")?;
        println!("io wchar={}", io.lines().find_map(|l| l.strip_prefix("wchar: ")).unwrap_or("0"));
        report("read", &samples, errors, secs, o.get("log"));
        if let Some(w) = written { report("write", &w, 0, secs + o.num("warmup", 5.0), None); }
        Ok(())
    })
}

fn main() -> Res<()> {
    let (pos, o) = parse_args();
    match pos.first().map(String::as_str) {
        Some("http") => http(&pos, &o),
        Some("tantivy-index") => tantivy_index(&pos, &o),
        Some("tantivy") => tantivy(&pos, &o),
        _ => Err("usage: xbench http|tantivy-index|tantivy ...".into()),
    }
}
