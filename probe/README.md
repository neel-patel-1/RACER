# Probe-point accelerator preemption with LLVM 17

This directory builds the libraries that RACER protects with compiler-inserted
probe points, through an LLVM 17 pass plugin:

- protobuf (v28.3, the `third-party/protobuf` submodule) and the generated
  parser of `src/protobuf/router.proto`
- ipp-crypto (`third-party/ipp-crypto`), all C code of the merged build
- the RACER kernels (`src/kernels/*.cpp`)

`preempt_bench` runs each library as a filler next to an accelerator and
measures the probe overhead and the preemption latency. The accelerator is
an emulated accelerator (a pinned thread that writes a completion record at a
chosen time) or an Intel QAT device (see [QAT accelerator](#qat-accelerator)).

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
| `scripts/` | `build.sh`, `count-probes.sh`, `run-bench.sh`, `summarize.py`, `run-qat-bench.sh`, `summarize-qat.py`, `run-dsa-bench.sh`, `summarize-dsa.py`, `build-preemption-slide.py` |

## Usage

Needs clang-17, LLVM 17 (`llvm-17-dev`), CMake 3.20+, Ninja and nasm 2.16.02+.

```sh
probe/scripts/build.sh 200   # probe about every 200 IR instructions -> build/interval-200
probe/scripts/build.sh 0     # probe every loop iteration             -> build/interval-0
probe/scripts/build.sh off   # same libraries, no probes (baseline)   -> build/off
probe/scripts/count-probes.sh probe/build/interval-200
probe/scripts/run-bench.sh results/today --cpu 2 --accelerator-cpu 4
probe/scripts/summarize.py results/today
probe/scripts/run-qat-bench.sh results/qat 38 110 --size 1048576   # filler CPU, observer CPU
probe/scripts/summarize-qat.py results/qat
```

One build directory holds one configuration. The third-party builds do not track the plugin: after a change
to the pass, build into a new directory.

`count-probes.sh` counts the call sites of `do_yield` in each library.
`run-bench.sh` runs `preempt_bench` of each built configuration in the three
modes and writes `overhead.csv` (`OVERHEAD_SWEEPS` sweeps over the builds,
default 10), `latency.csv`, `samples-<build>.csv` (each latency run) and
`signal-floor.csv`. The arguments after the output directory go to each run.
`summarize.py` joins these CSVs into the tables of [Results](#results).
`run-qat-bench.sh` and `summarize-qat.py` do the same for the QAT modes, and
`run-dsa-bench.sh` and `summarize-dsa.py` for the DSA modes.
`build-preemption-slide.py` draws one slide (python-pptx) from the medians of
[Interrupt path](#interrupt-path) and [DSA results](#dsa-results): the MSI-X
path of QAT next to the completion record that a DSA device writes itself.

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
preempt_bench --mode overhead|latency|signal-floor|qat-latency|qat-floor|
              qat-interrupt|dsa-latency|dsa-floor [options]
  --filler NAME        a filler, or all (default); repeat for more than one
  --size BYTES         input size of each filler, a multiple of 64 (default 65536)
  --runs N             measured runs (default 2000 overhead, 1000 latency,
                       qat-latency and dsa-latency, 10000 signal-floor,
                       qat-floor, qat-interrupt and dsa-floor)
  --cpu N              CPU of the filler (default 2)
  --accelerator-cpu N  CPU of the emulated accelerator (default 4)
  --observer-cpu N     qat and dsa modes: CPU of the completion observer
                       (default 6)
  --offload-bytes N    qat and dsa modes: bytes of each offload (default 4096);
                       for QAT a multiple of 16, at most 65536
  --work-queue PATH    dsa modes: the user WQ of the DSA device (default
                       /dev/dsa/wq0.1)
  --idle-us N          qat-floor, qat-interrupt and dsa-floor modes: the device
                       is idle for N us before each offload (default 0)
  --offload-ns N       offload time; default uniform in [0, 0.9 x filler runtime]
                       in latency mode, 1000 in signal-floor mode
  --seed N             seed of the inputs and the offload times of each filler
                       (default 1)
  --samples PATH       latency, qat-latency and dsa-latency modes: also write
                       each run to PATH
  --ipp-code-path P    GCM code of ipp-crypto: native (default; VAES assembly on
                       AVX-512 CPUs), aesni (AES-NI assembly), or portable-c
                       (table C code, which has probe points in its block loop)
  --no-header          do not print the CSV header
```

| Filler | Library | Work |
|---|---|---|
| `deserialize` | protobuf | parse a `RouterRequest` |
| `serialize` | protobuf | `ByteSizeLong` and `SerializeToArray` of a `RouterRequest` |
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

### Protobuf by input size

`serialize` and `deserialize` at four sizes of the `bytes` value, with the
filler on CPU 38 and the accelerator thread on CPU 40 (second socket: jobs of
another user ran on cores 7-14 of the first socket). Signal floor 192-193 ns.
10 sweeps of 2000 runs, as in the overhead table.

| Value | Filler | Runtime (off) | Off spread | Probes a run 0 / 200 | Overhead 0 / 200 | Yields 0 / 200 | Latency median 0 / 200 / off |
|---|---|---|---|---|---|---|---|
| 64 B | `serialize` | 72 ns | 5.6 % | 8 / 2 | +5.6 / +8.3 % | 0 / 0 | — |
| 64 B | `deserialize` | 126 ns | 2.4 % | 12 / 6 | +0.8 / +8.7 % | 0 / 1 | 42 / 45 / 47 ns |
| 4 KiB | `serialize` | 142 ns | 0.7 % * | 9 / 3 | +2.8 / +7.7 % | 0 / 0 | 54 / 45 / 45 ns |
| 4 KiB | `deserialize` | 218 ns | 0.9 % | 12 / 6 | +0.9 / +6.4 % | 896 / 970 | 221 / 224 / 138 ns |
| 64 KiB | `serialize` | 2.09 µs | 25.4 % | 10 / 3 | +0.5 / +0.5 % | 0 / 0 | 1.23 / 1.24 / 1.23 µs |
| 64 KiB | `deserialize` | 4.10 µs | 1.5 % | 12 / 6 | +0.4 / +0.5 % | 1000 / 1000 | 2.26 / 2.28 / 2.25 µs |
| 1 MiB | `serialize` | 48.0 µs | 9.1 % | 10 / 3 | +1.1 / −1.1 % | 1 / 0 | 28.1 / 28.4 / 27.8 µs |
| 1 MiB | `deserialize` | 76.5 µs | 6.7 % | 12 / 6 | −0.2 / −0.7 % | 1000 / 1000 | 43.2 / 44.4 / 43.9 µs |

\* 149 % with one slow sweep of `off` (353 ns); the other nine are 141-142 ns.

- The value size does not change the probe count: one call executes 2 to 12
  probe points, and the copy of the value (one glibc `memcpy`) executes none.
- Up to 4 KiB the added time is 1-4 ns a call at interval 0 and 6-14 ns at
  interval 200, the same at both sizes. It is a large share only because a
  small message takes 70-220 ns. At 64 KiB and up the overhead is inside
  the spread of `off`.
- Interval 200 costs more than interval 0 with fewer probe points, as in
  `gather`. The cause is not studied yet.
- `serialize` does not yield: it executes no probe point after the copy of
  the value starts. RACER serializes only in its setup code.
- `deserialize` yields only after the copy, so its latency equals the latency
  of the `off` build. Up to 4 KiB the call is shorter than the signal floor, so
  a yield cannot come sooner than the end of the call: at 4 KiB the yield
  makes the latency 83 ns longer.

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

## QAT accelerator

The qat modes use a real accelerator: an Intel QAT 4xxx device decrypts the
offload. The probe points and the filler builds do not change. Only the
completion byte changes: it is in the io_event that the kernel writes when the
device completes the request.

### QAT, DSA and IAA on the lab hosts

The QAT devices (8086:4940: 8 on emerald-02, 4 on emerald-01 and emerald-03)
are open to users through the kernel crypto API (AF_ALG sockets): the
`qat_4xxx` driver gives `qat_aes_cbc`, `qat_aes_ctr` and `qat_aes_xts`. This
needs no root and no device setup. `/dev/qat_adf_ctl` (the QAT user-space
library) needs root. On sapphire the QAT functions are bound to vfio. So the
QAT runs are on emerald-02.

A DSA or IAA user work queue (WQ) needs an administrator (`accel-config` or
`scripts/accel_conf.py`). On the emerald hosts all DSA devices and WQs are
disabled, and the IAA devices have only the kernel WQ of the `iaa_crypto`
driver. On sapphire (2026-11-17) an administrator enabled two user WQs on
`dsa0` (NUMA node 0), with their device files in the group `qat`:
`/dev/dsa/wq0.0` (shared) and `/dev/dsa/wq0.1` (dedicated), both with
`block_on_fault` 1. The [dsa modes](#dsa-accelerator) use them. The IAA WQ
`iax3/wq3.0` is not enabled: `accel-config` stops with "WQ priority is 0", so
it needs `accel-config config-wq iax3/wq3.0 --priority=10` before the enable.

### How a QAT offload completes

`QatDecryptOffload` does one AES-128-CBC decrypt at a time:

1. `sendmsg` on the AF_ALG operation socket gives the kernel the ciphertext,
   the operation and the IV. The kernel copies the ciphertext.
2. `io_submit` of a read (`IOCB_CMD_PREAD`) of the plaintext starts the
   decrypt on the device and returns at once (`-EIOCBQUEUED` in the kernel).
3. The device writes its response into the response ring of its bank in
   host memory and sends an MSI-X message (a PCIe memory write, remapped by
   the IOMMU to a vector of one CPU: `IR-PCI-MSIX`, `qat4-bundle1` on CPU 113).
   On that CPU, `adf_msix_isr_bundle` masks the bank interrupt and schedules
   a high-priority tasklet. At the interrupt exit, the softirq runs the
   tasklet `adf_response_handler`: `qat_alg_callback`,
   `qat_skcipher_alg_callback`, the skcipher completion, `af_alg_async_cb`
   and `aio_complete`. `aio_complete` writes the io_event into the AIO ring at
   `events[tail]`, then moves `tail` on. The tasklet then unmasks the bank
   interrupt. All of this runs on the interrupt CPU, in the time of the
   program that the interrupt stopped. The Linux 6.8 driver sets the
   interrupt coalescing timer of the bank to its minimum (511,
   `ADF_COALESCING_MIN_TIME`).
4. The AIO ring is mapped into user memory by `io_setup`. Before the submit,
   `prepare()` clears the `data` field of `events[tail]`; the completion sets it
   to 1 (`aio_data`). The low byte of `data` is the completion byte that
   `preempt_signal` points to.
5. After the run, `io_getevents` takes the event, and the output is compared
   with the plaintext. The ciphertext is made by ipp-crypto.

The constructor runs one offload and stops if the byte is not set or the
output is wrong. The driver binds by its driver name: `cbc(aes)` binds AES-NI,
which has a higher priority than QAT. The kernel picks the QAT instance on the
NUMA node of the CPU that sets the key, so the filler CPU sets the interrupt
CPU.

The device gives no completion time stamp. A `CompletionObserver` thread on
its own CPU loads the completion byte in a tight loop and reads the TSC when
the byte is set. So the detect time is from the observer seeing the byte.
`observer-lag.csv` (signal-floor from the interrupt CPU to the observer CPU)
gives the time from the kernel write to the observer seeing the byte; the
"from write" columns add its median.

- **qat-latency** is latency mode with the QAT offload. The completion time
  is the device time, not a chosen time, so the filler must run longer than
  the offload: `--size 1048576`.
- **qat-floor** loads the completion byte in a tight loop on the filler CPU,
  with no filler. The floor is the filler CPU seeing the byte minus the
  observer seeing it. It also gives the offload time at each size and after
  `--idle-us` of device idle.
- **qat-interrupt** gives the time of the interrupt path. Put the filler on
  the interrupt CPU and the observer on its SMT sibling. The filler CPU loads
  the completion byte and reads the TSC in a tight loop. A TSC step longer
  than 200 ns is a gap: the CPU was in the kernel. The last gap before the
  byte is set is the QAT interrupt, and the observer sees the kernel write of
  the byte inside it. Each run splits in four: the submit end to the gap
  start (device and delivery: the device, the coalescing timer and the MSI-X
  message, which do not split without a device time stamp), the gap start to
  the observer seeing the byte (the kernel path to the write, plus the
  observer lag), the observer seeing the byte to the gap end (the return,
  minus the observer lag) and the gap length (the CPU time of the interrupt).
  The other gaps (mostly timer ticks) give the cost of an interrupt with a
  short handler. The mode stops when the filler CPU handles fewer QAT
  interrupts than runs.

qat-latency and qat-floor count the involuntary context switches of the
filler thread and of the observer thread during the measured runs
(`nonvoluntary_ctxt_switches` of `/proc/self/task/TID/status`). A count above
0 means other work ran on their CPUs: use idle CPUs, or run again.

CSV columns:

- qat-latency: `mode,probe_interval,ipp_code_path,filler,size_bytes,offload_bytes,runs,yielded,finished_after_completion,finished_before_completion,wrong_outputs,failed_offloads,completed_during_submit,qat_interrupts,busiest_interrupt_cpu,busiest_interrupt_cpu_count,submit_median_ns,submit_p99_ns,offload_median_ns,offload_p99_ns,latency_median_ns,latency_p90_ns,latency_p99_ns,latency_max_ns,detect_median_ns,detect_p90_ns,detect_p99_ns,switch_median_ns,switch_p90_ns,switch_p99_ns,filler_median_ns,filler_involuntary_switches,observer_involuntary_switches`
- `--samples`: `probe_interval,ipp_code_path,filler,size_bytes,offload_bytes,run,submit_ns,offload_ns,outcome,detect_ns,switch_ns,latency_ns,output_correct,offload_output_correct`
- qat-floor: `mode,cpu,observer_cpu,offload_bytes,idle_us,runs,failed_offloads,completed_during_submit,qat_interrupts,busiest_interrupt_cpu,busiest_interrupt_cpu_count,submit_median_ns,submit_p99_ns,offload_median_ns,offload_p10_ns,offload_p90_ns,offload_p99_ns,floor_median_ns,floor_p10_ns,floor_p90_ns,floor_p99_ns,filler_involuntary_switches,observer_involuntary_switches`
- qat-interrupt: `mode,cpu,observer_cpu,offload_bytes,idle_us,runs,failed_offloads,completed_during_submit,runs_without_gap,seen_outside_gap,other_gaps,qat_interrupts,filler_cpu_interrupts,submit_median_ns,offload_median_ns`, then the median, p10, p90 and p99 of `device_and_delivery`, `entry_to_seen`, `seen_to_return` and `gap` (`..._median_ns,..._p10_ns,..._p90_ns,..._p99_ns`), then `other_gap_median_ns,other_gap_p90_ns`

### QAT results

Measured on emerald-02, filler on CPU 38 (second socket), 1 MiB inputs and
4 KiB offloads, unless a row says other. The QAT instance of the second
socket (`qat4-bundle1`) gives its interrupt on CPU 113. All filler and
offload outputs are correct, and no offload fails.

#### Offload

`qat-floor`, observer on CPU 110 (the SMT sibling of the filler CPU), 10,000
runs (200 runs with device idle).

| Offload | Device idle before | Submit median | Offload median | Offload p10 / p90 / p99 | Interrupts an offload | Done in submit | Floor median (p10 / p90) |
|---|---|---|---|---|---|---|---|
| 16 B | 0 | 7.68 µs | 14.7 µs | 14.3 / 15.1 / 26.7 µs | 1.00 | 0 / 10000 | −3 ns (−9 / 8) |
| 4 KiB | 0 | 8.55 µs | 18.2 µs | 17.8 / 18.6 / 30.6 µs | 1.00 | 1 / 10000 | 3 ns (−9 / 8) |
| 64 KiB | 0 | 43.5 µs | 88.1 µs | 86.7 / 99.3 / 149.3 µs | 1.00 | 0 / 10000 | 3 ns (−8 / 8) |
| 4 KiB | 100 µs | 8.52 µs | 18.3 µs | 18.0 / 19.1 / 46.7 µs | 1.02 | 0 / 200 | −3 ns (−9 / 8) |
| 4 KiB | 1 ms | 8.60 µs | 19.1 µs | 18.7 / 46.4 / 47.4 µs | 1.21 | 0 / 200 | −5 ns (−9 / 8) |
| 4 KiB | 10 ms | 9.20 µs | 47.9 µs | 47.0 / 48.6 / 57.2 µs | 2.91 | 0 / 200 | 3 ns (−8 / 8) |
| 4 KiB | 100 ms | 9.57 µs | 48.8 µs | 48.2 / 50.0 / 63.2 µs | 2.81 | 0 / 200 | 5 ns (−9 / 8) |

- The device is asynchronous: `io_submit` returns before the completion on
  all runs but one of 10,000 (a slow submit).
- The submit (the copy into the kernel and the request to the device) is
  about half of the offload. An offload costs about 14 µs and then about
  1.1 µs a KiB: one request at a time, not the peak rate of the device.
- After 10 ms or more of device idle, an offload takes 48 µs and gives about
  two more QAT interrupts (on `qat4-ae-cluster`): the device comes out of a
  power state. After 1 ms of idle, 10 % or more of the offloads pay this.
- On the SMT sibling the floor is −9 to 8 ns: the filler CPU sees the byte at
  the same time as the observer. So the detect times below start when the
  byte gets to the filler core.

#### Latency

`qat-latency`, observer on CPU 110, 1000 runs. The offload starts with the
filler, so the device sets the completion time. Observer lag (signal-floor
from CPU 113 to CPU 110) is 192 ns; "from write" adds it. Detect, switch and
latency are at interval 200; where the probe points are dense, interval 0
gives the same within 20 ns.

| Filler | Runtime (off) | Offload median | Yields 0 / 200 | Detect median / p99 | Switch median / p99 | Latency from write median / p99 | Latency off median |
|---|---|---|---|---|---|---|---|
| `deserialize` | 75.3 µs | 21.0 µs | 1000 / 1000 | 68.5 / 74.4 µs | 93 / 132 ns | 68.7 / 74.7 µs | 66.3 µs |
| `serialize` | 47.2 µs | 21.2 µs | 0 / 0 | 39.8 / 44.8 µs | 91 / 123 ns | 40.1 / 45.1 µs | 39.5 µs |
| `decrypt` | 97.9 µs | 20.6 µs | 0 / 0 | 84.6 / 91.6 µs | 84 / 93 ns | 84.8 / 91.9 µs | 88.9 µs |
| `decrypt-chunked` | 108.3 µs | 20.9 µs | 999 / 999 | 129 / 340 ns | 56 / 123 ns | 392 / 619 ns | 100.1 µs |
| `matmul` | 86.22 ms | 48.9 µs | 1000 / 1000 | 18 / 28 ns | 53 / 63 ns | 263 / 278 ns | 86.20 ms |
| `histogram` | 546.5 µs | 19.1 µs | 1000 / 1000 | 20 / 29 ns | 42 / 60 ns | 254 / 274 ns | 538.2 µs |
| `dotproduct` | 476.7 µs | 18.5 µs | 1000 / 1000 | 16 / 21 ns | 39 / 48 ns | 247 / 259 ns | 467.4 µs |
| `gather` | 268.3 µs | 21.7 µs | 1000 / 1000 | 51 / 58 ns | 54 / 99 ns | 297 / 345 ns | 258.9 µs |
| `scatter-update` | 463.4 µs | 22.0 µs | 1000 / 1000 | 52 / 66 ns | 54 / 65 ns | 298 / 317 ns | 454.4 µs |
| `hash` | 239.0 µs | 18.4 µs | 1000 / 1000 | 15 / 21 ns | 40 / 48 ns | 247 / 257 ns | 229.7 µs |
| `sequential-writer` | 15.2 µs | 19.2 µs | 744 / 740 | 20 / 25 ns | 53 / 65 ns | 264 / 279 ns | 5.86 µs |

- Where the probe points are dense, the latency from the kernel write is
  247-298 ns median and at most 345 ns p99. This is the latency of the
  emulated accelerator on another core of the socket (244-265 ns): 192 ns
  to move the cache line from the interrupt CPU, 15-52 ns to the next probe
  point and 39-56 ns to switch. The real device adds no other delay to the
  preemption.
- `gather` and `scatter-update` detect in about 50 ns, the other kernels in
  15-20 ns. At 1 MiB their loads miss the cache, and the yield retires only
  after the older loads. This cause is not measured.
- `decrypt-chunked` detects in 129 ns: the wait for the end of a chunk.
  `deserialize` yields only after the `bytes` copy; `serialize` and `decrypt`
  (native) do not yield. This is the same as with the emulated accelerator.
- The offload is 18-22 µs, not 18.2 µs as in `qat-floor`: the filler
  removes the kernel data from the cache. `matmul` (86 ms a run) leaves the
  device idle for 86 ms, so each of its offloads pays the power state
  (49 µs).
- `sequential-writer` (15 µs) ends before the device in 26 % of the runs.

#### Placement

`run-qat-bench.sh` with three CPU placements. The interrupt is on CPU 113 in
each placement (the filler CPU picks the QAT instance, and the kernel the
interrupt CPU of the instance). The latency columns are the dense kernels
(`matmul`, `histogram`, `dotproduct`, `gather`, `scatter-update`, `hash`,
`sequential-writer`) at interval 200.

| Placement | Filler / observer CPU | Floor median (p10 / p90) | Observer lag | Submit / offload median, 4 KiB | Latency from write median | Latency from write p99 |
|---|---|---|---|---|---|---|
| Observer on the SMT sibling | 38 / 110 | 3 ns (−9 / 8) | 192 ns | 8.55 / 18.2 µs | 247-298 ns | 257-345 ns |
| Observer on another core | 38 / 40 | −55 ns (−71 / 68) | 191 ns | 6.91 / 16.7 µs | 195-345 ns | 318-460 ns |
| Filler on the interrupt CPU | 113 / 41 | 466 ns (459 / 478) | 64 ns | 7.71 / 16.8 µs | 596-739 ns | 644-862 ns |

- The observer on the SMT sibling gives the time at which the byte gets to
  the filler core (floor within 9 ns). But its spin loop shares the core
  with the filler CPU: the submit takes 1.6 µs (24 %) more than with the
  observer on another core. The filler runtimes do not change.
- The observer on another core sees the byte 55 ns before or 68 ns after the
  filler CPU (two modes), so its detect times also have two modes (−40 or
  +65 ns). Inside this spread, the latency is the same as with the observer
  on the sibling.
- When the filler runs on the interrupt CPU, it sees the byte 466 ns after
  the observer on the sibling (530 ns after the write). The filler runs
  again only when the interrupt handler and the completion in softirq end.
  So the latency is more than two times the latency with the interrupt on
  another CPU. A user cannot move the interrupt (`smp_affinity` needs root),
  but can put the filler on another CPU of the same NUMA node.
- On the interrupt CPU, `decrypt-chunked` detects in 1.7-1.9 µs (129 ns
  with the interrupt on another CPU), and its `off` runtime is 12-16 µs longer
  (120 µs). The cause is not studied yet.

#### Interrupt path

`qat-interrupt`, filler on CPU 113 (the interrupt CPU), observer on CPU 41
(its SMT sibling), 10,000 runs (200 runs with device idle). Observer lag
(signal-floor from CPU 113 to CPU 41) is 63 ns: the write is the observer
time minus 63 ns. In each row, CPU 113 handles one QAT interrupt a run, and at
most 16 of 10,000 runs are not in the split (done in the submit, or the
observer saw the byte outside the gap).

| Offload | Device idle before | Submit median | Offload median | Device and delivery median | Interrupt to write median (p99) | Write to return median | Gap median (p99) |
|---|---|---|---|---|---|---|---|
| 16 B | 0 | 6.70 µs | 13.2 µs | 3.64 µs | 2.79 µs (14.5 µs) | 535 ns | 3.33 µs (15.0 µs) |
| 4 KiB | 0 | 7.80 µs | 16.9 µs | 5.89 µs | 3.13 µs (14.9 µs) | 533 ns | 3.66 µs (15.5 µs) |
| 8 KiB | 0 | 10.1 µs | 23.2 µs | 9.13 µs | 3.85 µs (15.7 µs) | 536 ns | 4.39 µs (16.3 µs) |
| 16 KiB | 0 | 14.7 µs | 33.1 µs | 13.1 µs | 5.27 µs (17.3 µs) | 528 ns | 5.80 µs (17.9 µs) |
| 32 KiB | 0 | 23.7 µs | 50.3 µs | 18.5 µs | 7.95 µs (20.3 µs) | 532 ns | 8.48 µs (20.9 µs) |
| 64 KiB | 0 | 41.6 µs | 84.4 µs | 29.5 µs | 13.0 µs (28.6 µs) | 528 ns | 13.5 µs (29.5 µs) |
| 4 KiB | 10 ms | 7.95 µs | 45.5 µs | 33.6 µs | 3.61 µs (3.99 µs) | 535 ns | 4.14 µs (4.55 µs) |

- The kernel writes the completion byte 2.79 µs (16 B) to 3.13 µs (4 KiB)
  after the interrupt gets to the CPU, and 0.66-0.72 µs later for each
  4 KiB page after the first (13.0 µs at 64 KiB). A device that writes its
  completion record itself (DSA, IAA, the emulated accelerator) does not
  have this delay. The per-page cause is not measured (no kernel profiler:
  `perf_event_paranoid` is 4). The completion unmaps (IOMMU in lazy mode,
  `DMA-FQ`) and releases the source and destination pages.
- Device and delivery holds the device time, the coalescing timer and the
  MSI-X message. At 16 B it is 3.64 µs, so the coalescing timer and the MSI-X
  delivery together are at most 3.64 µs. Relative to a direct write at the
  device end, the interrupt makes the completion 2.79 µs (16 B) to 6.43 µs
  later.
- From the write to the return to the program is 528-536 ns at each size.
  This is the 466 ns floor of the "Filler on the interrupt CPU" placement
  (plus the 63 ns lag).
- The gap is the CPU time that each completion takes from the program on the
  interrupt CPU: 3.33 µs (16 B), 3.66 µs (4 KiB), 13.5 µs (64 KiB). The
  other gaps on the same CPU (mostly timer ticks, 66 to 301 a row from
  8 KiB) have a median of 3.82-4.02 µs.
- 1 % of the completions take about 11 µs more in the kernel (p99). The
  cause is not measured.
- After 10 ms of device idle, the device and delivery part is 33.6 µs and the
  kernel path does not change (3.61 µs): the power state is in the device.

## DSA accelerator

The dsa modes use a DSA device of sapphire (`dsa0`, NUMA node 0; see
[the lab hosts](#qat-dsa-and-iaa-on-the-lab-hosts)). As with QAT, the probe
points and the filler builds do not change. Only the completion byte changes:
it is the status byte of the completion record that the device writes itself.
No kernel code and no interrupt are on the completion path.

### How a DSA offload completes

`DsaMemmoveOffload` does one memory move (`DSA_OPCODE_MEMMOVE`) at a time:

1. `prepare()` clears the destination and the next completion record (one
   64-byte line of a pool of 1024 lines, as in `CompletionRecordPool`) and
   puts the record address in the descriptor. The status byte of the record
   is the completion byte that `preempt_signal` points to.
2. `submit()` gives the 64-byte descriptor to the work queue (WQ). The fast
   path is one instruction to the mapped WQ portal: `MOVDIR64B` to a
   dedicated WQ, `ENQCMD` to a shared WQ. On sapphire the driver refuses the
   `mmap` of the portal (`EPERM`): the first DSA (Sapphire Rapids) has a
   user-submission erratum (CVE-2024-21823), and the Linux 6.8 `idxd` driver
   then lets only a process with `CAP_SYS_RAWIO` map the portal. So the
   program gives the descriptor with `write()` on the device file. The driver
   checks it (no batch descriptor, an aligned completion record) and writes
   it to the portal: `MOVDIR64B` to a dedicated WQ, `ENQCMDS` with the PASID
   of the process to a shared WQ. This adds a system call to the submit; the
   completion does not change.
3. The device reads the source and writes the destination at the virtual
   addresses of the program (shared virtual memory through the PASID; with
   `block_on_fault` 1 the device waits on a page fault until the kernel maps
   the page). Then it writes the 32-byte completion record: status 1 is
   success. The descriptor does not ask for a completion interrupt, so the
   `idxd` lines of `/proc/interrupts` do not change.
4. `reap()` waits for the status, and the destination is compared with the
   source.

The constructor reads the WQ mode, `block_on_fault` and `max_transfer_size`
from `/sys/bus/dsa/devices/<wq>/`, and runs one offload. It stops if the
offload fails or the output is wrong.

The device gives no completion time stamp, so the `CompletionObserver` of the
qat modes reads the TSC when it sees the status byte. Put the observer on the
SMT sibling of the filler CPU: the device writes the record, so the two CPUs
see the byte at the same time, and the times start when the byte gets to the
filler core. There is no kernel write, so there is no observer lag to add.

- **dsa-latency** is qat-latency with the DSA offload (`--size 1048576`).
- **dsa-floor** is qat-floor with the DSA offload.
- `--work-queue PATH` picks the WQ (default `/dev/dsa/wq0.1`).
- The CSV columns are those of qat-latency and qat-floor, with
  `idxd_interrupts` in place of `qat_interrupts`.

```
scripts/run-dsa-bench.sh results/dsa 2 42 --size 1048576
scripts/summarize-dsa.py results/dsa
```

`run-dsa-bench.sh` runs each WQ that the user can open: dsa-floor at 16 B,
4 KiB and 64 KiB and after 0.1-100 ms of device idle, and dsa-latency of each
build. `work-queues.csv` gives the mode and `block_on_fault` of each WQ.

### DSA results

Measured on sapphire (2026-11-17), filler on CPU 2 and observer on CPU 42
(the SMT sibling of CPU 2), both on NUMA node 0 with `dsa0`, 1 MiB inputs
and 4 KiB offloads, unless a row says other. The submit is the `write()`
system call (see step 2 above). All filler and offload outputs are correct,
and no offload fails. No row has an `idxd` interrupt (0.00 an offload), so
the tables have no interrupt column. Other users' jobs ran on the host
during the runs (load average 76-91 on 80 CPUs): the switch columns give the
involuntary context switches of the filler and of the observer during the
measured runs.

#### Offload

`dsa-floor`, 10,000 runs (200 runs with device idle). `wq0.1` is the
dedicated WQ, `wq0.0` the shared WQ.

| WQ | Offload | Device idle before | Submit median | Offload median | Offload p10 / p90 / p99 | Done in submit | Floor median (p10 / p90) | Switches filler / observer |
|---|---|---|---|---|---|---|---|---|
| wq0.1 | 16 B | 0 | 1.19 µs | 1.61 µs | 1.43 / 1.73 / 1.91 µs | 21 / 10000 | 2 ns (−6 / 5) | 0 / 0 |
| wq0.1 | 4 KiB | 0 | 1.17 µs | 2.14 µs | 2.06 / 2.28 / 3.15 µs | 5 / 10000 | −2 ns (−6 / 6) | 0 / 0 |
| wq0.1 | 64 KiB | 0 | 1.24 µs | 7.87 µs | 7.59 / 8.25 / 9.52 µs | 1 / 10000 | 2 ns (−6 / 6) | 1 / 1 |
| wq0.1 | 4 KiB | 100 µs | 1.28 µs | 3.24 µs | 3.06 / 3.51 / 4.99 µs | 0 / 200 | 2 ns (−6 / 6) | 0 / 0 |
| wq0.1 | 4 KiB | 1 ms | 1.71 µs | 3.73 µs | 2.88 / 4.29 / 5.15 µs | 0 / 200 | −5 ns (−6 / 7) | 0 / 1 |
| wq0.1 | 4 KiB | 10 ms | 3.12 µs | 4.20 µs | 3.46 / 6.03 / 9.58 µs | 5 / 200 | 3 ns (−6 / 6) | 25 / 2 |
| wq0.1 | 4 KiB | 100 ms | 5.75 µs | 6.21 µs | 5.34 / 7.35 / 71.2 µs | 32 / 200 | 5 ns (−7 / 7) | 77 / 10 |
| wq0.0 | 16 B | 0 | 1.41 µs | 1.63 µs | 1.56 / 1.72 / 1.86 µs | 480 / 10000 | −3 ns (−7 / 7) | 11 / 11 |
| wq0.0 | 4 KiB | 0 | 1.35 µs | 3.48 µs | 3.31 / 3.64 / 3.91 µs | 5 / 10000 | −2 ns (−6 / 6) | 0 / 1 |
| wq0.0 | 64 KiB | 0 | 1.38 µs | 8.14 µs | 7.76 / 8.66 / 9.41 µs | 1 / 10000 | 2 ns (−6 / 6) | 0 / 5 |
| wq0.0 | 4 KiB | 100 µs | 1.41 µs | 3.03 µs | 2.80 / 3.22 / 7.92 µs | 0 / 200 | −4 ns (−7 / 6) | 7 / 1 |
| wq0.0 | 4 KiB | 1 ms | 1.89 µs | 3.58 µs | 3.20 µs / 5.65 µs / 2.99 ms | 0 / 200 | −4 ns (−7 / 6) | 1 / 12 |
| wq0.0 | 4 KiB | 10 ms | 3.82 µs | 4.78 µs | 3.83 / 5.70 / 10.2 µs | 10 / 200 | 3 ns (−6 / 6) | 8 / 2 |
| wq0.0 | 4 KiB | 100 ms | 6.80 µs | 7.16 µs | 5.82 / 10.1 / 95.8 µs | 90 / 200 | 6 ns (−7 / 153) | 237 / 30 |

- The submit with `write()` is 1.17-1.41 µs (QAT `io_submit`: 7.68-8.55 µs).
  The portal instruction would remove the system call, but on this DSA only
  a process with `CAP_SYS_RAWIO` can map the portal.
- An offload costs about 2.1-3.5 µs at 4 KiB and then about 0.08-0.10 µs a
  KiB (QAT: about 1.1 µs a KiB), one descriptor at a time.
- At 16 B the byte is set 0.42 µs (dedicated) or 0.22 µs (shared) after
  `write()` returns, and on the shared WQ it is set before `write()`
  returns in 480 of 10,000 runs. The device starts inside `write()`, so the
  device and the record write take between this time and the offload
  (1.61-1.63 µs). The device gives no time stamp, so the time is not
  measured.
- At 4 KiB the shared WQ is 1.34 µs slower than the dedicated WQ, but at
  16 B and 64 KiB the two are within 0.27 µs. Both WQs are in `group0.0`
  with `engine0.0` and priority 10; only the WQ threshold is different (31
  vs. 0). The cause is not measured.
- After 100 ms of device idle the submit is 4.6-5.5 µs slower, and the
  offload is 3.7-4.1 µs slower, so the device adds no time of its own (QAT:
  30 µs more after 10 ms, from the power state of the device). The cause of
  the slow submit is not measured: the `write()` path is not in the caches
  and the TLB after the sleep, and other jobs ran on CPU 2 during the sleep
  (77-237 filler switches).
- On the SMT sibling the floor is −7 to 7 ns (p10 / p90) in all rows but
  the shared WQ after 100 ms of idle (p90 153 ns, 237 filler switches): the
  filler CPU sees the byte at the same time as the observer. So the detect
  times below start when the byte gets to the filler core.

#### Latency

`dsa-latency` on the dedicated WQ (`wq0.1`), 1000 runs. The offload starts
with the filler, so the device sets the completion time. Detect, switch,
latency and the switch counts are at interval 200. There is no kernel write,
so there is no observer lag to add: the latency is from the byte at the
filler core.

| Filler | Runtime (off) | Offload median | Yields 0 / 200 | Detect median / p99 | Switch median / p99 | Latency median / p99 | Latency off median | Switches filler / observer |
|---|---|---|---|---|---|---|---|---|
| `deserialize` | 61.9 µs | 4.62 µs | 1000 / 1000 | 59.9 / 66.3 µs | 45 / 80 ns | 59.9 / 66.3 µs | 60.0 µs | 1 / 1 |
| `serialize` | 38.1 µs | 4.14 µs | 17 / 115 | 40.0 / 47.4 µs | 99 / 120 ns | 40.1 / 47.5 µs | 37.9 µs | 4 / 2 |
| `decrypt` | 81.2 µs | 4.63 µs | 8 / 41 | 87.8 / 98.8 µs | 107 / 129 ns | 87.9 / 98.9 µs | 81.0 µs | 2 / 2 |
| `decrypt-chunked` | 91.2 µs | 4.41 µs | 1000 / 1000 | 386 ns / 1.31 µs | 34 / 51 ns | 420 ns / 1.35 µs | 90.6 µs | 19 / 1 |
| `matmul` | 69.45 ms | 6.66 µs | 1000 / 1000 | 11 / 307 ns | 35 / 162 ns | 48 / 400 ns | 67.52 ms | 186 / 132 |
| `histogram` | 465.6 µs | 3.49 µs | 1000 / 1000 | 11 / 40 ns | 31 / 78 ns | 42 / 88 ns | 453.5 µs | 2 / 0 |
| `dotproduct` | 391.6 µs | 3.54 µs | 1000 / 1000 | 9 / 31 ns | 31 / 49 ns | 39 / 66 ns | 386.8 µs | 0 / 0 |
| `gather` | 218.9 µs | 5.71 µs | 1000 / 1000 | 37 / 198 ns | 41 / 132 ns | 78 / 251 ns | 216.9 µs | 4 / 0 |
| `scatter-update` | 352.9 µs | 5.07 µs | 1000 / 1000 | 46 / 302 ns | 37 / 159 ns | 125 / 358 ns | 366.4 µs | 3 / 0 |
| `hash` | 194.7 µs | 3.79 µs | 1000 / 1000 | 8 / 15 ns | 31 / 35 ns | 39 / 46 ns | 187.3 µs | 0 / 0 |
| `sequential-writer` | 12.0 µs | 3.92 µs | 1000 / 1000 | 10 / 116 ns | 38 / 400 ns | 49 / 450 ns | 10.8 µs | 3 / 0 |

The dense kernels (`matmul`, `histogram`, `dotproduct`, `gather`,
`scatter-update`, `hash`, `sequential-writer`) on the two WQs, with the
switch counts summed over the 7 kernels:

| WQ | Interval | Submit / offload median, 4 KiB | Detect median | Switch median | Latency median | Latency p99 | Switches filler / observer |
|---|---|---|---|---|---|---|---|
| wq0.1, dedicated | 200 | 1.17 / 2.14 µs | 8-46 ns | 31-41 ns | 39-125 ns | 46-450 ns | 198 / 132 |
| wq0.1, dedicated | 0 | 1.17 / 2.14 µs | 7-39 ns | 30-39 ns | 38-78 ns | 47-399 ns | 349 / 352 |
| wq0.0, shared | 200 | 1.35 / 3.48 µs | 7-35 ns | 31-39 ns | 37-72 ns | 48-444 ns | 163 / 104 |
| wq0.0, shared | 0 | 1.35 / 3.48 µs | 6-36 ns | 31-43 ns | 38-78 ns | 54-566 ns | 203 / 437 |

- Where the probe points are dense, the program switches 37-125 ns median
  after the byte gets to its core: 7-46 ns to the next probe point and
  31-41 ns to switch. The detect and the switch are those of QAT (15-52 and
  39-56 ns), but there is no kernel path before them (QAT: 3.13 µs from the
  interrupt to the kernel write) and no cache-line move from an interrupt
  CPU (QAT: 192 ns of the 247-298 ns from the write).
- The emulated accelerator on the SMT sibling gives 109-146 ns with a floor
  of 62 ns: 47-84 ns without the floor, as the DSA latency. Its times start
  at the write of the byte on the sibling CPU, so they include the floor.
  The DSA times start when the byte gets to the filler core: the write of
  the record from the device to the core is not measured.
- `gather` and `scatter-update` detect in 34-46 ns, the other kernels in
  7-11 ns, as with QAT. On `wq0.1` the `scatter-update` detect has two modes
  (p25 37 ns, p75 125 ns), so its latency median (125 ns) is more than the
  detect and switch medians. The cause is not measured.
- A p99 above 250 ns comes with a switch p99 of 130-400 ns and with
  involuntary switches: `matmul` (69 ms a run) has 149-186 filler switches
  in 1000 runs, `hash` on `wq0.0` has a switch p99 of 394 ns and
  `sequential-writer` on `wq0.1` 400 ns. These are other jobs on the CPUs.
- `decrypt-chunked` detects in 386-494 ns median, p99 1.3 µs (QAT: 129 ns).
  The cause is not measured.
- `deserialize` yields only after the `bytes` copy, as with QAT. `serialize`
  yields in 11-115 and `decrypt` in 8-113 of 1000 runs (QAT: 0). The DSA
  offload ends after about 4 µs, not 21 µs; a likely cause is that the byte
  is set while the filler is still in probed set-up code. This cause is not
  verified.
- The offload median is 2.5-6.9 µs (`dsa-floor` at 4 KiB: 2.1-3.5 µs). It
  is 4.1-5.7 µs next to `deserialize`, `serialize`, `decrypt`, `gather` and
  `scatter-update`, and 6.7-6.9 µs next to `matmul` (69 ms a run), as after
  100 ms of idle in `dsa-floor`. The cause is not measured.
- `sequential-writer` (12 µs) yields in all runs: the 4 µs offload ends
  before the filler (QAT: the filler ends before the device in 26 % of the
  runs).
- A run on an idle host would remove most of the switches and the high p99
  values.

## Limits

- **Code with no loops in IR gets no probes.** The pass probes loop headers
  only; there are no function-entry probes.
  - The `bytes` field of `RouterRequest` is copied by one `memcpy`, so
    `deserialize` waits for the copy, and `serialize` executes no probe
    point after the copy starts.
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
- **The QAT completion goes through the kernel.** The device does not write
  the completion byte: the kernel does, in the interrupt of the device. So the
  QAT detect time includes a cache-line transfer from the interrupt CPU, as
  with the emulated accelerator. DSA and IAA write a completion record from
  the device: see [DSA accelerator](#dsa-accelerator). The RACER applications in `src/` are
  not yet linked to these probed builds.
- **Use idle CPUs for the bench.** On a loaded host, `late_completions` rises
  and the latency includes the delay of the accelerator thread.
