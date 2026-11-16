// preempt_bench: probe-point accelerator preemption with an emulated
// accelerator.
//
// This file is the application and is not probed. The fillers it runs call
// probed code: the RACER kernels, protobuf (with the generated router.pb.cc)
// and ipp-crypto. This file gives the strong definitions of the probe symbols:
//   preempt_signal  points at the status byte of the completion record of the
//                   offload in flight, and at a byte that is never set when
//                   no offload is in flight
//   do_yield        swaps from the filler fiber back to the scheduler, as
//                   do_yield in RACER's src/baseline.cpp does
//
// Modes:
//   overhead  runs each filler with no offload in flight and reports its
//             runtime. Compare a build with probes to the off build.
//   latency   submits an offload to the emulated accelerator, runs the filler
//             in a fiber, and measures the time from the accelerator
//             completion to the scheduler getting the core back (a yield at a
//             probe point, or the end of the filler). Then it resumes the
//             filler to its end and checks its output.
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "aes_gcm_chunked.h"
#include "ch3_hash.h"
#include "dotproduct.h"
#include "gather.h"
#include "ippcp.h"
#include "matmul_histogram.h"
#include "racer_probe.h"
#include "router.pb.h"
#include "sequential_writer.h"
extern "C" {
#include "fcontext.h"
}

// ---------------------------------------------------------------------------
// Probe symbols

// preempt_signal points here whenever no offload is in flight, so that probe
// points outside a filler (protobuf calls in the setup code) never yield.
static uint8_t signal_byte_never_set = 0;
__thread volatile uint8_t *preempt_signal = &signal_byte_never_set;

static thread_local fcontext_transfer_t scheduler_transfer;
static thread_local bool inside_filler_fiber = false;
static thread_local bool filler_finished = false;
// The time the filler last gave the core back: at a yield or at its end.
static thread_local uint64_t scheduler_return_tsc = 0;

static inline uint64_t read_tsc() {
  _mm_lfence();
  uint64_t tsc = __rdtsc();
  _mm_lfence();
  return tsc;
}

extern "C" void do_yield(void) {
  uint64_t yield_tsc = read_tsc();
  if (!inside_filler_fiber) {
    fprintf(stderr, "do_yield outside a filler fiber: preempt_signal points at a set byte\n");
    abort();
  }
  scheduler_return_tsc = yield_tsc;
  scheduler_transfer = fcontext_swap(scheduler_transfer.prev_context, nullptr);
}

// ---------------------------------------------------------------------------
// Helpers

static void pin_current_thread_to_cpu(int cpu) {
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpu, &cpu_set);
  int error = pthread_setaffinity_np(pthread_self(), sizeof cpu_set, &cpu_set);
  if (error != 0) {
    fprintf(stderr, "cannot pin a thread to CPU %d: %s\n", cpu, strerror(error));
    exit(1);
  }
}

static double monotonic_ns() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
  return now.tv_sec * 1e9 + now.tv_nsec;
}

class TscClock {
 public:
  // Counts TSC ticks over 100 ms of CLOCK_MONOTONIC_RAW.
  static TscClock calibrate() {
    double start_ns = monotonic_ns();
    uint64_t start_tsc = read_tsc();
    while (monotonic_ns() - start_ns < 100e6) {
    }
    double elapsed_ns = monotonic_ns() - start_ns;
    uint64_t elapsed_ticks = read_tsc() - start_tsc;
    return TscClock(elapsed_ticks / elapsed_ns);
  }
  double ticks_to_ns(double ticks) const { return ticks / ticks_per_ns; }
  uint64_t ns_to_ticks(double ns) const { return static_cast<uint64_t>(ns * ticks_per_ns); }

 private:
  explicit TscClock(double ticks_per_ns) : ticks_per_ns(ticks_per_ns) {}
  double ticks_per_ns;
};

static uint64_t fnv1a_64(const void *data, size_t length, uint64_t hash = 0xcbf29ce484222325ULL) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0; index < length; index++) {
    hash = (hash ^ bytes[index]) * 0x100000001b3ULL;
  }
  return hash;
}

static std::vector<uint8_t> random_bytes(size_t count, std::mt19937_64 &random) {
  std::vector<uint8_t> bytes(count);
  for (uint8_t &byte : bytes) {
    byte = static_cast<uint8_t>(random());
  }
  return bytes;
}

// Nearest-rank percentile; fraction 0.5 is the median.
static double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) {
    return NAN;
  }
  std::sort(values.begin(), values.end());
  size_t index = static_cast<size_t>(std::lround(fraction * (values.size() - 1)));
  return values[index];
}

// ---------------------------------------------------------------------------
// Fillers: the work a core does while an offload is in flight.

class Filler {
 public:
  virtual ~Filler() = default;
  // Restores the inputs that run() changes. Not timed.
  virtual void reset() {}
  // The work. Probe points in the code it calls can yield.
  virtual void run() = 0;
  // A checksum of the output of the last run.
  virtual uint64_t checksum() const = 0;
};

// RACER's deserialize step (src/dddfd.cpp): a router request with RACER's key
// and a value of size_bytes. The probe points are in the protobuf parser and
// in router.pb.cc; the copy of the value is glibc memcpy, which has none.
class DeserializeFiller : public Filler {
 public:
  DeserializeFiller(size_t size_bytes, std::mt19937_64 &random) {
    router::RouterRequest request;
    request.set_key("/region/cluster/foo:key|#|etc");
    std::vector<uint8_t> value = random_bytes(size_bytes, random);
    request.set_value(value.data(), value.size());
    request.set_operation(1);
    serialized_request = request.SerializeAsString();
  }
  void run() override {
    if (!parsed_request.ParseFromArray(serialized_request.data(),
                                       static_cast<int>(serialized_request.size()))) {
      fprintf(stderr, "deserialize: the router request does not parse\n");
      abort();
    }
  }
  uint64_t checksum() const override {
    uint64_t hash = fnv1a_64(parsed_request.key().data(), parsed_request.key().size());
    hash = fnv1a_64(parsed_request.value().data(), parsed_request.value().size(), hash);
    uint32_t operation = parsed_request.operation();
    return fnv1a_64(&operation, sizeof operation, hash);
  }

 private:
  std::string serialized_request;
  router::RouterRequest parsed_request;
};

// RACER's decrypt step (include/inline/decrypt.ipp): AES-128-GCM with RACER's
// key and IV. chunk_bytes 0 decrypts with one ippsAES_GCMDecrypt call, as
// RACER does; other values decrypt with one call a chunk (aes_gcm_chunked.h).
class DecryptFiller : public Filler {
 public:
  DecryptFiller(size_t size_bytes, int chunk_bytes, std::mt19937_64 &random)
      : chunk_bytes(chunk_bytes),
        length(static_cast<int>(size_bytes)),
        plaintext(random_bytes(size_bytes, random)),
        ciphertext(size_bytes),
        decrypted(size_bytes) {
    int state_size = 0;
    check(ippsAES_GCMGetSize(&state_size), "ippsAES_GCMGetSize");
    state_memory.resize(state_size);
    state = reinterpret_cast<IppsAES_GCMState *>(state_memory.data());
    check(ippsAES_GCMInit(key, key_bytes, state, state_size), "ippsAES_GCMInit");
    start_message();
    check(ippsAES_GCMEncrypt(plaintext.data(), ciphertext.data(), length, state),
          "ippsAES_GCMEncrypt");
    run();
    if (decrypted != plaintext) {
      fprintf(stderr, "decrypt: the decrypted bytes are not the plaintext\n");
      abort();
    }
  }
  void run() override {
    start_message();
    if (chunk_bytes == 0) {
      check(ippsAES_GCMDecrypt(ciphertext.data(), decrypted.data(), length, state),
            "ippsAES_GCMDecrypt");
    } else {
      check(racer_aes_gcm_decrypt_chunked(ciphertext.data(), decrypted.data(), length, chunk_bytes,
                                          state),
            "racer_aes_gcm_decrypt_chunked");
    }
  }
  uint64_t checksum() const override { return fnv1a_64(decrypted.data(), decrypted.size()); }

 private:
  static constexpr Ipp8u key[] = "0123456789abcdef";
  static constexpr int key_bytes = 16;
  static constexpr Ipp8u iv[] = "0123456789ab";
  static constexpr int iv_bytes = 12;
  static constexpr int aad_bytes = 16;

  static void check(IppStatus status, const char *function) {
    if (status != ippStsNoErr) {
      fprintf(stderr, "decrypt: %s returned %d\n", function, status);
      abort();
    }
  }
  void start_message() {
    const Ipp8u aad[aad_bytes] = {};
    check(ippsAES_GCMReset(state), "ippsAES_GCMReset");
    check(ippsAES_GCMStart(iv, iv_bytes, aad, aad_bytes, state), "ippsAES_GCMStart");
  }

  int chunk_bytes;
  int length;
  std::vector<Ipp8u> plaintext;
  std::vector<Ipp8u> ciphertext;
  std::vector<Ipp8u> decrypted;
  std::vector<Ipp8u> state_memory;
  IppsAES_GCMState *state = nullptr;
};

// Phoenix matrix multiply (matrix_mult): three int matrices that together take
// about size_bytes.
class MatmulFiller : public Filler {
 public:
  MatmulFiller(size_t size_bytes, std::mt19937_64 &random) {
    int matrix_len = std::max(1, static_cast<int>(std::sqrt(size_bytes / (3.0 * sizeof(int)))));
    size_t element_count = static_cast<size_t>(matrix_len) * matrix_len;
    std::uniform_int_distribution<int> small_value(0, 15);
    matrix_a.resize(element_count);
    matrix_b.resize(element_count);
    matrix_out.resize(element_count);
    for (size_t index = 0; index < element_count; index++) {
      matrix_a[index] = small_value(random);
      matrix_b[index] = small_value(random);
    }
    matrices = {matrix_a.data(), matrix_b.data(), matrix_out.data(), matrix_len};
  }
  // matrix_mult adds to matrix_out.
  void reset() override { std::fill(matrix_out.begin(), matrix_out.end(), 0); }
  void run() override { matrix_mult(&matrices); }
  uint64_t checksum() const override {
    return fnv1a_64(matrix_out.data(), matrix_out.size() * sizeof(int));
  }

 private:
  std::vector<int> matrix_a;
  std::vector<int> matrix_b;
  std::vector<int> matrix_out;
  mm_data_t matrices;
};

// Phoenix histogram (calc_hist): a 24-bit BMP of about size_bytes. calc_hist
// reads the pixel offset at byte 10 and counts the blue, green and red bytes.
class HistogramFiller : public Filler {
 public:
  HistogramFiller(size_t size_bytes, std::mt19937_64 &random) {
    size_t pixel_count = std::max<size_t>(1, (size_bytes - std::min(size_bytes, header_bytes)) / 3);
    image = random_bytes(header_bytes + 3 * pixel_count, random);
    image[IMG_DATA_OFFSET_POS] = header_bytes;
    image[BITS_PER_PIXEL_POS] = 24;
    color_counts.resize(256 * 3);
  }
  void run() override {
    int output_size = 0;
    calc_hist(image.data(), color_counts.data(), static_cast<int>(image.size()), &output_size);
  }
  uint64_t checksum() const override {
    return fnv1a_64(color_counts.data(), color_counts.size() * sizeof(int));
  }

 private:
  static constexpr size_t header_bytes = 54;
  std::vector<uint8_t> image;
  std::vector<int> color_counts;
};

// dotproduct: size_bytes of floats against RACER's fixed 8-float document.
class DotproductFiller : public Filler {
 public:
  DotproductFiller(size_t size_bytes, std::mt19937_64 &random) {
    std::uniform_real_distribution<float> unit_value(0.0f, 1.0f);
    features.resize(size_bytes / sizeof(float));
    for (float &feature : features) {
      feature = unit_value(random);
    }
  }
  void run() override {
    int output_size = 0;
    dotproduct(features.data(), &product, static_cast<int>(features.size() * sizeof(float)),
               &output_size);
  }
  uint64_t checksum() const override { return fnv1a_64(&product, sizeof product); }

 private:
  std::vector<float> features;
  float product = 0;
};

// gather_using_indir_array: reads a float table of size_bytes at random
// indices, one read for each table entry.
class GatherFiller : public Filler {
 public:
  GatherFiller(size_t size_bytes, std::mt19937_64 &random) {
    size_t entry_count = size_bytes / sizeof(float);
    std::uniform_int_distribution<int> table_index(0, static_cast<int>(entry_count) - 1);
    table.resize(entry_count);
    indices.resize(entry_count);
    gathered.resize(entry_count);
    for (size_t index = 0; index < entry_count; index++) {
      table[index] = static_cast<float>(index);
      indices[index] = table_index(random);
    }
  }
  void run() override {
    gather_using_indir_array(table.data(), gathered.data(), indices.data(),
                             static_cast<int>(indices.size()));
  }
  uint64_t checksum() const override {
    return fnv1a_64(gathered.data(), gathered.size() * sizeof(float));
  }

 private:
  std::vector<float> table;
  std::vector<int> indices;
  std::vector<float> gathered;
};

// scatter_update_inplace_using_indir_array: increments bytes of a size_bytes
// array at random indices, one update for each 4 bytes.
class ScatterUpdateFiller : public Filler {
 public:
  ScatterUpdateFiller(size_t size_bytes, std::mt19937_64 &random)
      : original_bytes(random_bytes(size_bytes, random)), updated_bytes(original_bytes) {
    std::uniform_int_distribution<int> byte_index(0, static_cast<int>(size_bytes) - 1);
    indices.resize(size_bytes / sizeof(int));
    for (int &index : indices) {
      index = byte_index(random);
    }
  }
  void reset() override { updated_bytes = original_bytes; }
  void run() override {
    scatter_update_inplace_using_indir_array(updated_bytes.data(), indices.data(),
                                             static_cast<int>(indices.size()));
  }
  uint64_t checksum() const override {
    return fnv1a_64(updated_bytes.data(), updated_bytes.size());
  }

 private:
  std::vector<uint8_t> original_bytes;
  std::vector<uint8_t> updated_bytes;
  std::vector<int> indices;
};

// RACER's hash step (src/baseline.cpp): furc_hash of a size_bytes key over 16
// shards.
class HashFiller : public Filler {
 public:
  HashFiller(size_t size_bytes, std::mt19937_64 &random) : key(random_bytes(size_bytes, random)) {}
  void run() override {
    shard = furc_hash(reinterpret_cast<const char *>(key.data()), key.size(), 16);
  }
  uint64_t checksum() const override { return shard; }

 private:
  std::vector<uint8_t> key;
  uint32_t shard = 0;
};

// sequential_writer: vector loads over a 64-byte aligned buffer of size_bytes.
// It has no output, so the checksum is that of the buffer.
class SequentialWriterFiller : public Filler {
 public:
  SequentialWriterFiller(size_t size_bytes, std::mt19937_64 &random)
      : size_bytes(size_bytes), buffer(static_cast<char *>(aligned_alloc(64, size_bytes)), free) {
    std::vector<uint8_t> contents = random_bytes(size_bytes, random);
    memcpy(buffer.get(), contents.data(), size_bytes);
  }
  void run() override { sequential_writer(buffer.get(), size_bytes); }
  uint64_t checksum() const override { return fnv1a_64(buffer.get(), size_bytes); }

 private:
  size_t size_bytes;
  std::unique_ptr<char, decltype(&free)> buffer;
};

constexpr int decrypt_chunk_bytes = 4096;

class OneCallDecryptFiller : public DecryptFiller {
 public:
  OneCallDecryptFiller(size_t size_bytes, std::mt19937_64 &random)
      : DecryptFiller(size_bytes, 0, random) {}
};

class ChunkedDecryptFiller : public DecryptFiller {
 public:
  ChunkedDecryptFiller(size_t size_bytes, std::mt19937_64 &random)
      : DecryptFiller(size_bytes, decrypt_chunk_bytes, random) {}
};

template <typename FillerType>
std::unique_ptr<Filler> make_filler(size_t size_bytes, std::mt19937_64 &random) {
  return std::make_unique<FillerType>(size_bytes, random);
}

struct FillerKind {
  const char *name;
  std::unique_ptr<Filler> (*make)(size_t size_bytes, std::mt19937_64 &random);
};

static const FillerKind filler_kinds[] = {
    {"deserialize", make_filler<DeserializeFiller>},
    {"decrypt", make_filler<OneCallDecryptFiller>},
    {"decrypt-chunked", make_filler<ChunkedDecryptFiller>},
    {"matmul", make_filler<MatmulFiller>},
    {"histogram", make_filler<HistogramFiller>},
    {"dotproduct", make_filler<DotproductFiller>},
    {"gather", make_filler<GatherFiller>},
    {"scatter-update", make_filler<ScatterUpdateFiller>},
    {"hash", make_filler<HashFiller>},
    {"sequential-writer", make_filler<SequentialWriterFiller>},
};

// ---------------------------------------------------------------------------
// Emulated accelerator

// The completion record of an offload. As in the IAA and DSA completion
// records, its first byte is the status the device writes when it is done.
struct alignas(64) CompletionRecord {
  volatile uint8_t status;
  uint64_t completion_tsc;
};

// A thread on its own CPU that plays the accelerator. It takes one offload at
// a time, spins until the completion time of the offload, then writes the
// completion time and the status byte of the completion record.
class EmulatedAccelerator {
 public:
  explicit EmulatedAccelerator(int cpu) : device_thread([this, cpu] { serve(cpu); }) {}
  ~EmulatedAccelerator() {
    stop_requested.store(true, std::memory_order_release);
    device_thread.join();
  }
  void submit(CompletionRecord *record, uint64_t completion_tsc) {
    pending_completion_tsc.store(completion_tsc, std::memory_order_relaxed);
    pending_record.store(record, std::memory_order_release);
  }
  void wait_until_idle() const {
    while (pending_record.load(std::memory_order_acquire) != nullptr) {
      _mm_pause();
    }
  }

 private:
  void serve(int cpu) {
    pin_current_thread_to_cpu(cpu);
    while (!stop_requested.load(std::memory_order_acquire)) {
      CompletionRecord *record = pending_record.load(std::memory_order_acquire);
      if (record == nullptr) {
        _mm_pause();
        continue;
      }
      uint64_t completion_tsc = pending_completion_tsc.load(std::memory_order_relaxed);
      while (read_tsc() < completion_tsc) {
        _mm_pause();
      }
      record->completion_tsc = read_tsc();
      __atomic_store_n(&record->status, 1, __ATOMIC_RELEASE);
      pending_record.store(nullptr, std::memory_order_release);
    }
  }

  std::atomic<CompletionRecord *> pending_record{nullptr};
  std::atomic<uint64_t> pending_completion_tsc{0};
  std::atomic<bool> stop_requested{false};
  std::thread device_thread;  // Last, so that it starts after the atomics exist.
};

// ---------------------------------------------------------------------------
// Filler fiber

static void filler_fiber_entry(fcontext_transfer_t transfer) {
  scheduler_transfer = transfer;
  Filler *filler = static_cast<Filler *>(transfer.data);
  filler->run();
  scheduler_return_tsc = read_tsc();
  filler_finished = true;
  fcontext_swap(scheduler_transfer.prev_context, nullptr);
  // The scheduler never swaps back to a finished filler.
}

// One fiber stack, used again for each run.
class FillerFiber {
 public:
  FillerFiber() : state(fcontext_create(filler_fiber_entry)) {}
  ~FillerFiber() { fcontext_destroy(state); }
  FillerFiber(const FillerFiber &) = delete;
  FillerFiber &operator=(const FillerFiber &) = delete;
  // A new context at the top of the stack that enters filler_fiber_entry.
  fcontext_t new_context() {
    return make_fcontext(state->stack + FCONTEXT_STACK_SIZE, FCONTEXT_STACK_SIZE,
                         filler_fiber_entry);
  }

 private:
  fcontext_state_t *state;
};

enum class RunOutcome {
  yielded,                     // a probe point saw the completion and yielded
  finished_after_completion,   // no probe point saw the completion in time
  finished_before_completion,  // the offload was still in flight at the end
};

static const char *outcome_name(RunOutcome outcome) {
  switch (outcome) {
    case RunOutcome::yielded:
      return "yielded";
    case RunOutcome::finished_after_completion:
      return "finished_after_completion";
    case RunOutcome::finished_before_completion:
      return "finished_before_completion";
  }
  return "unknown";
}

struct PreemptedRun {
  RunOutcome outcome;
  // From the completion to the scheduler getting the core back. Negative for
  // finished_before_completion.
  int64_t latency_ticks;
  // From the completion time asked for to the completion. Large when the OS
  // takes the accelerator CPU away: the run then does not show the probes.
  int64_t accelerator_lateness_ticks;
  bool output_correct;
};

// Runs the filler in the fiber while an offload is in flight. After a yield,
// resumes the filler to its end, then checks its output.
static PreemptedRun run_filler_with_offload(Filler &filler, FillerFiber &fiber,
                                            EmulatedAccelerator &accelerator,
                                            CompletionRecord &record, uint64_t offload_ticks,
                                            uint64_t reference_checksum) {
  filler.reset();
  record.status = 0;
  record.completion_tsc = 0;
  filler_finished = false;
  inside_filler_fiber = true;
  preempt_signal = &record.status;
  uint64_t requested_completion_tsc = read_tsc() + offload_ticks;
  accelerator.submit(&record, requested_completion_tsc);

  fcontext_transfer_t from_filler = fcontext_swap(fiber.new_context(), &filler);
  preempt_signal = &signal_byte_never_set;
  uint64_t return_tsc = scheduler_return_tsc;
  bool yielded = !filler_finished;
  if (yielded) {
    fcontext_swap(from_filler.prev_context, nullptr);
  }
  inside_filler_fiber = false;
  accelerator.wait_until_idle();

  PreemptedRun run;
  run.latency_ticks = static_cast<int64_t>(return_tsc - record.completion_tsc);
  run.accelerator_lateness_ticks =
      static_cast<int64_t>(record.completion_tsc - requested_completion_tsc);
  if (yielded) {
    run.outcome = RunOutcome::yielded;
  } else if (run.latency_ticks >= 0) {
    run.outcome = RunOutcome::finished_after_completion;
  } else {
    run.outcome = RunOutcome::finished_before_completion;
  }
  run.output_correct = filler.checksum() == reference_checksum;
  return run;
}

// ---------------------------------------------------------------------------
// Measurements

struct Options {
  std::string mode;
  std::vector<std::string> filler_names;
  size_t size_bytes = 65536;
  int runs = 0;
  int cpu = 2;
  int accelerator_cpu = 4;
  double offload_ns = 0;  // 0: uniform in [0, 0.9 x median filler runtime]
  uint64_t seed = 1;
  std::string samples_path;
  std::string ipp_code_path = "native";
  bool print_header = true;
};

constexpr int warmup_runs = 5;
// A completion this much later than asked for counts as late: the OS took the
// accelerator CPU away. Use idle CPUs when the late count is not near 0.
constexpr double late_completion_ns = 1000;

// Runs the filler warmup_runs times on the calling thread, with no offload.
// Returns the checksum of the last run: the reference output.
static uint64_t warm_up(Filler &filler) {
  for (int run = 0; run < warmup_runs; run++) {
    filler.reset();
    filler.run();
  }
  return filler.checksum();
}

// Runtimes in ns of runs with no offload in flight.
static std::vector<double> time_runs(Filler &filler, int runs, uint64_t reference_checksum,
                                     const TscClock &clock, int &wrong_outputs) {
  std::vector<double> runtimes_ns;
  for (int run = 0; run < runs; run++) {
    filler.reset();
    uint64_t start_tsc = read_tsc();
    filler.run();
    uint64_t end_tsc = read_tsc();
    runtimes_ns.push_back(clock.ticks_to_ns(end_tsc - start_tsc));
    if (filler.checksum() != reference_checksum) {
      wrong_outputs++;
    }
  }
  return runtimes_ns;
}

static void measure_overhead(const char *filler_name, Filler &filler, const Options &options,
                             const TscClock &clock) {
  uint64_t reference_checksum = warm_up(filler);
  int wrong_outputs = 0;
  std::vector<double> runtimes_ns =
      time_runs(filler, options.runs, reference_checksum, clock, wrong_outputs);
  printf("overhead,%s,%s,%s,%zu,%d,%d,%.0f,%.0f,%.0f,%016" PRIx64 "\n", RACER_PROBE_INTERVAL,
         options.ipp_code_path.c_str(), filler_name, options.size_bytes, options.runs,
         wrong_outputs, percentile(runtimes_ns, 0.5), percentile(runtimes_ns, 0.1),
         percentile(runtimes_ns, 0.9), reference_checksum);
}

static void measure_latency(const char *filler_name, Filler &filler, const Options &options,
                            const TscClock &clock, EmulatedAccelerator &accelerator,
                            std::mt19937_64 &offload_random, FILE *samples) {
  uint64_t reference_checksum = warm_up(filler);
  int wrong_outputs = 0;
  double filler_median_ns =
      percentile(time_runs(filler, 20, reference_checksum, clock, wrong_outputs), 0.5);
  std::uniform_real_distribution<double> uniform_offload_ns(0, 0.9 * filler_median_ns);

  FillerFiber fiber;
  CompletionRecord record = {};
  int run_counts[3] = {};
  std::vector<double> latencies_ns;
  int late_completions = 0;
  for (int run_index = 0; run_index < options.runs; run_index++) {
    double offload_ns =
        options.offload_ns > 0 ? options.offload_ns : uniform_offload_ns(offload_random);
    PreemptedRun run = run_filler_with_offload(filler, fiber, accelerator, record,
                                               clock.ns_to_ticks(offload_ns), reference_checksum);
    run_counts[static_cast<int>(run.outcome)]++;
    if (!run.output_correct) {
      wrong_outputs++;
    }
    double latency_ns = clock.ticks_to_ns(static_cast<double>(run.latency_ticks));
    double lateness_ns = clock.ticks_to_ns(static_cast<double>(run.accelerator_lateness_ticks));
    if (lateness_ns > late_completion_ns) {
      late_completions++;
    }
    if (run.outcome != RunOutcome::finished_before_completion) {
      latencies_ns.push_back(latency_ns);
    }
    if (samples != nullptr) {
      fprintf(samples, "%s,%s,%s,%zu,%d,%.0f,%.0f,%s,%.0f,%d\n", RACER_PROBE_INTERVAL,
              options.ipp_code_path.c_str(), filler_name, options.size_bytes, run_index, offload_ns,
              lateness_ns, outcome_name(run.outcome), latency_ns, run.output_correct ? 1 : 0);
    }
  }
  printf("latency,%s,%s,%s,%zu,%d,%d,%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%.0f\n", RACER_PROBE_INTERVAL,
         options.ipp_code_path.c_str(), filler_name, options.size_bytes, options.runs,
         run_counts[static_cast<int>(RunOutcome::yielded)],
         run_counts[static_cast<int>(RunOutcome::finished_after_completion)],
         run_counts[static_cast<int>(RunOutcome::finished_before_completion)], late_completions,
         wrong_outputs, percentile(latencies_ns, 0.5), percentile(latencies_ns, 0.9),
         percentile(latencies_ns, 0.99),
         latencies_ns.empty() ? NAN : *std::max_element(latencies_ns.begin(), latencies_ns.end()),
         filler_median_ns);
}

// ---------------------------------------------------------------------------
// Command line

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: preempt_bench --mode overhead|latency [options]\n"
          "  --filler NAME        a filler, or all (default); repeat for more than one\n"
          "  --size BYTES         input size of each filler, a multiple of 64 (default 65536)\n"
          "  --runs N             measured runs (default 200 overhead, 1000 latency)\n"
          "  --cpu N              CPU of the filler (default 2)\n"
          "  --accelerator-cpu N  CPU of the emulated accelerator (default 4)\n"
          "  --offload-ns N       offload time; default uniform in [0, 0.9 x filler runtime]\n"
          "  --seed N             seed of the inputs and the offload times of each filler\n"
          "                       (default 1)\n"
          "  --samples PATH       latency mode: also write each run to PATH\n"
          "  --ipp-code-path P    GCM code of ipp-crypto: native (default; VAES assembly on\n"
          "                       AVX-512 CPUs), aesni (AES-NI assembly), or portable-c\n"
          "                       (table C code, which has probe points in its block loop)\n"
          "  --no-header          do not print the CSV header\n"
          "fillers:");
  for (const FillerKind &kind : filler_kinds) {
    fprintf(stream, " %s", kind.name);
  }
  fprintf(stream, "\n");
}

static const FillerKind *find_filler_kind(const std::string &name) {
  for (const FillerKind &kind : filler_kinds) {
    if (name == kind.name) {
      return &kind;
    }
  }
  return nullptr;
}

static Options parse_options(int argc, char **argv) {
  enum {
    mode_option = 1000,
    filler_option,
    size_option,
    runs_option,
    cpu_option,
    accelerator_cpu_option,
    offload_ns_option,
    seed_option,
    samples_option,
    ipp_code_path_option,
    no_header_option,
    help_option
  };
  static const option long_options[] = {
      {"mode", required_argument, nullptr, mode_option},
      {"filler", required_argument, nullptr, filler_option},
      {"size", required_argument, nullptr, size_option},
      {"runs", required_argument, nullptr, runs_option},
      {"cpu", required_argument, nullptr, cpu_option},
      {"accelerator-cpu", required_argument, nullptr, accelerator_cpu_option},
      {"offload-ns", required_argument, nullptr, offload_ns_option},
      {"seed", required_argument, nullptr, seed_option},
      {"samples", required_argument, nullptr, samples_option},
      {"ipp-code-path", required_argument, nullptr, ipp_code_path_option},
      {"no-header", no_argument, nullptr, no_header_option},
      {"help", no_argument, nullptr, help_option},
      {nullptr, 0, nullptr, 0}};
  Options options;
  int option_code;
  while ((option_code = getopt_long(argc, argv, "", long_options, nullptr)) != -1) {
    switch (option_code) {
      case mode_option:
        options.mode = optarg;
        break;
      case filler_option:
        options.filler_names.push_back(optarg);
        break;
      case size_option:
        options.size_bytes = strtoull(optarg, nullptr, 0);
        break;
      case runs_option:
        options.runs = atoi(optarg);
        break;
      case cpu_option:
        options.cpu = atoi(optarg);
        break;
      case accelerator_cpu_option:
        options.accelerator_cpu = atoi(optarg);
        break;
      case offload_ns_option:
        options.offload_ns = atof(optarg);
        break;
      case seed_option:
        options.seed = strtoull(optarg, nullptr, 0);
        break;
      case samples_option:
        options.samples_path = optarg;
        break;
      case ipp_code_path_option:
        options.ipp_code_path = optarg;
        break;
      case no_header_option:
        options.print_header = false;
        break;
      case help_option:
        print_usage(stdout);
        exit(0);
      default:
        print_usage(stderr);
        exit(2);
    }
  }
  if (options.mode != "overhead" && options.mode != "latency") {
    print_usage(stderr);
    exit(2);
  }
  if (options.size_bytes == 0 || options.size_bytes % 64 != 0 || options.size_bytes > (1u << 30)) {
    fprintf(stderr, "--size must be a nonzero multiple of 64, at most 1 GiB\n");
    exit(2);
  }
  if (options.ipp_code_path != "native" && options.ipp_code_path != "aesni" &&
      options.ipp_code_path != "portable-c") {
    fprintf(stderr, "--ipp-code-path must be native, aesni or portable-c\n");
    exit(2);
  }
  if (options.runs <= 0) {
    options.runs = options.mode == "overhead" ? 200 : 1000;
  }
  if (options.filler_names.empty() ||
      std::find(options.filler_names.begin(), options.filler_names.end(), "all") !=
          options.filler_names.end()) {
    options.filler_names.clear();
    for (const FillerKind &kind : filler_kinds) {
      options.filler_names.push_back(kind.name);
    }
  }
  for (const std::string &name : options.filler_names) {
    if (find_filler_kind(name) == nullptr) {
      fprintf(stderr, "unknown filler '%s'\n", name.c_str());
      print_usage(stderr);
      exit(2);
    }
  }
  return options;
}

// The merged ipp-crypto library picks a code path for each call from the CPU
// features it has, and ippsAES_GCMInit picks the GCM functions of the state.
// Without the AVX-512 and VAES features, GCM is the AES-NI assembly; without
// AES-NI and CLMUL too, it is table C code, compiled through the plugin.
// Call before a GCM state is made.
static void select_ipp_code_path(const std::string &ipp_code_path) {
  if (ipp_code_path == "native") {
    return;
  }
  Ipp64u cpu_features = 0;
  if (ippcpGetCpuFeatures(&cpu_features) != ippStsNoErr) {
    fprintf(stderr, "ippcpGetCpuFeatures failed\n");
    exit(1);
  }
  cpu_features &= ~(ippCPUID_AVX512F | ippCPUID_AVX512VAES | ippCPUID_AVX512VCLMUL |
                    ippCPUID_AVX2VAES | ippCPUID_AVX2VCLMUL);
  if (ipp_code_path == "portable-c") {
    cpu_features &= ~(ippCPUID_AES | ippCPUID_CLMUL);
  }
  if (ippcpSetCpuFeatures(cpu_features) != ippStsNoErr) {
    fprintf(stderr, "ippcpSetCpuFeatures failed\n");
    exit(1);
  }
}

int main(int argc, char **argv) {
  Options options = parse_options(argc, argv);
  select_ipp_code_path(options.ipp_code_path);
  pin_current_thread_to_cpu(options.cpu);
  TscClock clock = TscClock::calibrate();

  if (options.mode == "overhead") {
    if (options.print_header) {
      printf(
          "mode,probe_interval,ipp_code_path,filler,size_bytes,runs,wrong_outputs,"
          "median_ns,p10_ns,p90_ns,checksum\n");
    }
    for (const std::string &name : options.filler_names) {
      std::mt19937_64 input_random(options.seed);
      std::unique_ptr<Filler> filler =
          find_filler_kind(name)->make(options.size_bytes, input_random);
      measure_overhead(name.c_str(), *filler, options, clock);
      fflush(stdout);
    }
    return 0;
  }

  FILE *samples = nullptr;
  if (!options.samples_path.empty()) {
    samples = fopen(options.samples_path.c_str(), "w");
    if (samples == nullptr) {
      fprintf(stderr, "cannot write %s: %s\n", options.samples_path.c_str(), strerror(errno));
      return 1;
    }
    fprintf(samples,
            "probe_interval,ipp_code_path,filler,size_bytes,run,offload_ns,accelerator_lateness_ns,"
            "outcome,latency_ns,"
            "output_correct\n");
  }
  if (options.print_header) {
    printf(
        "mode,probe_interval,ipp_code_path,filler,size_bytes,runs,yielded,finished_after_"
        "completion,"
        "finished_before_completion,late_completions,wrong_outputs,latency_median_ns,latency_p90_"
        "ns,"
        "latency_p99_ns,latency_max_ns,filler_median_ns\n");
  }
  EmulatedAccelerator accelerator(options.accelerator_cpu);
  for (const std::string &name : options.filler_names) {
    std::mt19937_64 input_random(options.seed);
    std::unique_ptr<Filler> filler = find_filler_kind(name)->make(options.size_bytes, input_random);
    std::mt19937_64 offload_random(options.seed);
    measure_latency(name.c_str(), *filler, options, clock, accelerator, offload_random, samples);
    fflush(stdout);
  }
  if (samples != nullptr) {
    fclose(samples);
  }
  return 0;
}
