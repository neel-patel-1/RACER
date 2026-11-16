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
| `scripts/` | `build.sh`, `count-probes.sh`, `run-bench.sh`, `summarize.py` |

## Usage

Needs clang-17, LLVM 17 (`llvm-17-dev`), CMake 3.20+, Ninja and nasm 2.16.02+.

```sh
probe/scripts/build.sh 200   # probe about every 200 IR instructions -> build/interval-200
probe/scripts/build.sh 0     # probe every loop iteration             -> build/interval-0
probe/scripts/build.sh off   # same libraries, no probes (baseline)   -> build/off
probe/scripts/count-probes.sh probe/build/interval-200
probe/scripts/run-bench.sh results/today --cpu 2 --accelerator-cpu 4
probe/scripts/summarize.py results/today
```

One build directory holds one configuration. The third-party builds do not track the plugin: after a change
to the pass, build into a new directory.

`count-probes.sh` counts the call sites of `do_yield` in each library.
`run-bench.sh` runs `preempt_bench` of each built configuration in the three
modes and writes `overhead.csv` (`OVERHEAD_SWEEPS` sweeps over the builds,
default 10), `latency.csv`, `samples-<build>.csv` (each latency run) and
`signal-floor.csv`. The arguments after the output directory go to each run.
`summarize.py` joins these CSVs into the tables of [Results](#results).

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

After the backend, the hot path of a probe point is three instructions, and
the yield block is out of line:

```
movq  %fs:(%rbp), %rax      # preempt_signal of this thread
cmpb  $0x0, (%rax)          # status byte of the completion record
jne   <yield block>         # callq do_yield; jmp <loop body>
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
preempt_bench --mode overhead|latency|signal-floor [options]
  --filler NAME        a filler, or all (default); repeat for more than one
  --size BYTES         input size of each filler, a multiple of 64 (default 65536)
  --runs N             measured runs (default 2000 overhead, 1000 latency,
                       10000 signal-floor)
  --cpu N              CPU of the filler (default 2)
  --accelerator-cpu N  CPU of the emulated accelerator (default 4)
  --offload-ns N       offload time; default uniform in [0, 0.9 x filler runtime]
                       in latency mode, 1000 in signal-floor mode
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
build must equal the checksum of the `off` build. Then it runs the filler one
more time with `preempt_signal` on a byte that is always set and with
`do_yield` in count mode (it counts and returns at once), and reports the
probe points that one run executes. The lab hosts do not give hardware
counters to users (`perf_event_paranoid` is 4), so the count comes from
`do_yield`.

**Latency mode** does this for each run:

1. Sets `preempt_signal` to the status byte of a completion record and asks
   the emulated accelerator to complete at a time in the filler runtime.
2. Switches to the filler fiber.
3. When the filler yields (or ends), records the time.
4. Resumes the filler to its end and checks its output.

The latency is the time from the completion write to the scheduler running
again, in two parts:

- **detect**: from the completion write (the accelerator reads the TSC just
  before it) to the probe point that sees the status byte (`do_yield` reads
  the TSC first), or to the end of the filler.
- **switch**: from that probe point to the scheduler running again after
  the fiber swap.

Each run ends in one of three outcomes: `yielded`,
`finished_after_completion` (the filler did not see the signal) or
`finished_before_completion` (no latency recorded). `late_completions` counts
runs where the accelerator thread wrote the record more than 1 µs after the
requested time; a nonzero count means that the CPUs were not idle.

**Signal-floor mode** measures the least detect time: the filler CPU loads
the status byte in a tight loop, with no filler and no fiber. The floor is
the time to move the cache line of the record from the accelerator core to
the filler core, plus the offset between the TSCs of the two cores. Detect
minus the floor is the wait for the next probe point.

Each run takes the next record of a pool of 1024 records (one cache line for
the status byte, one for the time stamp). With one record for all runs, the
floor and detect of one process were 160 ns to 211 ns on emerald-02,
different in each process: the L3 slice that holds the line depends on its
physical address.

CSV columns:

- overhead: `mode,probe_interval,ipp_code_path,filler,size_bytes,runs,wrong_outputs,median_ns,p10_ns,p90_ns,probe_executions_per_run,checksum`
- latency: `mode,probe_interval,ipp_code_path,filler,size_bytes,runs,yielded,finished_after_completion,finished_before_completion,late_completions,wrong_outputs,latency_median_ns,latency_p90_ns,latency_p99_ns,latency_max_ns,detect_median_ns,detect_p90_ns,detect_p99_ns,switch_median_ns,switch_p90_ns,switch_p99_ns,filler_median_ns`
- `--samples`: `probe_interval,ipp_code_path,filler,size_bytes,run,offload_ns,accelerator_lateness_ns,outcome,detect_ns,switch_ns,latency_ns,output_correct`
- signal-floor: `mode,cpu,accelerator_cpu,runs,late_completions,median_ns,p10_ns,p90_ns,p99_ns,max_ns`

## Results

Measured on emerald-02 (Xeon Gold 6554S, same CPU flags as the build host
emerald-01), host idle, 64 KiB inputs, filler on CPU 2. All outputs and
checksums match the `off` build. `late_completions` is at most 8 of 1000 in
each latency row.

### Overhead

`run-bench.sh` defaults: 10 sweeps of 2000 runs, each sweep one process for
each build. Overhead is the median of the sweep medians against `off`. Time a
probe is runtime / probe points a run (the mean time between two probe
points). Cost a probe is (runtime − `off` runtime) / probe points a run.

| Filler | Runtime (off) | Probes a run 0 / 200 | Time a probe 0 / 200 | Overhead 0 / 200 | Cost a probe 0 / 200 |
|---|---|---|---|---|---|
| `deserialize` | 4.05 µs | 12 / 6 | 338 / 678 ns | +0.1 / +0.4 % | 0.2 / 2.5 ns |
| `decrypt` | 7.25 µs | 5 / 1 | 1.45 / 7.27 µs | +0.1 / +0.2 % | — |
| `decrypt-chunked` | 7.82 µs | 21 / 17 | 373 / 461 ns | +0.2 / +0.2 % | 0.8 / 1.0 ns |
| `matmul` | 1.29 ms | 202,652 / 58,769 | 6.6 / 22 ns | +3.7 / −0.6 % | 0.2 / −0.1 ns |
| `histogram` | 37.3 µs | 21,827 / 3,119 | 2.1 / 11 ns | +21.2 / −11.3 % | 0.4 / — |
| `dotproduct` | 29.8 µs | 2,048 / 410 | 15 / 73 ns | +0.0 / +0.0 % | 0.0 / 0.0 ns |
| `gather` | 9.57 µs | 2,048 / 1,024 | 5.0 / 10 ns | +6.3 / +7.5 % | 0.3 / 0.7 ns |
| `scatter-update` | 11.7 µs | 2,048 / 1,024 | 5.9 / 12 ns | +2.9 / +1.9 % | 0.2 / 0.2 ns |
| `hash` | 15.0 µs | 1,035 / 349 | 15 / 43 ns | +0.1 / +0.1 % | 0.0 / 0.0 ns |
| `sequential-writer` | 972 ns | 1,024 / 32 | 1.1 / 30 ns | +13.6 / +0.3 % | 0.1 / 0.1 ns |
| `decrypt`, portable-c | 2.90 ms | 549,002 / 155,686 | 5.5 / 19 ns | +4.2 / −0.6 % | 0.2 / −0.1 ns |
| `decrypt-chunked`, portable-c | 2.91 ms | 549,018 / 155,702 | 5.5 / 19 ns | +4.2 / −0.6 % | 0.2 / −0.1 ns |

- Where the probe count is large enough to measure the cost, a probe point
  costs 0.1 ns to 0.7 ns: its three instructions run in parallel with the
  loop work. The overhead is about
  cost a probe / time a probe, so it is large only where the probe points are
  dense: `sequential-writer` and `histogram` at interval 0 (1-2 ns between
  probe points). At interval 200 there are at least 10 ns between probe
  points and the overhead is at most 2 %, except `gather`.
- `gather` is slower at 200 than at 0: the unrolled vector loop is about 2.4
  times the size of the `off` loop. The cause is not studied yet.
- `histogram` in the `off` build changes between processes (median 32.6 µs to
  40.4 µs in 30 processes of 2000 runs), so its overhead at 200 is noise. At
  0 the slowest of 30 processes of `off` (40.4 µs) is faster than the fastest
  of interval 0 (44.5 µs).
- `decrypt-chunked` is bimodal in one process (p10 6.5 µs, median 7.8 µs).
  With 200 runs, the median of some processes is in the fast mode; the old
  default of 200 runs gave a false +16 %.
- `decrypt` (native) executes no probe point in its assembly, so ±0.2 % is
  noise.

### Latency

Accelerator thread on CPU 4 (same socket, other core), 1000 runs, offload
time uniform in [0, 0.9 x filler runtime], interval 200. Detect model is
signal floor (188 ns) + time a probe / 2: the wait for the next probe point if
probe points are evenly spaced in time.

| Filler | Yields | Detect median / p99 | Detect model | Switch median / p99 | Latency median / p99 | Latency off median |
|---|---|---|---|---|---|---|
| `deserialize` | 1000 | 2.16 / 4.01 µs | 527 ns | 48 / 56 ns | 2.21 / 4.06 µs | 2.16 µs |
| `decrypt` | 0 | 3.49 / 7.30 µs | — | 47 / 55 ns | 3.54 / 7.35 µs | 3.46 µs |
| `decrypt-chunked` | 935 | 375 ns / 1.54 µs | 418 ns | 46 / 59 ns | 421 ns / 1.58 µs | 3.69 µs |
| `matmul` | 1000 | 212 / 302 ns | 199 ns | 52 / 75 ns | 265 / 357 ns | 697 µs |
| `histogram` | 1000 | 202 / 264 ns | 193 ns | 47 / 60 ns | 246 / 307 ns | 18.8 µs |
| `dotproduct` | 1000 | 211 / 264 ns | 224 ns | 40 / 51 ns | 252 / 307 ns | 16.2 µs |
| `gather` | 999 | 203 / 245 ns | 193 ns | 51 / 62 ns | 254 / 299 ns | 5.23 µs |
| `scatter-update` | 1000 | 203 / 248 ns | 194 ns | 50 / 61 ns | 252 / 303 ns | 6.42 µs |
| `hash` | 1000 | 203 / 252 ns | 210 ns | 40 / 52 ns | 244 / 296 ns | 8.16 µs |
| `sequential-writer` | 999 | 204 / 247 ns | 203 ns | 43 / 56 ns | 246 / 294 ns | 577 ns |
| `decrypt`, portable-c | 1000 | 214 / 315 ns | 197 ns | 59 / 72 ns | 269 / 381 ns | 1.57 ms |
| `decrypt-chunked`, portable-c | 1000 | 217 / 323 ns | 197 ns | 62 / 73 ns | 276 / 385 ns | 1.58 ms |

- Where the probe points are dense, the signal floor is about 75 % of the
  latency, the switch about 20 %, and the wait for the next probe point the
  rest. The model is within 20 ns of the detect median where probe points
  are dense. At interval 0 the latency is the same within 15 ns: the probe
  period is already short at 200.
- `deserialize` yields on each run, but only after the `bytes` copy, so its
  latency equals the latency of the `off` build. Its probe points are not
  evenly spaced in time, so the model does not apply.
- `decrypt` (native) does not yield: the VAES assembly has no probe points.
- With `aesni`, one `decrypt` call takes 35 µs and does not yield;
  `decrypt-chunked` yields with a 1.4 µs median (measured on emerald-01).

### Accelerator placement

The 7 RACER kernels at interval 200, with the accelerator thread on three
CPUs. The overhead does not depend on the accelerator CPU (no offload).

| Accelerator CPU | Signal floor median (p10 / p90) | Detect median | Switch median | Latency median | Latency p99 |
|---|---|---|---|---|---|
| 74, SMT sibling of CPU 2 | 62 ns (46 / 66) | 61-105 ns | 38-54 ns | 109-146 ns | 137-189 ns |
| 4, same socket | 188 ns (159 / 217) | 202-212 ns | 40-52 ns | 244-265 ns | 294-357 ns |
| 40, other socket | 355 ns (317 / 380) | 365-372 ns | 40-56 ns | 405-428 ns | 455-863 ns |

The floor sets the latency: the placement changes it by 290 ns, the switch
does not change.

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
- **The accelerator is emulated.** A core writes the completion record, so
  the signal floor is a core-to-core cache-line transfer. A device writes the
  record from the I/O fabric, so its floor is different and is not measured
  here. With an IAA or DSA device, an administrator
  must first configure the work queues (`accel-config`, or
  `scripts/accel_conf.py`); the RACER applications in `src/` are not yet linked
  to these probed builds.
- **Use idle CPUs for the bench.** On a loaded host, `late_completions` rises
  and the latency includes the delay of the accelerator thread.
