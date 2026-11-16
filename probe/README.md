# Probe-point accelerator preemption with LLVM 17

This directory builds the libraries that RACER protects with compiler-inserted
probe points, through an LLVM 17 pass plugin:

- protobuf (v28.3, the `third-party/protobuf` submodule) and the generated
  parser of `src/protobuf/router.proto`
- ipp-crypto (`third-party/ipp-crypto`), all C code of the merged build
- the RACER kernels (`src/kernels/*.cpp`)

`preempt_bench` runs each library as a filler next to an emulated accelerator
and measures the probe overhead and the preemption latency. There is no
IAA or DSA device on the lab hosts, so the accelerator is a pinned thread that
writes a completion record at a chosen time.

`pass/` replaces `src/XMPPass` (a legacy pass manager pass) for use as a clang
plugin with the new pass manager. It keeps the symbol names and the
`xmp_skip` annotation.

## Layout

| Path | What it holds |
|---|---|
| `pass/RacerProbePass.cpp` | the pass plugin |
| `runtime/` | `racer_probe.h` and weak default definitions of the probe symbols |
| `cmake/racer-probe-compiler-launcher.in` | the compiler launcher of the probed third-party builds |
| `CMakeLists.txt` | the superbuild: abseil, protobuf, ipp-crypto, workloads |
| `workloads/` | `racer_kernels`, `racer_router_proto`, `aes_gcm_chunked`, `preempt_bench` |
| `test/pass/*.ll` | `opt` tests of the pass, run by `ctest` |
| `scripts/` | `build.sh`, `count-probes.sh`, `run-bench.sh` |

## Usage

Needs clang-17, LLVM 17 (`llvm-17-dev`), CMake 3.20+, Ninja and nasm 2.16.02+.

```sh
probe/scripts/build.sh 200   # probe about every 200 IR instructions -> build/interval-200
probe/scripts/build.sh 0     # probe every loop iteration             -> build/interval-0
probe/scripts/build.sh off   # same libraries, no probes (baseline)   -> build/off
probe/scripts/count-probes.sh probe/build/interval-200
probe/scripts/run-bench.sh results/today --cpu 2 --accelerator-cpu 4
```

One build directory holds one configuration. The third-party builds do not track the plugin: after a change
to the pass, build into a new directory.

`count-probes.sh` counts the call sites of `do_yield` in each library.
`run-bench.sh` runs `preempt_bench` of each built configuration in both modes
and writes `overhead.csv` and `latency.csv`. The arguments after the output
directory go to each run.

To probe another library, compile it with

```sh
clang-17 -fpass-plugin=RacerProbePass.so -Xclang -load -Xclang RacerProbePass.so \
         -mllvm -racer-probe-interval=200 ...
```

and give the program `preempt_signal` and `do_yield`, or link
`libracer_probe_runtime.a` for the weak defaults. The pass is also available
in `opt` as `-load-pass-plugin=RacerProbePass.so -passes=racer-probe`.

## The pass

A probe point is, at a loop header:

```llvm
%signal_address = call ptr @llvm.threadlocal.address.p0(ptr @preempt_signal)
%signal = load ptr, ptr %signal_address
%status = load volatile i8, ptr %signal
%completed = icmp ne i8 %status, 0
br i1 %completed, label %racer.yield, label %loop.body, !prof !{1, 10000}
racer.yield:
call void @do_yield()
br label %loop.body
```

`preempt_signal` (`__thread volatile uint8_t *`) points at the status byte of
the completion record of the offload in flight. `do_yield()` switches from the
filler back to the scheduler and returns when the scheduler resumes the filler.
These are the names that the RACER applications define (`src/*.cpp`).

The pass runs at `OptimizerLast` (after the full `-O2`/`-O3` pipeline), in two
phases:

1. **Unroll.** An innermost loop whose body is smaller than
   `-racer-probe-interval` IR instructions is partially unrolled by
   interval / body size, so one probe covers about that many instructions.
   Each copy keeps its exit test, so there is no remainder loop. A loop with a
   call, inline assembly, a convergent operation or a memory intrinsic of
   non-constant length is not unrolled. With interval 0 this phase is off.
2. **Probe.** Each loop header gets one probe point. Functions annotated
   `__attribute__((annotate("xmp_skip")))`, naked functions and `do_yield`
   get none.

Third-party CMake builds get the flags through a compiler launcher
(`CMAKE_<LANG>_COMPILER_LAUNCHER`), so only compile steps see them. Programs
inside those builds (protoc) link `libracer_probe_runtime.a`, which gives weak
definitions: the signal points at a byte that stays zero and `do_yield` does
nothing.

## preempt_bench

```
preempt_bench --mode overhead|latency [options]
  --filler NAME        a filler, or all (default); repeat for more than one
  --size BYTES         input size of each filler, a multiple of 64 (default 65536)
  --runs N             measured runs (default 200 overhead, 1000 latency)
  --cpu N              CPU of the filler (default 2)
  --accelerator-cpu N  CPU of the emulated accelerator (default 4)
  --offload-ns N       offload time; default uniform in [0, 0.9 x filler runtime]
  --seed N             seed of the inputs and the offload times of each filler
  --samples PATH       latency mode: also write each run to PATH
  --ipp-code-path P    GCM code of ipp-crypto: native, aesni or portable-c
  --no-header          do not print the CSV header
```

| Filler | Library | Work |
|---|---|---|
| `deserialize` | protobuf | parse a `RouterRequest` |
| `decrypt` | ipp-crypto | one `ippsAES_GCMDecrypt` call |
| `decrypt-chunked` | ipp-crypto + `aes_gcm_chunked` | one call for each 4 KiB |
| `matmul`, `histogram`, `dotproduct`, `gather`, `scatter-update`, `hash`, `sequential-writer` | RACER kernels | the kernel of the same name |

**Overhead mode** runs each filler with no offload and reports the median,
p10 and p90 runtime and a checksum of the output. The checksum of a probed
build must equal the checksum of the `off` build.

**Latency mode** does this for each run:

1. Sets `preempt_signal` to the status byte of a completion record and asks
   the emulated accelerator to complete at a time in the filler runtime.
2. Switches to the filler fiber.
3. When the filler yields (or ends), records the time.
4. Resumes the filler to its end and checks its output.

The latency is the time from the completion write to the return to the
scheduler. Each run ends in one of three outcomes: `yielded`,
`finished_after_completion` (the filler did not see the signal) or
`finished_before_completion` (no latency recorded). `late_completions` counts
runs where the accelerator thread wrote the record more than 1 µs after the
requested time; a nonzero count means that the CPUs were not idle.

CSV columns:

- overhead: `mode,probe_interval,ipp_code_path,filler,size_bytes,runs,wrong_outputs,median_ns,p10_ns,p90_ns,checksum`
- latency: `mode,probe_interval,ipp_code_path,filler,size_bytes,runs,yielded,finished_after_completion,finished_before_completion,late_completions,wrong_outputs,latency_median_ns,latency_p90_ns,latency_p99_ns,latency_max_ns,filler_median_ns`
- `--samples`: `probe_interval,ipp_code_path,filler,size_bytes,run,offload_ns,accelerator_lateness_ns,outcome,latency_ns,output_correct`

## Results

Measured on emerald-02 (Xeon Gold 6554S, same CPU flags as the build host
emerald-01), 64 KiB inputs, filler on CPU 2, accelerator thread on CPU 4, the
host idle. Overhead: median of 200 runs against the `off` build. Latency: 1000
runs, offload time uniform in [0, 0.9 x filler runtime], from completion write
to return to the scheduler. All outputs and checksums match the `off` build;
`late_completions` is 0 to 7 of 1000 in each row.

| Filler | Runtime (off) | Overhead 200 | Overhead 0 | Yields 200 | Latency 200 median / p99 | Yields 0 | Latency 0 median / p99 | Latency off median |
|---|---|---|---|---|---|---|---|---|
| `deserialize` | 4.07 µs | -0.1 % | +0.1 % | 1000 | 2.15 µs / 4.00 µs | 1000 | 2.12 µs / 3.98 µs | 2.08 µs |
| `decrypt` | 7.44 µs | -3.3 % | -3.2 % | 0 | 3.46 µs / 7.08 µs | 0 | 3.44 µs / 7.14 µs | 3.43 µs |
| `decrypt-chunked` | 7.82 µs | -2.0 % | -1.2 % | 926 | 377 ns / 1.57 µs | 919 | 341 ns / 1.54 µs | 3.59 µs |
| `matmul` | 1.29 ms | -0.8 % | +3.6 % | 1000 | 205 ns / 223 ns | 1000 | 175 ns / 188 ns | 699 µs |
| `histogram` | 36.6 µs | -10.4 % | +22.8 % | 1000 | 196 ns / 238 ns | 1000 | 168 ns / 178 ns | 19.2 µs |
| `dotproduct` | 29.8 µs | +0.0 % | +0.0 % | 1000 | 209 ns / 245 ns | 1000 | 168 ns / 178 ns | 16.0 µs |
| `gather` | 9.60 µs | +7.3 % | +5.8 % | 1000 | 197 ns / 210 ns | 1000 | 168 ns / 180 ns | 5.19 µs |
| `scatter-update` | 11.7 µs | +2.4 % | +4.2 % | 1000 | 201 ns / 211 ns | 1000 | 170 ns / 183 ns | 6.30 µs |
| `hash` | 15.0 µs | +0.1 % | +0.1 % | 1000 | 203 ns / 229 ns | 1000 | 168 ns / 178 ns | 8.08 µs |
| `sequential-writer` | 971 ns | +0.3 % | +11.8 % | 1000 | 196 ns / 212 ns | 1000 | 166 ns / 185 ns | 511 ns |
| `decrypt`, portable-c | 2.91 ms | -0.6 % | +4.2 % | 1000 | 188 ns / 285 ns | 1000 | 185 ns / 295 ns | 1.57 ms |
| `decrypt-chunked`, portable-c | 2.91 ms | -0.7 % | +4.1 % | 1000 | 189 ns / 293 ns | 1000 | 186 ns / 295 ns | 1.57 ms |

Notes:

- `histogram` in the `off` build is bimodal (p10 32.5 µs, p90 46 µs), so its
  overhead at 200 is noise. Two sweeps of 3000 runs give the same picture for
  the other rows: `gather` +7.5 % at 200 and +6 % at 0, `scatter-update`
  +2.6 % and +4.3 %, `sequential-writer` 0 % and +10.5 %, `decrypt` 0 %. The
  native `decrypt` rows have no probes, so about ±3 % is the noise floor of
  one sweep.
- `gather` is slower at 200 than at 0: the unrolled vector loop is about 2.4
  times the size of the `off` loop. The cause is not studied yet.
- `deserialize` yields on each run, but only after the `bytes` copy, so its
  latency equals the latency of the `off` build.
- With `aesni`, one `decrypt` call takes 35 µs and does not yield;
  `decrypt-chunked` yields with a 1.4 µs median (measured on emerald-01).

Probe points (`count-probes.sh`, call sites of `do_yield`):

| Library | Interval 200 | Interval 0 |
|---|---|---|
| `libracer_kernels.a` | 43 | 49 |
| `libracer_router_proto.a` | 1 | 1 |
| `libprotobuf.a` | 4,151 | 4,178 |
| `libprotoc.a` | 6,700 | 6,704 |
| `libippcp.a` | 14,475 | 17,147 |
| `libabsl_strings.a` | 0 | 0 |

At 200 there are fewer call sites than at 0: the unroll phase caps the count
at the maximum trip count, so a loop with a small constant trip count is fully
unrolled and has no header left to probe.

## Limits

- **Code with no loops in IR gets no probes.** The pass probes loop headers
  only; there are no function-entry probes.
  - The `bytes` field of `RouterRequest` is copied by one `memcpy`, so
    `deserialize` waits for the copy.
  - The protobuf table-driven parser chains field handlers with `musttail`
    calls, which are not loops.
  - ipp-crypto GCM on AVX-512 CPUs (`native`) and with AES-NI (`aesni`) runs in
    nasm assembly. One `decrypt` call does not yield. Use `decrypt-chunked`
    (a probe between chunks) or `--ipp-code-path portable-c`.
- **A yield must not happen while the filler holds a lock** that the scheduler
  or another fiber takes. The pass does not know about locks; mark such
  functions `xmp_skip`.
- **A loop with a call is not unrolled**, so it polls on each iteration. The
  kernels are compiled with `-DPERF` to remove the logging calls in their
  loops.
- **abseil is not probed.** abseil code that protobuf inlines from headers is
  probed inside protobuf.
- **All ipp-crypto C code is probed**, not only the GCM files that RACER-Old
  probed.
- **The accelerator is emulated.** With an IAA or DSA device, an administrator
  must first configure the work queues (`accel-config`, or
  `scripts/accel_conf.py`); the RACER applications in `src/` are not yet linked
  to these probed builds.
- **Use idle CPUs for the bench.** On a loaded host, `late_completions` rises
  and the latency includes the delay of the accelerator thread.
