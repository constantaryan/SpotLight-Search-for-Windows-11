import pathlib, subprocess, json, statistics
root=pathlib.Path(__file__).resolve().parents[1]
binaries={"c":root/"build/release/bench_c.exe","cpp":root/"build/release/bench_cpp.exe","rust":root/"benchmarks/rust/target/release/bench_rust.exe"}
runs={key:[] for key in binaries}
for trial in range(7):
    order=list(binaries)
    if trial%2:order.reverse()
    for key in order:
        exe=binaries[key]
        subprocess.run([str(exe)],check=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
        result=json.loads(pathlib.Path(str(exe)+".json").read_text())
        result["exe_bytes"]=exe.stat().st_size
        runs[key].append(result)
checksum={r["checksum"] for values in runs.values() for r in values}
assert len(checksum)==1,checksum
summary={key:{metric:statistics.median(r[metric] for r in values) for metric in values[0]} for key,values in runs.items()}
output={"scope":"Native controls + fixed 5000-entry catalogue + ranking. No search-provider, allocation-count or long-idle measurements.","runs":runs,"medians":summary}
(root/"build/language-results.json").write_text(json.dumps(output,indent=2))
print(json.dumps(summary,indent=2))
