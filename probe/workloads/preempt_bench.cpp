// preempt_bench: probe-point accelerator preemption with an emulated
// accelerator, with an Intel QAT device and with an Intel DSA device.
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
//   overhead      runs each filler with no offload in flight and reports its
//                 runtime, and counts the probe points one run executes.
//                 Compare a build with probes to the off build.
//   latency       submits an offload to the emulated accelerator, runs the
//                 filler in a fiber, and measures the time from the
//                 accelerator completion to the scheduler running again, in
//                 two parts: detect (to the probe point that sees the
//                 completion, or the end of the filler) and switch (from the
//                 filler fiber to the scheduler). Then it resumes the filler to
//                 its end and checks its output.
//   signal-floor  measures the least detect time: the filler CPU loads the
//                 status byte in a tight loop, with no filler and no fiber.
//   qat-latency   latency mode with a QAT device in place of the emulated
//                 accelerator: the offload is an AES-CBC decrypt through the
//                 kernel crypto API, and the completion byte is in the
//                 io_event that the kernel writes when the device interrupt
//                 completes the request. An observer thread on its own CPU
//                 gives the completion time.
//   qat-floor     the least detect time of qat-latency: the filler CPU loads
//                 the completion byte in a tight loop. Also the offload time.
//   qat-interrupt the kernel time of the QAT completion, with the filler on
//                 the CPU of the QAT interrupt: the filler CPU loads the
//                 completion byte and reads the TSC in a tight loop, and a
//                 long step between two TSC reads is the interrupt.
//   dsa-latency   qat-latency with a DSA device: the offload is a memory move
//                 through a user work queue, and the completion byte is the
//                 status byte of the completion record that the device
//                 writes. No kernel code is on the completion path.
//   dsa-floor     qat-floor with the DSA device of dsa-latency.
//   switch-floor  the least switch time: a fiber with no filler calls do_yield
//                 in a loop, and the scheduler swaps back to it at once.
//
// The switch parts (--switch-parts) and the yield jump (--yield-jump) find the
// parts of the switch: see the comment above yield_with_switch_experiment.
#include <fcntl.h>
#include <getopt.h>
#include <linux/aio_abi.h>
#include <linux/idxd.h>
#include <linux/if_alg.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
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
// preempt_signal points here while count_probe_executions runs a filler, so
// that each probe point the filler executes calls do_yield.
static uint8_t signal_byte_always_set = 1;
__thread volatile uint8_t *preempt_signal = &signal_byte_never_set;

static thread_local fcontext_transfer_t scheduler_transfer;
static thread_local bool inside_filler_fiber = false;
static thread_local bool filler_finished = false;
// The time the filler last stopped: at the entry of do_yield, or at the end of
// the filler.
static thread_local uint64_t filler_stopped_tsc = 0;
// While true, do_yield counts the call in probe_executions and returns with no
// switch.
static thread_local bool counting_probe_executions = false;
static thread_local uint64_t probe_executions = 0;

static inline uint64_t read_tsc() {
  _mm_lfence();
  uint64_t tsc = __rdtsc();
  _mm_lfence();
  return tsc;
}

// The variants of jump_fcontext in jump_fcontext_variants.S.
extern "C" fcontext_transfer_t jump_fcontext_yield_copy(fcontext_t const to, void *vp);
extern "C" fcontext_transfer_t jump_fcontext_no_fpu_control_load(fcontext_t const to, void *vp);

// The function that do_yield jumps to the scheduler with.
enum class YieldJump {
  boost,                // jump_fcontext, as the scheduler uses
  own_copy,             // jump_fcontext_yield_copy
  no_fpu_control_load,  // jump_fcontext_no_fpu_control_load
};

static const char *yield_jump_name(YieldJump yield_jump) {
  switch (yield_jump) {
    case YieldJump::boost:
      return "boost";
    case YieldJump::own_copy:
      return "own-copy";
    case YieldJump::no_fpu_control_load:
      return "no-fpu-control-load";
  }
  return "unknown";
}

// The TSC stamps that do_yield takes between its entry stamp (the filler stop)
// and the jump, when stamping_switch_parts is true. The scheduler takes the
// last stamp when the jump returns into it.
struct SwitchPartStamps {
  uint64_t checks_end_tsc;      // after the flag checks and the call
  uint64_t second_stamp_tsc;    // at once after checks_end_tsc: the time of a stamp
  uint64_t stores_drained_tsc;  // after an mfence: the stores of the filler are in the cache
  uint64_t context_loaded_tsc;  // after loads of the two cache lines of the scheduler context
};

// Set once in main, before the first filler. While switch_experiment_active
// is false, do_yield jumps with jump_fcontext and takes no stamps.
static thread_local bool switch_experiment_active = false;
static thread_local bool stamping_switch_parts = false;
static thread_local YieldJump yield_jump = YieldJump::boost;
static thread_local SwitchPartStamps switch_part_stamps;

static fcontext_transfer_t jump_to_scheduler(fcontext_t scheduler_context) {
  switch (yield_jump) {
    case YieldJump::boost:
      return jump_fcontext(scheduler_context, nullptr);
    case YieldJump::own_copy:
      return jump_fcontext_yield_copy(scheduler_context, nullptr);
    case YieldJump::no_fpu_control_load:
      return jump_fcontext_no_fpu_control_load(scheduler_context, nullptr);
  }
  abort();
}

// The yield of do_yield when switch_experiment_active is true. The switch is
// from the entry stamp of do_yield to the stamp of the scheduler after the
// jump. With stamping_switch_parts, four more stamps cut it into parts, in
// this order:
//   checks        the flag checks of do_yield and the call of this function
//   store drain   an mfence: the time for the stores of the filler that are
//                 still in the store buffer to reach the cache. Without the
//                 mfence, the switch waits for them only when the store
//                 buffer has no free entry for the 9 stores of the call and
//                 the jump (the return address and the saved context).
//   context load  loads of the first and the last byte of the 64-byte saved
//                 context of the scheduler, which can be on two cache lines.
//                 The jump loads the same lines.
//   jump          the selected jump function and the code of the scheduler
//                 from the return of fcontext_swap to its stamp. With the
//                 store buffer empty and the context in the cache, this is
//                 the instructions and the indirect jump.
// A stamp is lfence, rdtsc, lfence and a store. Each part has the time of one
// stamp in it; the stamp part (second_stamp_tsc - checks_end_tsc) gives it.
// The switch without the stamps has one stamp in it, as the switch of a run
// without --switch-parts does.
static __attribute__((noinline)) void yield_with_switch_experiment(uint64_t yield_tsc) {
  filler_stopped_tsc = yield_tsc;
  if (stamping_switch_parts) {
    switch_part_stamps.checks_end_tsc = read_tsc();
    switch_part_stamps.second_stamp_tsc = read_tsc();
    _mm_mfence();
    switch_part_stamps.stores_drained_tsc = read_tsc();
    const volatile uint8_t *scheduler_context =
        reinterpret_cast<const volatile uint8_t *>(scheduler_transfer.prev_context);
    (void)scheduler_context[0];
    (void)scheduler_context[63];
    // The lfence of read_tsc waits for the two loads.
    switch_part_stamps.context_loaded_tsc = read_tsc();
  }
  scheduler_transfer = jump_to_scheduler(scheduler_transfer.prev_context);
}

extern "C" void do_yield(void) {
  uint64_t yield_tsc = read_tsc();
  if (counting_probe_executions) {
    probe_executions++;
    return;
  }
  if (!inside_filler_fiber) {
    fprintf(stderr, "do_yield outside a filler fiber: preempt_signal points at a set byte\n");
    abort();
  }
  if (switch_experiment_active) {
    yield_with_switch_experiment(yield_tsc);
    return;
  }
  filler_stopped_tsc = yield_tsc;
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

// The serialize step of RACER's gen_ser_buf (src/baseline.cpp): the size and
// the bytes of a router request with RACER's key and a value of size_bytes.
// RACER serializes only in its setup code, so there no probe point yields; this
// filler measures the probes that the serializer would have if it yielded. The
// probe points are in the protobuf serializer and in router.pb.cc; the copy of
// the value is glibc memcpy, which has none.
class SerializeFiller : public Filler {
 public:
  SerializeFiller(size_t size_bytes, std::mt19937_64 &random) {
    request.set_key("/region/cluster/foo:key|#|etc");
    std::vector<uint8_t> value = random_bytes(size_bytes, random);
    request.set_value(value.data(), value.size());
    request.set_operation(1);
    serialized_request.resize(request.ByteSizeLong());
  }
  void run() override {
    size_t request_bytes = request.ByteSizeLong();
    if (request_bytes != serialized_request.size() ||
        !request.SerializeToArray(serialized_request.data(), static_cast<int>(request_bytes))) {
      fprintf(stderr, "serialize: the router request does not serialize\n");
      abort();
    }
  }
  uint64_t checksum() const override {
    return fnv1a_64(serialized_request.data(), serialized_request.size());
  }

 private:
  router::RouterRequest request;
  std::vector<uint8_t> serialized_request;
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
    {"serialize", make_filler<SerializeFiller>},
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
struct CompletionRecord {
  alignas(64) volatile uint8_t status;
  // The time of the completion, in its own cache line: a device writes its
  // record in one write, so the status write must be the only write to the
  // line that the probe points load.
  alignas(64) uint64_t completion_tsc;
};

// The completion records that the runs take in turn. The time for the status
// write to reach the filler CPU depends on the L3 slice that is home to the
// cache line, and the physical address selects that slice. With one record,
// the latency of one process differs from that of the next by up to 50 ns
// (emerald-02). Records on many pages put the runs of a process on many
// slices.
class CompletionRecordPool {
 public:
  CompletionRecord &next() {
    CompletionRecord &record = records[next_index];
    next_index = (next_index + 1) % records.size();
    return record;
  }

 private:
  static constexpr size_t record_count = 1024;  // 128 KiB, 32 pages
  std::vector<CompletionRecord> records = std::vector<CompletionRecord>(record_count);
  size_t next_index = 0;
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
  void submit(CompletionRecord *record, uint64_t requested_completion_tsc) {
    pending_requested_completion_tsc.store(requested_completion_tsc, std::memory_order_relaxed);
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
      uint64_t requested_completion_tsc =
          pending_requested_completion_tsc.load(std::memory_order_relaxed);
      while (read_tsc() < requested_completion_tsc) {
        _mm_pause();
      }
      uint64_t completion_tsc = read_tsc();
      __atomic_store_n(&record->status, 1, __ATOMIC_RELEASE);
      record->completion_tsc = completion_tsc;
      // The scheduler reads completion_tsc after wait_until_idle sees this.
      pending_record.store(nullptr, std::memory_order_release);
    }
  }

  std::atomic<CompletionRecord *> pending_record{nullptr};
  std::atomic<uint64_t> pending_requested_completion_tsc{0};
  std::atomic<bool> stop_requested{false};
  std::thread device_thread;  // Last, so that it starts after the atomics exist.
};

// ---------------------------------------------------------------------------
// QAT accelerator
//
// An Intel QAT device (4xxx) decrypts through the Linux kernel crypto API:
// AF_ALG sockets and Linux AIO. This needs no root and no device setup. When
// the device is done, its interrupt handler completes the AIO request: the
// kernel writes an io_event into the AIO ring, which io_setup maps into user
// memory. The first byte of that io_event is the completion byte that the
// probe points load.

#ifndef SOL_ALG
#define SOL_ALG 279
#endif

static void exit_with_system_error(const char *what) {
  fprintf(stderr, "%s: %s\n", what, strerror(errno));
  exit(1);
}

constexpr size_t aes_block_bytes = 16;

// One AF_ALG skcipher transform with one key, bound to a kernel crypto driver
// by its driver name.
class AfAlgSkcipher {
 public:
  AfAlgSkcipher(const char *driver_name, const uint8_t *key, size_t key_bytes) {
    transform_fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
    if (transform_fd < 0) {
      exit_with_system_error("socket(AF_ALG)");
    }
    sockaddr_alg address = {};
    address.salg_family = AF_ALG;
    snprintf(reinterpret_cast<char *>(address.salg_type), sizeof address.salg_type, "skcipher");
    snprintf(reinterpret_cast<char *>(address.salg_name), sizeof address.salg_name, "%s",
             driver_name);
    if (bind(transform_fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
      fprintf(stderr, "cannot bind the kernel crypto driver %s: %s\n", driver_name,
              strerror(errno));
      exit(1);
    }
    if (setsockopt(transform_fd, SOL_ALG, ALG_SET_KEY, key, key_bytes) != 0) {
      exit_with_system_error("setsockopt(ALG_SET_KEY)");
    }
    operation_fd = accept(transform_fd, nullptr, nullptr);
    if (operation_fd < 0) {
      exit_with_system_error("accept(AF_ALG)");
    }
  }
  ~AfAlgSkcipher() {
    close(operation_fd);
    close(transform_fd);
  }
  AfAlgSkcipher(const AfAlgSkcipher &) = delete;
  AfAlgSkcipher &operator=(const AfAlgSkcipher &) = delete;

  // Gives the kernel the input of one operation (ALG_OP_ENCRYPT or
  // ALG_OP_DECRYPT) and its 16-byte IV. The kernel copies the input; a read of
  // the operation socket starts the operation.
  void send_request(uint32_t operation, const uint8_t *iv, const uint8_t *input, size_t length) {
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof operation) +
                                  CMSG_SPACE(sizeof(af_alg_iv) + aes_block_bytes)] = {};
    iovec input_vector = {const_cast<uint8_t *>(input), length};
    msghdr message = {};
    message.msg_iov = &input_vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    cmsghdr *operation_header = CMSG_FIRSTHDR(&message);
    operation_header->cmsg_level = SOL_ALG;
    operation_header->cmsg_type = ALG_SET_OP;
    operation_header->cmsg_len = CMSG_LEN(sizeof operation);
    memcpy(CMSG_DATA(operation_header), &operation, sizeof operation);
    cmsghdr *iv_header = CMSG_NXTHDR(&message, operation_header);
    iv_header->cmsg_level = SOL_ALG;
    iv_header->cmsg_type = ALG_SET_IV;
    iv_header->cmsg_len = CMSG_LEN(sizeof(af_alg_iv) + aes_block_bytes);
    af_alg_iv *iv_data = reinterpret_cast<af_alg_iv *>(CMSG_DATA(iv_header));
    iv_data->ivlen = aes_block_bytes;
    memcpy(iv_data->iv, iv, aes_block_bytes);
    ssize_t sent_bytes = sendmsg(operation_fd, &message, 0);
    if (sent_bytes < 0) {
      exit_with_system_error("sendmsg(AF_ALG)");
    }
    if (static_cast<size_t>(sent_bytes) != length) {
      fprintf(stderr, "sendmsg(AF_ALG) took %zd of %zu bytes\n", sent_bytes, length);
      exit(1);
    }
  }
  int operation_socket() const { return operation_fd; }

 private:
  int transform_fd;
  int operation_fd;
};

// The header of the AIO ring that io_setup maps into user memory (struct
// aio_ring of the kernel fs/aio.c, which is not in the uapi headers). The
// io_events follow the header. The kernel writes the event of each completion
// at events()[tail], then moves tail on, and wraps at event_count.
struct KernelAioRing {
  unsigned id;
  unsigned event_count;
  unsigned head;
  unsigned tail;
  unsigned magic;
  unsigned compat_features;
  unsigned incompat_features;
  unsigned header_length;
  io_event *events() { return reinterpret_cast<io_event *>(this + 1); }
};
constexpr unsigned kernel_aio_ring_magic = 0xa10a10a1;

// One AES-128-CBC decrypt at a time on a QAT device. The kernel binds the QAT
// driver by its driver name, qat_aes_cbc: the AES-NI driver of cbc(aes) has a
// higher priority. The ciphertext is made by ipp-crypto, so each output is
// checked against code that is not the kernel's or the device's.
class QatDecryptOffload {
 public:
  // The /proc/interrupts lines of the device hold this name.
  static constexpr const char *interrupt_name = "qat";

  QatDecryptOffload(size_t offload_bytes, std::mt19937_64 &random)
      : offload_bytes(offload_bytes),
        plaintext(random_bytes(offload_bytes, random)),
        ciphertext(encrypt_with_ipp_crypto(plaintext)),
        output(offload_bytes),
        qat_cipher("qat_aes_cbc", key, key_bytes) {
    if (syscall(SYS_io_setup, 1, &aio_context) != 0) {
      exit_with_system_error("io_setup");
    }
    ring = reinterpret_cast<KernelAioRing *>(aio_context);
    if (ring->magic != kernel_aio_ring_magic || ring->header_length != sizeof(KernelAioRing)) {
      fprintf(stderr, "the AIO ring of this kernel does not have the layout of fs/aio.c\n");
      exit(1);
    }
    // One offload checks that the kernel writes the io_event where prepare()
    // expects it.
    volatile uint8_t *completion_byte = prepare();
    submit();
    bool output_correct = reap();
    if (*completion_byte != 1) {
      fprintf(stderr, "the kernel wrote the io_event to another slot of the AIO ring\n");
      exit(1);
    }
    if (!output_correct) {
      fprintf(stderr, "the QAT output is not the plaintext\n");
      exit(1);
    }
  }
  ~QatDecryptOffload() { syscall(SYS_io_destroy, aio_context); }
  QatDecryptOffload(const QatDecryptOffload &) = delete;
  QatDecryptOffload &operator=(const QatDecryptOffload &) = delete;

  // Clears the output and the completion byte of the io_event that the next
  // completion fills, and returns the completion byte. The completion sets it
  // to 1 (aio_data).
  volatile uint8_t *prepare() {
    std::fill(output.begin(), output.end(), 0);
    io_event &next_event = ring->events()[ring->tail];
    next_event.data = 0;
    return reinterpret_cast<volatile uint8_t *>(&next_event.data);
  }
  // sendmsg of the ciphertext, then io_submit of a read of the plaintext. The
  // read starts the decrypt on the device and returns at once.
  void submit() {
    qat_cipher.send_request(ALG_OP_DECRYPT, iv, ciphertext.data(), offload_bytes);
    iocb read_request = {};
    read_request.aio_data = 1;
    read_request.aio_lio_opcode = IOCB_CMD_PREAD;
    read_request.aio_fildes = static_cast<uint32_t>(qat_cipher.operation_socket());
    read_request.aio_buf = reinterpret_cast<uint64_t>(output.data());
    read_request.aio_nbytes = offload_bytes;
    iocb *read_requests[] = {&read_request};
    if (syscall(SYS_io_submit, aio_context, 1, read_requests) != 1) {
      exit_with_system_error("io_submit");
    }
  }
  // Takes the io_event of the offload (waits at most 1 s) and returns true when
  // the read is whole and the output is the plaintext.
  bool reap() {
    io_event event = {};
    timespec timeout = {1, 0};
    long event_count = syscall(SYS_io_getevents, aio_context, 1, 1, &event, &timeout);
    if (event_count < 0) {
      exit_with_system_error("io_getevents");
    }
    if (event_count == 0) {
      fprintf(stderr, "the QAT offload did not complete in 1 s\n");
      exit(1);
    }
    return event.res == static_cast<int64_t>(offload_bytes) && output == plaintext;
  }

 private:
  static constexpr uint8_t key[] = "0123456789abcdef";
  static constexpr size_t key_bytes = 16;
  static constexpr uint8_t iv[] = "fedcba9876543210";

  static std::vector<uint8_t> encrypt_with_ipp_crypto(const std::vector<uint8_t> &plaintext) {
    int spec_size = 0;
    check(ippsAESGetSize(&spec_size), "ippsAESGetSize");
    std::vector<Ipp8u> spec_memory(spec_size);
    IppsAESSpec *spec = reinterpret_cast<IppsAESSpec *>(spec_memory.data());
    check(ippsAESInit(key, key_bytes, spec, spec_size), "ippsAESInit");
    std::vector<uint8_t> ciphertext(plaintext.size());
    check(ippsAESEncryptCBC(plaintext.data(), ciphertext.data(), static_cast<int>(plaintext.size()),
                            spec, iv),
          "ippsAESEncryptCBC");
    return ciphertext;
  }
  static void check(IppStatus status, const char *function) {
    if (status != ippStsNoErr) {
      fprintf(stderr, "QAT offload: %s returned %d\n", function, status);
      exit(1);
    }
  }

  size_t offload_bytes;
  std::vector<uint8_t> plaintext;
  std::vector<uint8_t> ciphertext;
  std::vector<uint8_t> output;
  AfAlgSkcipher qat_cipher;
  aio_context_t aio_context = 0;
  KernelAioRing *ring = nullptr;
};

// ---------------------------------------------------------------------------
// DSA accelerator
//
// An Intel DSA device moves memory through a user work queue (WQ). The program
// gives a 64-byte descriptor to the WQ of /dev/dsa/wqD.Q, and the device writes
// a 32-byte completion record when it is done. The device works on the virtual
// addresses of the program (shared virtual memory through the PASID of the
// process). No kernel code and no interrupt are on the completion path: the
// status byte of the completion record is the completion byte that the probe
// points load. An administrator makes and enables the WQ with accel-config and
// gives the users access to its device file.
//
// The program writes the descriptor to the mapped WQ portal with one
// instruction. On a device with the user-submission erratum of the first DSA
// (Sapphire Rapids, CVE-2024-21823), the idxd driver lets only a process with
// CAP_SYS_RAWIO map the portal; the others give the descriptor with write() on
// the device file, and the driver checks it and writes it to the portal. That
// adds a system call to the submit, but not to the completion.

// Returns the first word of a file, such as the mode of a WQ in sysfs.
static std::string read_first_word(const std::string &path) {
  std::ifstream file(path);
  std::string word;
  if (!(file >> word)) {
    fprintf(stderr, "cannot read %s\n", path.c_str());
    exit(1);
  }
  return word;
}

// One memory move at a time on a DSA WQ. Through the portal, a dedicated WQ
// takes the descriptor with MOVDIR64B, and a shared WQ with ENQCMD, which
// returns "retry" when the WQ is full. Each output is checked against the
// source.
class DsaMemmoveOffload {
 public:
  // The /proc/interrupts lines of the device hold this name.
  static constexpr const char *interrupt_name = "idxd";
  // How the program gives the descriptor to the WQ.
  enum class Submission { portal_instruction, write_system_call };

  DsaMemmoveOffload(const std::string &work_queue_path, size_t offload_bytes,
                    std::mt19937_64 &random)
      : source(random_bytes(offload_bytes, random)),
        destination(offload_bytes),
        record_slots(record_slot_count) {
    std::string work_queue_name = work_queue_path.substr(work_queue_path.rfind('/') + 1);
    std::string sysfs_directory = "/sys/bus/dsa/devices/" + work_queue_name + "/";
    shared_work_queue = read_first_word(sysfs_directory + "mode") == "shared";
    bool block_on_fault = read_first_word(sysfs_directory + "block_on_fault") == "1";
    size_t max_transfer_bytes = std::stoull(read_first_word(sysfs_directory + "max_transfer_size"));
    if (offload_bytes > max_transfer_bytes) {
      fprintf(stderr, "--offload-bytes %zu is more than the %zu bytes that %s moves at most\n",
              offload_bytes, max_transfer_bytes, work_queue_name.c_str());
      exit(1);
    }
    work_queue_fd = open(work_queue_path.c_str(), O_RDWR);
    if (work_queue_fd < 0) {
      fprintf(stderr, "cannot open the DSA WQ %s: %s\n", work_queue_path.c_str(), strerror(errno));
      exit(1);
    }
    portal = mmap(nullptr, portal_bytes, PROT_WRITE, MAP_SHARED | MAP_POPULATE, work_queue_fd, 0);
    if (portal != MAP_FAILED) {
      submission = Submission::portal_instruction;
    } else if (errno == EPERM) {
      portal = nullptr;
      submission = Submission::write_system_call;
      fprintf(stderr,
              "the idxd driver does not let this process map the portal of %s "
              "(no CAP_SYS_RAWIO on a device with the user-submission erratum); "
              "the submit is a write() system call\n",
              work_queue_name.c_str());
    } else {
      exit_with_system_error("mmap of the DSA WQ portal");
    }
    descriptor.opcode = DSA_OPCODE_MEMMOVE;
    // CRAV: completion_addr is valid. RCR: write a completion record. BOF: on
    // a page fault, wait for the kernel to map the page (when the WQ allows it).
    descriptor.flags =
        IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR | (block_on_fault ? IDXD_OP_FLAG_BOF : 0);
    descriptor.src_addr = reinterpret_cast<uint64_t>(source.data());
    descriptor.dst_addr = reinterpret_cast<uint64_t>(destination.data());
    descriptor.xfer_size = static_cast<uint32_t>(offload_bytes);
    // One offload checks that the WQ takes the descriptor and that the device
    // moves the bytes.
    prepare();
    submit();
    if (!reap()) {
      fprintf(stderr, "the DSA offload failed: completion status 0x%02x\n", last_status);
      exit(1);
    }
  }
  ~DsaMemmoveOffload() {
    if (portal != nullptr) {
      munmap(portal, portal_bytes);
    }
    close(work_queue_fd);
  }
  DsaMemmoveOffload(const DsaMemmoveOffload &) = delete;
  DsaMemmoveOffload &operator=(const DsaMemmoveOffload &) = delete;

  // Clears the destination and the completion record of the next offload, and
  // returns the status byte of the record. The device sets it to 1 (success)
  // or to an error code. Each offload has its own record line, as in
  // CompletionRecordPool.
  volatile uint8_t *prepare() {
    std::fill(destination.begin(), destination.end(), 0);
    current_record = &record_slots[next_record_slot].record;
    next_record_slot = (next_record_slot + 1) % record_slot_count;
    memset(current_record, 0, sizeof *current_record);
    descriptor.completion_addr = reinterpret_cast<uint64_t>(current_record);
    return &current_record->status;
  }
  // Gives the descriptor to the WQ: one instruction to the portal, or one
  // write() system call.
  void submit() {
    if (submission == Submission::write_system_call) {
      submit_with_write_system_call();
      return;
    }
    // The fence makes the cleared record and destination visible to the device
    // first.
    _mm_sfence();
    if (!shared_work_queue) {
      _movdir64b(portal, &descriptor);
      return;
    }
    int rejections = 0;
    while (_enqcmd(portal, &descriptor) != 0) {
      if (++rejections == max_enqcmd_rejections) {
        fprintf(stderr, "the shared DSA WQ rejected the descriptor %d times\n", rejections);
        exit(1);
      }
      _mm_pause();
    }
  }
  // Waits for the completion record of the offload (at most 1 s) and returns
  // true when the status is success and the destination holds the source.
  bool reap() {
    double deadline_ns = monotonic_ns() + 1e9;
    while (current_record->status == 0) {
      if (monotonic_ns() > deadline_ns) {
        fprintf(stderr, "the DSA offload did not complete in 1 s\n");
        exit(1);
      }
      _mm_pause();
    }
    last_status = DSA_COMP_STATUS(current_record->status);
    return last_status == DSA_COMP_SUCCESS && destination == source;
  }

 private:
  // The driver writes the descriptor to the portal. For a shared WQ it
  // retries ENQCMD a few times and then returns EAGAIN.
  void submit_with_write_system_call() {
    int rejections = 0;
    while (write(work_queue_fd, &descriptor, sizeof descriptor) !=
           static_cast<ssize_t>(sizeof descriptor)) {
      if (errno != EAGAIN) {
        exit_with_system_error("write of the DSA descriptor");
      }
      if (++rejections == max_enqcmd_rejections) {
        fprintf(stderr, "the shared DSA WQ rejected the descriptor %d times\n", rejections);
        exit(1);
      }
    }
  }

  static constexpr size_t portal_bytes = 4096;
  static constexpr int record_slot_count = 1024;
  static constexpr int max_enqcmd_rejections = 1000000;
  struct alignas(64) CompletionRecordSlot {
    dsa_completion_record record;
  };

  std::vector<uint8_t> source;
  std::vector<uint8_t> destination;
  std::vector<CompletionRecordSlot> record_slots;
  int next_record_slot = 0;
  dsa_completion_record *current_record = nullptr;
  alignas(64) dsa_hw_desc descriptor = {};
  bool shared_work_queue = false;
  int work_queue_fd = -1;
  void *portal = nullptr;
  Submission submission = Submission::portal_instruction;
  uint8_t last_status = 0;
};

// ---------------------------------------------------------------------------
// Device completions: the observer, the device interrupts and the context
// switches of the threads, for the QAT and DSA modes

// A thread on its own CPU that records when a completion byte becomes
// nonzero. The device gives no time stamp of its completion, so the time the
// observer sees the byte stands for the completion time. The observer loads the
// byte in a tight loop, as the filler CPU does in qat-floor mode.
class CompletionObserver {
 public:
  explicit CompletionObserver(int cpu) : observer_thread([this, cpu] { observe(cpu); }) {}
  ~CompletionObserver() {
    stop_requested.store(true, std::memory_order_release);
    observer_thread.join();
  }
  // Returns when the observer loads the byte. Call before the submit.
  void watch(volatile uint8_t *completion_byte) {
    watched_byte = completion_byte;
    state.store(State::watch_requested, std::memory_order_release);
    while (state.load(std::memory_order_acquire) != State::watching) {
      _mm_pause();
    }
  }
  // Waits until the observer sees the byte set, and returns the TSC it read
  // then.
  uint64_t wait_for_completion_tsc() {
    while (state.load(std::memory_order_acquire) != State::completion_seen) {
      _mm_pause();
    }
    state.store(State::idle, std::memory_order_relaxed);
    return completion_seen_tsc;
  }
  // The kernel thread id of the observer thread.
  pid_t thread_id() const {
    while (observer_thread_id.load(std::memory_order_acquire) == 0) {
      _mm_pause();
    }
    return observer_thread_id.load(std::memory_order_acquire);
  }

 private:
  enum class State { idle, watch_requested, watching, completion_seen };

  void observe(int cpu) {
    pin_current_thread_to_cpu(cpu);
    observer_thread_id.store(gettid(), std::memory_order_release);
    while (!stop_requested.load(std::memory_order_acquire)) {
      if (state.load(std::memory_order_acquire) != State::watch_requested) {
        _mm_pause();
        continue;
      }
      volatile uint8_t *completion_byte = watched_byte;
      state.store(State::watching, std::memory_order_release);
      while (*completion_byte == 0) {
      }
      completion_seen_tsc = read_tsc();
      state.store(State::completion_seen, std::memory_order_release);
    }
  }

  volatile uint8_t *watched_byte = nullptr;  // written before watch_requested
  uint64_t completion_seen_tsc = 0;          // written before completion_seen
  std::atomic<State> state{State::idle};
  std::atomic<bool> stop_requested{false};
  std::atomic<pid_t> observer_thread_id{0};
  std::thread observer_thread;  // Last, so that it starts after the atomics exist.
};

// The interrupts of a kind of device that each CPU has handled, from
// /proc/interrupts: the sum over the lines whose names hold name_part.
static std::map<int, uint64_t> read_device_interrupts_per_cpu(const char *name_part) {
  std::ifstream interrupts_file("/proc/interrupts");
  std::string line;
  std::getline(interrupts_file, line);
  std::vector<int> column_cpus;  // the header names each column: CPU0 CPU1 ...
  std::istringstream header(line);
  std::string cpu_name;
  while (header >> cpu_name) {
    column_cpus.push_back(atoi(cpu_name.c_str() + strlen("CPU")));
  }
  std::map<int, uint64_t> interrupts_per_cpu;
  while (std::getline(interrupts_file, line)) {
    if (line.find(name_part) == std::string::npos) {
      continue;
    }
    std::istringstream fields(line);
    std::string irq_label;
    fields >> irq_label;
    for (int cpu : column_cpus) {
      uint64_t count = 0;
      fields >> count;
      interrupts_per_cpu[cpu] += count;
    }
  }
  return interrupts_per_cpu;
}

struct DeviceInterrupts {
  uint64_t total = 0;
  int busiest_cpu = -1;  // the CPU that handled the most of them
  uint64_t busiest_cpu_count = 0;
};

static DeviceInterrupts device_interrupts_between(const std::map<int, uint64_t> &before,
                                                  const std::map<int, uint64_t> &after) {
  DeviceInterrupts interrupts;
  for (const auto &[cpu, count_after] : after) {
    auto count_before = before.find(cpu);
    uint64_t count = count_after - (count_before == before.end() ? 0 : count_before->second);
    interrupts.total += count;
    if (count > interrupts.busiest_cpu_count) {
      interrupts.busiest_cpu = cpu;
      interrupts.busiest_cpu_count = count;
    }
  }
  return interrupts;
}

// The involuntary context switches of a thread of this process so far: the
// times the kernel took its CPU to run another thread.
static uint64_t read_involuntary_context_switches(pid_t thread_id) {
  std::ifstream status_file("/proc/self/task/" + std::to_string(thread_id) + "/status");
  const std::string field = "nonvoluntary_ctxt_switches:";
  std::string line;
  while (std::getline(status_file, line)) {
    if (line.compare(0, field.size(), field) == 0) {
      return std::stoull(line.substr(field.size()));
    }
  }
  fprintf(stderr, "no %s in the status of thread %d\n", field.c_str(), thread_id);
  exit(1);
}

// Counts the involuntary context switches of the filler thread (the thread
// that makes the counter) and of the observer thread from the construction of
// the counter. They are 0 when nothing else runs on the two CPUs. On a busy
// host, a run with a switch has a long time.
class InvoluntarySwitchCounter {
 public:
  explicit InvoluntarySwitchCounter(const CompletionObserver &observer)
      : filler_thread_id(gettid()),
        observer_thread_id(observer.thread_id()),
        filler_switches_at_start(read_involuntary_context_switches(filler_thread_id)),
        observer_switches_at_start(read_involuntary_context_switches(observer_thread_id)) {}
  uint64_t filler_switches() const {
    return read_involuntary_context_switches(filler_thread_id) - filler_switches_at_start;
  }
  uint64_t observer_switches() const {
    return read_involuntary_context_switches(observer_thread_id) - observer_switches_at_start;
  }

 private:
  pid_t filler_thread_id;
  pid_t observer_thread_id;
  uint64_t filler_switches_at_start;
  uint64_t observer_switches_at_start;
};

// ---------------------------------------------------------------------------
// Filler fiber

static void filler_fiber_entry(fcontext_transfer_t transfer) {
  scheduler_transfer = transfer;
  Filler *filler = static_cast<Filler *>(transfer.data);
  filler->run();
  filler_stopped_tsc = read_tsc();
  filler_finished = true;
  fcontext_swap(scheduler_transfer.prev_context, nullptr);
  // The scheduler never swaps back to a finished filler.
}

// The fiber of switch-floor: it yields in a loop and never ends.
static void yield_loop_fiber_entry(fcontext_transfer_t transfer) {
  scheduler_transfer = transfer;
  while (true) {
    do_yield();
  }
}

// One fiber stack, used again for each run.
class FillerFiber {
 public:
  FillerFiber() : state(fcontext_create(filler_fiber_entry)) {}
  ~FillerFiber() { fcontext_destroy(state); }
  FillerFiber(const FillerFiber &) = delete;
  FillerFiber &operator=(const FillerFiber &) = delete;
  // A new context at the top of the stack that enters entry_function.
  fcontext_t new_context(fcontext_fn_t entry_function = filler_fiber_entry) {
    return make_fcontext(state->stack + FCONTEXT_STACK_SIZE, FCONTEXT_STACK_SIZE,
                         entry_function);
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

// The parts of the switch of one yield with stamping_switch_parts, in TSC
// ticks. See yield_with_switch_experiment.
struct SwitchParts {
  int64_t switch_with_stamps_ticks;  // from the filler stop to the scheduler stamp
  int64_t stamp_ticks;
  int64_t checks_ticks;
  int64_t store_drain_ticks;
  int64_t context_load_ticks;
  int64_t jump_ticks;
  // switch_with_stamps_ticks without the four stamps of the parts: the sum
  // of stamp_ticks and the four parts.
  int64_t switch_without_part_stamps_ticks;
};

// Call in the scheduler at once after a yield, before the filler can yield
// again.
static SwitchParts switch_parts_of_yield(uint64_t stopped_tsc, uint64_t scheduler_resumed_tsc) {
  const SwitchPartStamps &stamps = switch_part_stamps;
  SwitchParts parts;
  parts.switch_with_stamps_ticks = static_cast<int64_t>(scheduler_resumed_tsc - stopped_tsc);
  parts.stamp_ticks = static_cast<int64_t>(stamps.second_stamp_tsc - stamps.checks_end_tsc);
  parts.checks_ticks =
      static_cast<int64_t>(stamps.checks_end_tsc - stopped_tsc) - parts.stamp_ticks;
  parts.store_drain_ticks =
      static_cast<int64_t>(stamps.stores_drained_tsc - stamps.second_stamp_tsc) -
      parts.stamp_ticks;
  parts.context_load_ticks =
      static_cast<int64_t>(stamps.context_loaded_tsc - stamps.stores_drained_tsc) -
      parts.stamp_ticks;
  parts.jump_ticks =
      static_cast<int64_t>(scheduler_resumed_tsc - stamps.context_loaded_tsc) - parts.stamp_ticks;
  parts.switch_without_part_stamps_ticks = parts.switch_with_stamps_ticks - 4 * parts.stamp_ticks;
  return parts;
}

struct PreemptedRun {
  RunOutcome outcome;
  // From the completion to the filler stop: the entry of do_yield at the probe
  // point that saw the completion, or the end of the filler. Negative for
  // finished_before_completion.
  int64_t detect_ticks;
  // From the filler stop to the scheduler running again: the switch from the
  // filler fiber to the scheduler.
  int64_t switch_ticks;
  // From the completion time asked for to the completion. Large when the OS
  // takes the accelerator CPU away: the run then does not show the probes.
  int64_t accelerator_lateness_ticks;
  bool output_correct;
  // Only for a yield with stamping_switch_parts.
  std::optional<SwitchParts> switch_parts;
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
  uint64_t scheduler_resumed_tsc = read_tsc();
  preempt_signal = &signal_byte_never_set;
  uint64_t stopped_tsc = filler_stopped_tsc;
  bool yielded = !filler_finished;
  PreemptedRun run;
  if (yielded && stamping_switch_parts) {
    run.switch_parts = switch_parts_of_yield(stopped_tsc, scheduler_resumed_tsc);
  }
  if (yielded) {
    fcontext_swap(from_filler.prev_context, nullptr);
  }
  inside_filler_fiber = false;
  accelerator.wait_until_idle();

  run.detect_ticks = static_cast<int64_t>(stopped_tsc - record.completion_tsc);
  run.switch_ticks = static_cast<int64_t>(scheduler_resumed_tsc - stopped_tsc);
  run.accelerator_lateness_ticks =
      static_cast<int64_t>(record.completion_tsc - requested_completion_tsc);
  if (yielded) {
    run.outcome = RunOutcome::yielded;
  } else if (run.detect_ticks >= 0) {
    run.outcome = RunOutcome::finished_after_completion;
  } else {
    run.outcome = RunOutcome::finished_before_completion;
  }
  run.output_correct = filler.checksum() == reference_checksum;
  return run;
}

struct DeviceOffloadRun {
  RunOutcome outcome;
  // The time of the scheduler in the submit: sendmsg and io_submit for QAT,
  // the fence and the portal write for DSA.
  int64_t submit_ticks;
  // From the start of the submit to the observer seeing the completion byte.
  // QAT: the copy into the kernel, the device, its interrupt and the kernel
  // completion. DSA: the device and its completion record write.
  int64_t offload_ticks;
  // From the observer seeing the completion byte to the filler stop. Can be
  // negative for a yield: qat-floor and dsa-floor measure the offset between
  // the filler CPU and the observer CPU in seeing the byte.
  int64_t detect_ticks;
  // From the filler stop to the scheduler running again.
  int64_t switch_ticks;
  // The completion byte was set when the submit returned. When this is so on
  // each run, the offload is done in the submit and not on the device in the
  // background; on a few runs it only means that the submit was slow.
  bool completed_during_submit;
  bool output_correct;
  bool offload_output_correct;
  // Only for a yield with stamping_switch_parts.
  std::optional<SwitchParts> switch_parts;
};

// run_filler_with_offload with a QAT or DSA offload in flight.
template <typename DeviceOffload>
static DeviceOffloadRun run_filler_with_device_offload(Filler &filler, FillerFiber &fiber,
                                                       DeviceOffload &offload,
                                                       CompletionObserver &observer,
                                                       uint64_t reference_checksum) {
  filler.reset();
  volatile uint8_t *completion_byte = offload.prepare();
  observer.watch(completion_byte);
  filler_finished = false;
  inside_filler_fiber = true;
  preempt_signal = completion_byte;
  uint64_t submit_start_tsc = read_tsc();
  offload.submit();
  uint64_t submit_end_tsc = read_tsc();
  bool completed_during_submit = *completion_byte != 0;

  fcontext_transfer_t from_filler = fcontext_swap(fiber.new_context(), &filler);
  uint64_t scheduler_resumed_tsc = read_tsc();
  preempt_signal = &signal_byte_never_set;
  uint64_t stopped_tsc = filler_stopped_tsc;
  bool yielded = !filler_finished;
  DeviceOffloadRun run;
  if (yielded && stamping_switch_parts) {
    run.switch_parts = switch_parts_of_yield(stopped_tsc, scheduler_resumed_tsc);
  }
  if (yielded) {
    fcontext_swap(from_filler.prev_context, nullptr);
  }
  inside_filler_fiber = false;
  bool offload_output_correct = offload.reap();
  uint64_t completion_seen_tsc = observer.wait_for_completion_tsc();

  run.submit_ticks = static_cast<int64_t>(submit_end_tsc - submit_start_tsc);
  run.offload_ticks = static_cast<int64_t>(completion_seen_tsc - submit_start_tsc);
  run.detect_ticks = static_cast<int64_t>(stopped_tsc - completion_seen_tsc);
  run.switch_ticks = static_cast<int64_t>(scheduler_resumed_tsc - stopped_tsc);
  if (yielded) {
    run.outcome = RunOutcome::yielded;
  } else if (run.detect_ticks >= 0) {
    run.outcome = RunOutcome::finished_after_completion;
  } else {
    run.outcome = RunOutcome::finished_before_completion;
  }
  run.completed_during_submit = completed_during_submit;
  run.output_correct = filler.checksum() == reference_checksum;
  run.offload_output_correct = offload_output_correct;
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
  int observer_cpu = 6;
  size_t offload_bytes = 4096;
  std::string work_queue_path = "/dev/dsa/wq0.1";
  double idle_us = 0;
  double offload_ns = 0;  // 0: the default of the mode
  uint64_t seed = 1;
  std::string samples_path;
  std::string switch_parts_path;
  YieldJump yield_jump = YieldJump::boost;
  std::string ipp_code_path = "native";
  bool print_header = true;
};

constexpr int warmup_runs = 5;
// A completion this much later than asked for counts as late: the OS took the
// accelerator CPU away. Use idle CPUs when the late count is not near 0.
constexpr double late_completion_ns = 1000;
constexpr double signal_floor_default_offload_ns = 1000;
// In the spin loop of qat-interrupt, a step between two TSC reads longer than
// this is time in the kernel. One step with no kernel entry is a load and a
// TSC read, about 10 ns.
constexpr double kernel_gap_threshold_ns = 200;

// Runs the filler warmup_runs times on the calling thread, with no offload.
// Returns the checksum of the last run: the reference output.
static uint64_t warm_up(Filler &filler) {
  for (int run = 0; run < warmup_runs; run++) {
    filler.reset();
    filler.run();
  }
  return filler.checksum();
}

// The clock of the filler core in GHz over one chain of 2 million dependent
// register adds (about 1 ms), one core cycle each. The TSC does not count core
// cycles: its rate is the same at each core clock.
static double time_core_ghz_once(const TscClock &clock) {
  constexpr uint64_t iterations = 250000;
  constexpr int adds_per_iteration = 8;
  uint64_t sum = 0;
  uint64_t one = 1;
  uint64_t start_tsc = read_tsc();
  for (uint64_t iteration = 0; iteration < iterations; iteration++) {
    // Register operands: the renamer of a core can do an add of an immediate
    // with no execution cycle.
    asm volatile(
        "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\t"
        "add %1, %0\n\tadd %1, %0\n\tadd %1, %0\n\tadd %1, %0"
        : "+r"(sum)
        : "r"(one));
  }
  uint64_t end_tsc = read_tsc();
  return static_cast<double>(adds_per_iteration * iterations) /
         clock.ticks_to_ns(static_cast<double>(end_tsc - start_tsc));
}

// The clock of the filler core in GHz, after the clock is steady. The
// schedutil governor of sapphire raises the clock of an idle core over tens of
// ms of work, so this times chains of adds until two in a row agree within 1 %
// (at most 1000 chains, about 1 s). It also keeps the core busy until the runs
// that follow start at that clock. Then it gives the fastest of 50 more
// chains: when another task shares the CPU, a chain that the task interrupts
// takes longer and gives a lower clock (on emerald-02, a CPU that two tasks
// shared gave 1.10 GHz with 80 ms chains, and 2.18 GHz alone). The ondemand
// governor of emerald-03 can hold a busy core at its lowest clock (0.8 GHz)
// for more than 8 s, so a run there has a clock that changes in the run.
static double measure_core_ghz(const TscClock &clock) {
  double previous_ghz = time_core_ghz_once(clock);
  for (int chain = 1; chain < 1000; chain++) {
    double ghz = time_core_ghz_once(clock);
    bool clock_is_steady = std::fabs(ghz - previous_ghz) <= 0.01 * ghz;
    previous_ghz = ghz;
    if (clock_is_steady) {
      break;
    }
  }
  double fastest_ghz = previous_ghz;
  for (int chain = 0; chain < 50; chain++) {
    fastest_ghz = std::max(fastest_ghz, time_core_ghz_once(clock));
  }
  return fastest_ghz;
}

// The CSV header of the --switch-parts file. The part columns are in ns.
constexpr const char *switch_parts_header =
    "mode,probe_interval,filler,cpu,yield_jump,core_ghz,yields,"
    "switch_with_stamps_median_ns,switch_without_part_stamps_median_ns,"
    "switch_without_part_stamps_p90_ns,stamp_median_ns,"
    "checks_median_ns,checks_p90_ns,store_drain_median_ns,store_drain_p90_ns,"
    "context_load_median_ns,context_load_p90_ns,jump_median_ns,jump_p90_ns";

// The switch parts of the yields of one filler, in ns.
class SwitchPartsCollector {
 public:
  void add(const SwitchParts &parts, const TscClock &clock) {
    auto ns = [&clock](int64_t ticks) { return clock.ticks_to_ns(static_cast<double>(ticks)); };
    switches_with_stamps_ns.push_back(ns(parts.switch_with_stamps_ticks));
    switches_without_part_stamps_ns.push_back(ns(parts.switch_without_part_stamps_ticks));
    stamps_ns.push_back(ns(parts.stamp_ticks));
    checks_ns.push_back(ns(parts.checks_ticks));
    store_drains_ns.push_back(ns(parts.store_drain_ticks));
    context_loads_ns.push_back(ns(parts.context_load_ticks));
    jumps_ns.push_back(ns(parts.jump_ticks));
  }
  // Writes one row with switch_parts_header.
  void write_row(FILE *file, const Options &options, const char *filler_name,
                 double core_ghz) const {
    fprintf(file, "%s,%s,%s,%d,%s,%.2f,%zu", options.mode.c_str(), RACER_PROBE_INTERVAL,
            filler_name, options.cpu, yield_jump_name(options.yield_jump), core_ghz,
            switches_with_stamps_ns.size());
    fprintf(file, ",%.1f,%.1f,%.1f,%.1f", percentile(switches_with_stamps_ns, 0.5),
            percentile(switches_without_part_stamps_ns, 0.5),
            percentile(switches_without_part_stamps_ns, 0.9), percentile(stamps_ns, 0.5));
    for (const std::vector<double> *part_ns :
         {&checks_ns, &store_drains_ns, &context_loads_ns, &jumps_ns}) {
      fprintf(file, ",%.1f,%.1f", percentile(*part_ns, 0.5), percentile(*part_ns, 0.9));
    }
    fprintf(file, "\n");
    fflush(file);
  }

 private:
  std::vector<double> switches_with_stamps_ns;
  std::vector<double> switches_without_part_stamps_ns;
  std::vector<double> stamps_ns;
  std::vector<double> checks_ns;
  std::vector<double> store_drains_ns;
  std::vector<double> context_loads_ns;
  std::vector<double> jumps_ns;
};

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

// Runs the filler once with preempt_signal at a set byte and do_yield counting
// instead of switching, and returns the number of probe points the run
// executes. A probe point is one check at a loop header, so a run with the
// signal clear executes the same number. Not timed.
static uint64_t count_probe_executions(Filler &filler, uint64_t reference_checksum,
                                       int &wrong_outputs) {
  filler.reset();
  probe_executions = 0;
  counting_probe_executions = true;
  preempt_signal = &signal_byte_always_set;
  filler.run();
  preempt_signal = &signal_byte_never_set;
  counting_probe_executions = false;
  if (filler.checksum() != reference_checksum) {
    wrong_outputs++;
  }
  return probe_executions;
}

static void measure_overhead(const char *filler_name, Filler &filler, const Options &options,
                             const TscClock &clock) {
  uint64_t reference_checksum = warm_up(filler);
  int wrong_outputs = 0;
  std::vector<double> runtimes_ns =
      time_runs(filler, options.runs, reference_checksum, clock, wrong_outputs);
  uint64_t probe_executions_per_run =
      count_probe_executions(filler, reference_checksum, wrong_outputs);
  printf("overhead,%s,%s,%s,%zu,%d,%d,%.0f,%.0f,%.0f,%" PRIu64 ",%016" PRIx64 "\n",
         RACER_PROBE_INTERVAL, options.ipp_code_path.c_str(), filler_name, options.size_bytes,
         options.runs, wrong_outputs, percentile(runtimes_ns, 0.5), percentile(runtimes_ns, 0.1),
         percentile(runtimes_ns, 0.9), probe_executions_per_run, reference_checksum);
}

static void measure_latency(const char *filler_name, Filler &filler, const Options &options,
                            const TscClock &clock, EmulatedAccelerator &accelerator,
                            std::mt19937_64 &offload_random, FILE *samples,
                            FILE *switch_parts_file) {
  uint64_t reference_checksum = warm_up(filler);
  int wrong_outputs = 0;
  double filler_median_ns =
      percentile(time_runs(filler, 20, reference_checksum, clock, wrong_outputs), 0.5);
  std::uniform_real_distribution<double> uniform_offload_ns(0, 0.9 * filler_median_ns);
  double core_ghz = switch_parts_file != nullptr ? measure_core_ghz(clock) : NAN;

  FillerFiber fiber;
  CompletionRecordPool records;
  int run_counts[3] = {};
  // Of the runs where the completion came before the filler stopped.
  std::vector<double> latencies_ns;
  std::vector<double> detects_ns;
  std::vector<double> switches_ns;
  SwitchPartsCollector switch_parts;
  int late_completions = 0;
  for (int run_index = 0; run_index < options.runs; run_index++) {
    double offload_ns =
        options.offload_ns > 0 ? options.offload_ns : uniform_offload_ns(offload_random);
    PreemptedRun run = run_filler_with_offload(filler, fiber, accelerator, records.next(),
                                               clock.ns_to_ticks(offload_ns), reference_checksum);
    run_counts[static_cast<int>(run.outcome)]++;
    if (!run.output_correct) {
      wrong_outputs++;
    }
    if (run.switch_parts) {
      switch_parts.add(*run.switch_parts, clock);
    }
    double detect_ns = clock.ticks_to_ns(static_cast<double>(run.detect_ticks));
    double switch_ns = clock.ticks_to_ns(static_cast<double>(run.switch_ticks));
    double latency_ns = detect_ns + switch_ns;
    double lateness_ns = clock.ticks_to_ns(static_cast<double>(run.accelerator_lateness_ticks));
    if (lateness_ns > late_completion_ns) {
      late_completions++;
    }
    if (run.outcome != RunOutcome::finished_before_completion) {
      latencies_ns.push_back(latency_ns);
      detects_ns.push_back(detect_ns);
      switches_ns.push_back(switch_ns);
    }
    if (samples != nullptr) {
      fprintf(samples, "%s,%s,%s,%zu,%d,%.0f,%.0f,%s,%.0f,%.0f,%.0f,%d\n", RACER_PROBE_INTERVAL,
              options.ipp_code_path.c_str(), filler_name, options.size_bytes, run_index, offload_ns,
              lateness_ns, outcome_name(run.outcome), detect_ns, switch_ns, latency_ns,
              run.output_correct ? 1 : 0);
    }
  }
  printf("latency,%s,%s,%s,%zu,%d,%d,%d,%d,%d,%d", RACER_PROBE_INTERVAL,
         options.ipp_code_path.c_str(), filler_name, options.size_bytes, options.runs,
         run_counts[static_cast<int>(RunOutcome::yielded)],
         run_counts[static_cast<int>(RunOutcome::finished_after_completion)],
         run_counts[static_cast<int>(RunOutcome::finished_before_completion)], late_completions,
         wrong_outputs);
  printf(",%.0f,%.0f,%.0f,%.0f", percentile(latencies_ns, 0.5), percentile(latencies_ns, 0.9),
         percentile(latencies_ns, 0.99), percentile(latencies_ns, 1.0));
  printf(",%.0f,%.0f,%.0f", percentile(detects_ns, 0.5), percentile(detects_ns, 0.9),
         percentile(detects_ns, 0.99));
  printf(",%.0f,%.0f,%.0f", percentile(switches_ns, 0.5), percentile(switches_ns, 0.9),
         percentile(switches_ns, 0.99));
  printf(",%.0f\n", filler_median_ns);
  if (switch_parts_file != nullptr) {
    switch_parts.write_row(switch_parts_file, options, filler_name, core_ghz);
  }
}

// The least detect time. With no filler and no fiber, the filler CPU loads the
// status byte in a tight loop until it sees the completion. The time from the
// completion to that load is the time for the status write to reach the filler
// CPU, plus a TSC read and any TSC offset between the two CPUs.
static void measure_signal_floor(const Options &options, const TscClock &clock,
                                 EmulatedAccelerator &accelerator) {
  double offload_ns = options.offload_ns > 0 ? options.offload_ns : signal_floor_default_offload_ns;
  CompletionRecordPool records;
  std::vector<double> floors_ns;
  int late_completions = 0;
  for (int run_index = 0; run_index < warmup_runs + options.runs; run_index++) {
    CompletionRecord &record = records.next();
    record.status = 0;
    record.completion_tsc = 0;
    uint64_t requested_completion_tsc = read_tsc() + clock.ns_to_ticks(offload_ns);
    accelerator.submit(&record, requested_completion_tsc);
    while (record.status == 0) {
    }
    uint64_t seen_tsc = read_tsc();
    accelerator.wait_until_idle();
    if (run_index < warmup_runs) {
      continue;
    }
    floors_ns.push_back(clock.ticks_to_ns(
        static_cast<double>(static_cast<int64_t>(seen_tsc - record.completion_tsc))));
    double lateness_ns = clock.ticks_to_ns(static_cast<double>(
        static_cast<int64_t>(record.completion_tsc - requested_completion_tsc)));
    if (lateness_ns > late_completion_ns) {
      late_completions++;
    }
  }
  printf("signal-floor,%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%.0f\n", options.cpu,
         options.accelerator_cpu, options.runs, late_completions, percentile(floors_ns, 0.5),
         percentile(floors_ns, 0.1), percentile(floors_ns, 0.9), percentile(floors_ns, 0.99),
         percentile(floors_ns, 1.0));
}

// qat-latency and dsa-latency.
template <typename DeviceOffload>
static void measure_device_latency(const char *filler_name, Filler &filler, const Options &options,
                                   const TscClock &clock, DeviceOffload &offload,
                                   CompletionObserver &observer, FILE *samples,
                                   FILE *switch_parts_file) {
  uint64_t reference_checksum = warm_up(filler);
  int wrong_outputs = 0;
  double filler_median_ns =
      percentile(time_runs(filler, 20, reference_checksum, clock, wrong_outputs), 0.5);
  double core_ghz = switch_parts_file != nullptr ? measure_core_ghz(clock) : NAN;

  FillerFiber fiber;
  for (int run_index = 0; run_index < warmup_runs; run_index++) {
    run_filler_with_device_offload(filler, fiber, offload, observer, reference_checksum);
  }
  int run_counts[3] = {};
  int failed_offloads = 0;
  int completed_during_submit_count = 0;
  std::vector<double> submits_ns;
  std::vector<double> offloads_ns;
  // Of the runs where the completion came before the filler stopped.
  std::vector<double> latencies_ns;
  std::vector<double> detects_ns;
  std::vector<double> switches_ns;
  SwitchPartsCollector switch_parts;
  std::map<int, uint64_t> interrupts_before =
      read_device_interrupts_per_cpu(DeviceOffload::interrupt_name);
  InvoluntarySwitchCounter involuntary_switches(observer);
  for (int run_index = 0; run_index < options.runs; run_index++) {
    DeviceOffloadRun run =
        run_filler_with_device_offload(filler, fiber, offload, observer, reference_checksum);
    run_counts[static_cast<int>(run.outcome)]++;
    if (!run.output_correct) {
      wrong_outputs++;
    }
    if (run.switch_parts) {
      switch_parts.add(*run.switch_parts, clock);
    }
    if (!run.offload_output_correct) {
      failed_offloads++;
    }
    if (run.completed_during_submit) {
      completed_during_submit_count++;
    }
    double submit_ns = clock.ticks_to_ns(static_cast<double>(run.submit_ticks));
    double offload_ns = clock.ticks_to_ns(static_cast<double>(run.offload_ticks));
    double detect_ns = clock.ticks_to_ns(static_cast<double>(run.detect_ticks));
    double switch_ns = clock.ticks_to_ns(static_cast<double>(run.switch_ticks));
    double latency_ns = detect_ns + switch_ns;
    submits_ns.push_back(submit_ns);
    offloads_ns.push_back(offload_ns);
    if (run.outcome != RunOutcome::finished_before_completion) {
      latencies_ns.push_back(latency_ns);
      detects_ns.push_back(detect_ns);
      switches_ns.push_back(switch_ns);
    }
    if (samples != nullptr) {
      fprintf(samples, "%s,%s,%s,%zu,%zu,%d,%.0f,%.0f,%s,%.0f,%.0f,%.0f,%d,%d\n",
              RACER_PROBE_INTERVAL, options.ipp_code_path.c_str(), filler_name, options.size_bytes,
              options.offload_bytes, run_index, submit_ns, offload_ns, outcome_name(run.outcome),
              detect_ns, switch_ns, latency_ns, run.output_correct ? 1 : 0,
              run.offload_output_correct ? 1 : 0);
    }
  }
  uint64_t filler_involuntary_switches = involuntary_switches.filler_switches();
  uint64_t observer_involuntary_switches = involuntary_switches.observer_switches();
  DeviceInterrupts interrupts = device_interrupts_between(
      interrupts_before, read_device_interrupts_per_cpu(DeviceOffload::interrupt_name));
  printf("%s,%s,%s,%s,%zu,%zu,%d,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%d,%" PRIu64, options.mode.c_str(),
         RACER_PROBE_INTERVAL, options.ipp_code_path.c_str(), filler_name, options.size_bytes,
         options.offload_bytes, options.runs, run_counts[static_cast<int>(RunOutcome::yielded)],
         run_counts[static_cast<int>(RunOutcome::finished_after_completion)],
         run_counts[static_cast<int>(RunOutcome::finished_before_completion)], wrong_outputs,
         failed_offloads, completed_during_submit_count, interrupts.total, interrupts.busiest_cpu,
         interrupts.busiest_cpu_count);
  printf(",%.0f,%.0f,%.0f,%.0f", percentile(submits_ns, 0.5), percentile(submits_ns, 0.99),
         percentile(offloads_ns, 0.5), percentile(offloads_ns, 0.99));
  printf(",%.0f,%.0f,%.0f,%.0f", percentile(latencies_ns, 0.5), percentile(latencies_ns, 0.9),
         percentile(latencies_ns, 0.99), percentile(latencies_ns, 1.0));
  printf(",%.0f,%.0f,%.0f", percentile(detects_ns, 0.5), percentile(detects_ns, 0.9),
         percentile(detects_ns, 0.99));
  printf(",%.0f,%.0f,%.0f", percentile(switches_ns, 0.5), percentile(switches_ns, 0.9),
         percentile(switches_ns, 0.99));
  printf(",%.0f,%" PRIu64 ",%" PRIu64 "\n", filler_median_ns, filler_involuntary_switches,
         observer_involuntary_switches);
  if (switch_parts_file != nullptr) {
    switch_parts.write_row(switch_parts_file, options, filler_name, core_ghz);
  }
}

// The least switch time: the switch of a fiber that does nothing but yield,
// so the store buffer holds no filler stores and the code and the contexts
// stay in the cache. Prints one row; with --switch-parts, also writes a parts
// row with the filler name yield-loop.
static void measure_switch_floor(const Options &options, const TscClock &clock,
                                 FILE *switch_parts_file) {
  double core_ghz = measure_core_ghz(clock);
  FillerFiber fiber;
  fcontext_t fiber_context = fiber.new_context(yield_loop_fiber_entry);
  std::vector<double> switches_ns;
  SwitchPartsCollector switch_parts;
  inside_filler_fiber = true;
  for (int run_index = 0; run_index < warmup_runs + options.runs; run_index++) {
    fcontext_transfer_t from_fiber = fcontext_swap(fiber_context, nullptr);
    uint64_t scheduler_resumed_tsc = read_tsc();
    fiber_context = from_fiber.prev_context;
    uint64_t stopped_tsc = filler_stopped_tsc;
    if (run_index < warmup_runs) {
      continue;
    }
    switches_ns.push_back(
        clock.ticks_to_ns(static_cast<double>(scheduler_resumed_tsc - stopped_tsc)));
    if (stamping_switch_parts) {
      switch_parts.add(switch_parts_of_yield(stopped_tsc, scheduler_resumed_tsc), clock);
    }
  }
  inside_filler_fiber = false;
  printf("%s,%d,%s,%d,%d,%.2f,%.1f,%.1f,%.1f,%.1f\n", options.mode.c_str(), options.cpu,
         yield_jump_name(options.yield_jump), stamping_switch_parts ? 1 : 0, options.runs,
         core_ghz, percentile(switches_ns, 0.5), percentile(switches_ns, 0.1),
         percentile(switches_ns, 0.9), percentile(switches_ns, 0.99));
  if (switch_parts_file != nullptr) {
    switch_parts.write_row(switch_parts_file, options, "yield-loop", core_ghz);
  }
}

// The least detect time of qat-latency and dsa-latency. With no filler and no
// fiber, the filler CPU loads the completion byte in a tight loop, as the
// observer does. The time from the observer seeing the byte to the filler CPU
// seeing it is the offset between the two CPUs: the difference of their
// cache-line transfers from the writer of the byte (the CPU of the QAT
// interrupt, or the DSA device), plus any TSC offset. Before each offload, the
// device is idle for --idle-us: the offload time depends on how long the
// device was idle.
template <typename DeviceOffload>
static void measure_device_floor(const Options &options, const TscClock &clock,
                                 DeviceOffload &offload, CompletionObserver &observer) {
  std::vector<double> submits_ns;
  std::vector<double> offloads_ns;
  std::vector<double> floors_ns;
  int failed_offloads = 0;
  int completed_during_submit_count = 0;
  uint64_t idle_ticks = static_cast<uint64_t>(clock.ns_to_ticks(options.idle_us * 1000));
  std::map<int, uint64_t> interrupts_before;
  std::optional<InvoluntarySwitchCounter> involuntary_switches;
  for (int run_index = 0; run_index < warmup_runs + options.runs; run_index++) {
    if (run_index == warmup_runs) {
      interrupts_before = read_device_interrupts_per_cpu(DeviceOffload::interrupt_name);
      involuntary_switches.emplace(observer);
    }
    volatile uint8_t *completion_byte = offload.prepare();
    observer.watch(completion_byte);
    uint64_t idle_start_tsc = read_tsc();
    while (read_tsc() - idle_start_tsc < idle_ticks) {
    }
    uint64_t submit_start_tsc = read_tsc();
    offload.submit();
    uint64_t submit_end_tsc = read_tsc();
    bool completed_during_submit = *completion_byte != 0;
    while (*completion_byte == 0) {
    }
    uint64_t seen_tsc = read_tsc();
    bool offload_output_correct = offload.reap();
    uint64_t completion_seen_tsc = observer.wait_for_completion_tsc();
    if (run_index < warmup_runs) {
      continue;
    }
    if (!offload_output_correct) {
      failed_offloads++;
    }
    if (completed_during_submit) {
      completed_during_submit_count++;
    }
    submits_ns.push_back(clock.ticks_to_ns(static_cast<double>(submit_end_tsc - submit_start_tsc)));
    offloads_ns.push_back(clock.ticks_to_ns(
        static_cast<double>(static_cast<int64_t>(completion_seen_tsc - submit_start_tsc))));
    floors_ns.push_back(clock.ticks_to_ns(
        static_cast<double>(static_cast<int64_t>(seen_tsc - completion_seen_tsc))));
  }
  uint64_t filler_involuntary_switches = involuntary_switches->filler_switches();
  uint64_t observer_involuntary_switches = involuntary_switches->observer_switches();
  DeviceInterrupts interrupts = device_interrupts_between(
      interrupts_before, read_device_interrupts_per_cpu(DeviceOffload::interrupt_name));
  printf("%s,%d,%d,%zu,%g,%d,%d,%d,%" PRIu64 ",%d,%" PRIu64, options.mode.c_str(), options.cpu,
         options.observer_cpu, options.offload_bytes, options.idle_us, options.runs,
         failed_offloads, completed_during_submit_count, interrupts.total, interrupts.busiest_cpu,
         interrupts.busiest_cpu_count);
  printf(",%.0f,%.0f", percentile(submits_ns, 0.5), percentile(submits_ns, 0.99));
  printf(",%.0f,%.0f,%.0f,%.0f", percentile(offloads_ns, 0.5), percentile(offloads_ns, 0.1),
         percentile(offloads_ns, 0.9), percentile(offloads_ns, 0.99));
  printf(",%.0f,%.0f,%.0f,%.0f", percentile(floors_ns, 0.5), percentile(floors_ns, 0.1),
         percentile(floors_ns, 0.9), percentile(floors_ns, 0.99));
  printf(",%" PRIu64 ",%" PRIu64 "\n", filler_involuntary_switches, observer_involuntary_switches);
}

static void print_median_and_tail(const std::vector<double> &values_ns) {
  printf(",%.0f,%.0f,%.0f,%.0f", percentile(values_ns, 0.5), percentile(values_ns, 0.1),
         percentile(values_ns, 0.9), percentile(values_ns, 0.99));
}

// The kernel time of the QAT completion. Run with --cpu on the CPU of the QAT
// interrupt (the busiest_interrupt_cpu of qat-floor) and --observer-cpu on its
// SMT sibling. After the submit, the filler CPU loads the completion byte and
// reads the TSC in a tight loop. A step between two TSC reads longer than
// kernel_gap_threshold_ns is a gap: the CPU was in the kernel. The last gap
// before the filler CPU sees the byte is the QAT interrupt: it starts when
// the interrupt stops the loop and ends when the kernel returns to it, and the
// kernel writes the byte inside it. Each run is split in four parts:
//   device_and_delivery  submit end to the gap start: the rest of the copy to
//                        the device, the device, its interrupt coalescing
//                        timer and the MSI-X delivery to the CPU. The device
//                        gives no time stamp, so these do not split.
//   entry_to_seen        the gap start to the observer seeing the byte: the
//                        interrupt handler, the tasklet and the crypto and AIO
//                        callbacks to the io_event write, plus the observer lag
//                        (signal-floor from this CPU to the observer CPU)
//   seen_to_return       the observer seeing the byte to the gap end: the rest
//                        of the kernel path, minus the observer lag
//   gap                  the gap length: the CPU time that the completion takes
//                        from the program on the interrupt CPU
// The other gaps (before the last gap of a run) are mostly timer ticks: their
// length is the cost of an interrupt with a short handler, for comparison.
// The split counts only runs where the observer sees the byte inside the last
// gap. The loop loads the byte before it reads the TSC, and tests the step
// before it stops, so a kernel entry between any two instructions of the loop
// is a gap.
static void measure_qat_interrupt(const Options &options, const TscClock &clock,
                                  QatDecryptOffload &offload, CompletionObserver &observer) {
  std::vector<double> submits_ns;
  std::vector<double> offloads_ns;
  std::vector<double> device_and_deliveries_ns;
  std::vector<double> entries_to_seen_ns;
  std::vector<double> seens_to_return_ns;
  std::vector<double> gaps_ns;
  int failed_offloads = 0;
  int completed_during_submit_count = 0;
  int runs_without_gap = 0;
  int seen_outside_gap_count = 0;
  std::vector<double> other_gaps_ns;  // gaps before the last gap of a run
  uint64_t idle_ticks = static_cast<uint64_t>(clock.ns_to_ticks(options.idle_us * 1000));
  uint64_t kernel_gap_threshold_ticks =
      static_cast<uint64_t>(clock.ns_to_ticks(kernel_gap_threshold_ns));
  auto ns_between = [&clock](uint64_t from_tsc, uint64_t to_tsc) {
    return clock.ticks_to_ns(static_cast<double>(static_cast<int64_t>(to_tsc - from_tsc)));
  };
  std::map<int, uint64_t> interrupts_before;
  for (int run_index = 0; run_index < warmup_runs + options.runs; run_index++) {
    if (run_index == warmup_runs) {
      interrupts_before = read_device_interrupts_per_cpu(QatDecryptOffload::interrupt_name);
    }
    volatile uint8_t *completion_byte = offload.prepare();
    observer.watch(completion_byte);
    uint64_t idle_start_tsc = read_tsc();
    while (read_tsc() - idle_start_tsc < idle_ticks) {
    }
    uint64_t submit_start_tsc = read_tsc();
    offload.submit();
    uint64_t submit_end_tsc = read_tsc();
    bool completed_during_submit = *completion_byte != 0;
    uint64_t previous_tsc = read_tsc();
    uint64_t last_gap_start_tsc = 0;
    uint64_t last_gap_end_tsc = 0;
    int gap_count = 0;
    std::vector<double> gaps_of_run_ns;  // empty unless a run has more than one gap
    for (;;) {
      bool completed = *completion_byte != 0;
      uint64_t now_tsc = read_tsc();
      if (now_tsc - previous_tsc > kernel_gap_threshold_ticks) {
        if (gap_count > 0) {
          gaps_of_run_ns.push_back(ns_between(last_gap_start_tsc, last_gap_end_tsc));
        }
        last_gap_start_tsc = previous_tsc;
        last_gap_end_tsc = now_tsc;
        gap_count++;
      }
      previous_tsc = now_tsc;
      if (completed) {
        break;
      }
    }
    bool offload_output_correct = offload.reap();
    uint64_t completion_seen_tsc = observer.wait_for_completion_tsc();
    if (run_index < warmup_runs) {
      continue;
    }
    if (!offload_output_correct) {
      failed_offloads++;
    }
    submits_ns.push_back(ns_between(submit_start_tsc, submit_end_tsc));
    offloads_ns.push_back(ns_between(submit_start_tsc, completion_seen_tsc));
    if (completed_during_submit) {
      completed_during_submit_count++;
      continue;
    }
    if (gap_count == 0) {
      runs_without_gap++;
      continue;
    }
    other_gaps_ns.insert(other_gaps_ns.end(), gaps_of_run_ns.begin(), gaps_of_run_ns.end());
    bool seen_inside_gap =
        last_gap_start_tsc <= completion_seen_tsc && completion_seen_tsc <= last_gap_end_tsc;
    if (!seen_inside_gap) {
      seen_outside_gap_count++;
      continue;
    }
    device_and_deliveries_ns.push_back(ns_between(submit_end_tsc, last_gap_start_tsc));
    entries_to_seen_ns.push_back(ns_between(last_gap_start_tsc, completion_seen_tsc));
    seens_to_return_ns.push_back(ns_between(completion_seen_tsc, last_gap_end_tsc));
    gaps_ns.push_back(ns_between(last_gap_start_tsc, last_gap_end_tsc));
  }
  std::map<int, uint64_t> interrupts_after =
      read_device_interrupts_per_cpu(QatDecryptOffload::interrupt_name);
  DeviceInterrupts interrupts = device_interrupts_between(interrupts_before, interrupts_after);
  uint64_t filler_cpu_interrupts = interrupts_after[options.cpu] - interrupts_before[options.cpu];
  // Each completion is one interrupt of the response ring. With fewer on the
  // filler CPU, the gaps are not the QAT interrupt.
  if (filler_cpu_interrupts < static_cast<uint64_t>(options.runs)) {
    fprintf(stderr,
            "qat-interrupt: CPU %d handled %" PRIu64 " QAT interrupts in %d runs; the QAT "
            "interrupts are on CPU %d. Run with --cpu %d and its SMT sibling as --observer-cpu.\n",
            options.cpu, filler_cpu_interrupts, options.runs, interrupts.busiest_cpu,
            interrupts.busiest_cpu);
    exit(1);
  }
  printf("qat-interrupt,%d,%d,%zu,%g,%d,%d,%d,%d,%d,%zu,%" PRIu64 ",%" PRIu64, options.cpu,
         options.observer_cpu, options.offload_bytes, options.idle_us, options.runs,
         failed_offloads, completed_during_submit_count, runs_without_gap,
         seen_outside_gap_count, other_gaps_ns.size(), interrupts.total, filler_cpu_interrupts);
  printf(",%.0f,%.0f", percentile(submits_ns, 0.5), percentile(offloads_ns, 0.5));
  print_median_and_tail(device_and_deliveries_ns);
  print_median_and_tail(entries_to_seen_ns);
  print_median_and_tail(seens_to_return_ns);
  print_median_and_tail(gaps_ns);
  printf(",%.0f,%.0f\n", percentile(other_gaps_ns, 0.5), percentile(other_gaps_ns, 0.9));
}

// ---------------------------------------------------------------------------
// Command line

static void print_usage(FILE *stream) {
  fprintf(stream,
          "usage: preempt_bench --mode overhead|latency|signal-floor|qat-latency|qat-floor|\n"
          "                            qat-interrupt|dsa-latency|dsa-floor|switch-floor\n"
          "                            [options]\n"
          "  --filler NAME        a filler, or all (default); repeat for more than one\n"
          "  --size BYTES         input size of each filler, a multiple of 64 (default 65536)\n"
          "  --runs N             measured runs (default 2000 overhead, 1000 latency,\n"
          "                       qat-latency and dsa-latency, 10000 signal-floor,\n"
          "                       qat-floor, qat-interrupt, dsa-floor and switch-floor)\n"
          "  --cpu N              CPU of the filler (default 2)\n"
          "  --accelerator-cpu N  CPU of the emulated accelerator (default 4)\n"
          "  --observer-cpu N     qat and dsa modes: CPU of the completion observer\n"
          "                       (default 6)\n"
          "  --offload-bytes N    qat and dsa modes: bytes of each offload (default 4096);\n"
          "                       for QAT a multiple of 16, at most 65536\n"
          "  --work-queue PATH    dsa modes: the user WQ of the DSA device (default\n"
          "                       /dev/dsa/wq0.1)\n"
          "  --idle-us N          qat-floor, qat-interrupt and dsa-floor modes: the device\n"
          "                       is idle for N us before each offload (default 0)\n"
          "  --offload-ns N       offload time; default uniform in [0, 0.9 x filler runtime]\n"
          "                       in latency mode, 1000 in signal-floor mode\n"
          "  --seed N             seed of the inputs and the offload times of each filler\n"
          "                       (default 1)\n"
          "  --samples PATH       latency, qat-latency and dsa-latency modes: also write\n"
          "                       each run to PATH\n"
          "  --switch-parts PATH  latency, qat-latency, dsa-latency and switch-floor modes:\n"
          "                       cut the switch of each yield into parts with TSC stamps,\n"
          "                       and write one row of part medians a filler to PATH\n"
          "  --yield-jump J       the jump of do_yield to the scheduler: boost (default;\n"
          "                       jump_fcontext), own-copy (a copy of jump_fcontext that\n"
          "                       only do_yield calls) or no-fpu-control-load\n"
          "                       (jump_fcontext without ldmxcsr and fldcw)\n"
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
    observer_cpu_option,
    offload_bytes_option,
    work_queue_option,
    idle_us_option,
    offload_ns_option,
    seed_option,
    samples_option,
    switch_parts_option,
    yield_jump_option,
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
      {"observer-cpu", required_argument, nullptr, observer_cpu_option},
      {"offload-bytes", required_argument, nullptr, offload_bytes_option},
      {"work-queue", required_argument, nullptr, work_queue_option},
      {"idle-us", required_argument, nullptr, idle_us_option},
      {"offload-ns", required_argument, nullptr, offload_ns_option},
      {"seed", required_argument, nullptr, seed_option},
      {"samples", required_argument, nullptr, samples_option},
      {"switch-parts", required_argument, nullptr, switch_parts_option},
      {"yield-jump", required_argument, nullptr, yield_jump_option},
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
      case observer_cpu_option:
        options.observer_cpu = atoi(optarg);
        break;
      case offload_bytes_option:
        options.offload_bytes = strtoull(optarg, nullptr, 0);
        break;
      case work_queue_option:
        options.work_queue_path = optarg;
        break;
      case idle_us_option:
        options.idle_us = atof(optarg);
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
      case switch_parts_option:
        options.switch_parts_path = optarg;
        break;
      case yield_jump_option:
        if (strcmp(optarg, "boost") == 0) {
          options.yield_jump = YieldJump::boost;
        } else if (strcmp(optarg, "own-copy") == 0) {
          options.yield_jump = YieldJump::own_copy;
        } else if (strcmp(optarg, "no-fpu-control-load") == 0) {
          options.yield_jump = YieldJump::no_fpu_control_load;
        } else {
          fprintf(stderr, "--yield-jump must be boost, own-copy or no-fpu-control-load\n");
          exit(2);
        }
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
  bool qat_mode = options.mode == "qat-latency" || options.mode == "qat-floor" ||
                  options.mode == "qat-interrupt";
  bool dsa_mode = options.mode == "dsa-latency" || options.mode == "dsa-floor";
  if (options.mode != "overhead" && options.mode != "latency" && options.mode != "signal-floor" &&
      options.mode != "switch-floor" && !qat_mode && !dsa_mode) {
    print_usage(stderr);
    exit(2);
  }
  bool mode_has_switch = options.mode == "latency" || options.mode == "qat-latency" ||
                         options.mode == "dsa-latency" || options.mode == "switch-floor";
  if (!mode_has_switch &&
      (!options.switch_parts_path.empty() || options.yield_jump != YieldJump::boost)) {
    fprintf(stderr,
            "--switch-parts and --yield-jump are for latency, qat-latency, dsa-latency and "
            "switch-floor modes\n");
    exit(2);
  }
  if (options.size_bytes == 0 || options.size_bytes % 64 != 0 || options.size_bytes > (1u << 30)) {
    fprintf(stderr, "--size must be a nonzero multiple of 64, at most 1 GiB\n");
    exit(2);
  }
  if (options.offload_bytes == 0) {
    fprintf(stderr, "--offload-bytes must be nonzero\n");
    exit(2);
  }
  // AF_ALG takes at most the socket send buffer (about 212 KB) in one sendmsg.
  if (qat_mode && (options.offload_bytes % aes_block_bytes != 0 || options.offload_bytes > 65536)) {
    fprintf(stderr, "--offload-bytes of a qat mode must be a multiple of 16, at most 65536\n");
    exit(2);
  }
  if (options.ipp_code_path != "native" && options.ipp_code_path != "aesni" &&
      options.ipp_code_path != "portable-c") {
    fprintf(stderr, "--ipp-code-path must be native, aesni or portable-c\n");
    exit(2);
  }
  if (options.runs <= 0) {
    if (options.mode == "overhead") {
      options.runs = 2000;
    } else if (options.mode == "latency" || options.mode == "qat-latency" ||
               options.mode == "dsa-latency") {
      options.runs = 1000;
    } else {
      options.runs = 10000;
    }
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

// Opens the samples file and writes its header; nullptr when path is empty.
static FILE *open_samples_file(const std::string &path, const char *header) {
  if (path.empty()) {
    return nullptr;
  }
  FILE *samples = fopen(path.c_str(), "w");
  if (samples == nullptr) {
    fprintf(stderr, "cannot write %s: %s\n", path.c_str(), strerror(errno));
    exit(1);
  }
  fprintf(samples, "%s\n", header);
  return samples;
}

// The CSV header of qat-floor and dsa-floor. The interrupt column is named
// after the /proc/interrupts lines that it counts.
static std::string device_floor_header(const char *interrupt_name) {
  return std::string(
             "mode,cpu,observer_cpu,offload_bytes,idle_us,runs,failed_offloads,"
             "completed_during_submit,") +
         interrupt_name +
         "_interrupts,busiest_interrupt_cpu,busiest_interrupt_cpu_count,"
         "submit_median_ns,submit_p99_ns,"
         "offload_median_ns,offload_p10_ns,offload_p90_ns,offload_p99_ns,"
         "floor_median_ns,floor_p10_ns,floor_p90_ns,floor_p99_ns,"
         "filler_involuntary_switches,observer_involuntary_switches";
}

// The CSV header of qat-latency and dsa-latency.
static std::string device_latency_header(const char *interrupt_name) {
  return std::string(
             "mode,probe_interval,ipp_code_path,filler,size_bytes,offload_bytes,runs,"
             "yielded,finished_after_completion,finished_before_completion,"
             "wrong_outputs,failed_offloads,completed_during_submit,") +
         interrupt_name +
         "_interrupts,busiest_interrupt_cpu,busiest_interrupt_cpu_count,"
         "submit_median_ns,submit_p99_ns,offload_median_ns,offload_p99_ns,"
         "latency_median_ns,latency_p90_ns,latency_p99_ns,latency_max_ns,"
         "detect_median_ns,detect_p90_ns,detect_p99_ns,"
         "switch_median_ns,switch_p90_ns,switch_p99_ns,"
         "filler_median_ns,filler_involuntary_switches,observer_involuntary_switches";
}

template <typename DeviceOffload>
static void run_device_floor(const Options &options, const TscClock &clock,
                             DeviceOffload &offload) {
  if (options.print_header) {
    printf("%s\n", device_floor_header(DeviceOffload::interrupt_name).c_str());
  }
  CompletionObserver observer(options.observer_cpu);
  measure_device_floor(options, clock, offload, observer);
}

template <typename DeviceOffload>
static void run_device_latency(const Options &options, const TscClock &clock,
                               DeviceOffload &offload, FILE *switch_parts_file) {
  FILE *samples = open_samples_file(
      options.samples_path,
      "probe_interval,ipp_code_path,filler,size_bytes,offload_bytes,run,submit_ns,offload_ns,"
      "outcome,detect_ns,switch_ns,latency_ns,output_correct,offload_output_correct");
  if (options.print_header) {
    printf("%s\n", device_latency_header(DeviceOffload::interrupt_name).c_str());
  }
  CompletionObserver observer(options.observer_cpu);
  for (const std::string &name : options.filler_names) {
    std::mt19937_64 input_random(options.seed);
    std::unique_ptr<Filler> filler = find_filler_kind(name)->make(options.size_bytes, input_random);
    measure_device_latency(name.c_str(), *filler, options, clock, offload, observer, samples,
                           switch_parts_file);
    fflush(stdout);
  }
  if (samples != nullptr) {
    fclose(samples);
  }
}

int main(int argc, char **argv) {
  Options options = parse_options(argc, argv);
  select_ipp_code_path(options.ipp_code_path);
  pin_current_thread_to_cpu(options.cpu);
  TscClock clock = TscClock::calibrate();
  // The fillers and the fibers run on this thread, so these thread_local
  // flags are the ones do_yield reads.
  yield_jump = options.yield_jump;
  stamping_switch_parts = !options.switch_parts_path.empty();
  switch_experiment_active = stamping_switch_parts || yield_jump != YieldJump::boost;
  FILE *switch_parts_file = open_samples_file(options.switch_parts_path, switch_parts_header);

  if (options.mode == "switch-floor") {
    if (options.print_header) {
      printf(
          "mode,cpu,yield_jump,switch_parts,runs,core_ghz,switch_median_ns,switch_p10_ns,"
          "switch_p90_ns,switch_p99_ns\n");
    }
    measure_switch_floor(options, clock, switch_parts_file);
    if (switch_parts_file != nullptr) {
      fclose(switch_parts_file);
    }
    return 0;
  }

  if (options.mode == "overhead") {
    if (options.print_header) {
      printf(
          "mode,probe_interval,ipp_code_path,filler,size_bytes,runs,wrong_outputs,"
          "median_ns,p10_ns,p90_ns,probe_executions_per_run,checksum\n");
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

  if (options.mode == "signal-floor") {
    if (options.print_header) {
      printf(
          "mode,cpu,accelerator_cpu,runs,late_completions,median_ns,p10_ns,p90_ns,p99_ns,max_ns\n");
    }
    EmulatedAccelerator accelerator(options.accelerator_cpu);
    measure_signal_floor(options, clock, accelerator);
    return 0;
  }

  // The QAT instance of the kernel driver is on the NUMA node of the CPU that
  // makes the transform: the filler CPU, pinned above.
  if (options.mode == "qat-floor" || options.mode == "qat-latency") {
    std::mt19937_64 offload_random(options.seed);
    QatDecryptOffload offload(options.offload_bytes, offload_random);
    if (options.mode == "qat-floor") {
      run_device_floor(options, clock, offload);
    } else {
      run_device_latency(options, clock, offload, switch_parts_file);
    }
    if (switch_parts_file != nullptr) {
      fclose(switch_parts_file);
    }
    return 0;
  }

  if (options.mode == "qat-interrupt") {
    if (options.print_header) {
      printf(
          "mode,cpu,observer_cpu,offload_bytes,idle_us,runs,failed_offloads,completed_during_submit,"
          "runs_without_gap,seen_outside_gap,other_gaps,qat_interrupts,filler_cpu_interrupts,"
          "submit_median_ns,offload_median_ns,"
          "device_and_delivery_median_ns,device_and_delivery_p10_ns,device_and_delivery_p90_ns,"
          "device_and_delivery_p99_ns,"
          "entry_to_seen_median_ns,entry_to_seen_p10_ns,entry_to_seen_p90_ns,entry_to_seen_p99_ns,"
          "seen_to_return_median_ns,seen_to_return_p10_ns,seen_to_return_p90_ns,"
          "seen_to_return_p99_ns,"
          "gap_median_ns,gap_p10_ns,gap_p90_ns,gap_p99_ns,other_gap_median_ns,other_gap_p90_ns\n");
    }
    std::mt19937_64 offload_random(options.seed);
    QatDecryptOffload offload(options.offload_bytes, offload_random);
    CompletionObserver observer(options.observer_cpu);
    measure_qat_interrupt(options, clock, offload, observer);
    return 0;
  }

  if (options.mode == "dsa-floor" || options.mode == "dsa-latency") {
    std::mt19937_64 offload_random(options.seed);
    DsaMemmoveOffload offload(options.work_queue_path, options.offload_bytes, offload_random);
    if (options.mode == "dsa-floor") {
      run_device_floor(options, clock, offload);
    } else {
      run_device_latency(options, clock, offload, switch_parts_file);
    }
    if (switch_parts_file != nullptr) {
      fclose(switch_parts_file);
    }
    return 0;
  }

  FILE *samples = open_samples_file(
      options.samples_path,
      "probe_interval,ipp_code_path,filler,size_bytes,run,offload_ns,"
      "accelerator_lateness_ns,outcome,detect_ns,switch_ns,latency_ns,output_correct");
  if (options.print_header) {
    printf(
        "mode,probe_interval,ipp_code_path,filler,size_bytes,runs,"
        "yielded,finished_after_completion,finished_before_completion,"
        "late_completions,wrong_outputs,"
        "latency_median_ns,latency_p90_ns,latency_p99_ns,latency_max_ns,"
        "detect_median_ns,detect_p90_ns,detect_p99_ns,"
        "switch_median_ns,switch_p90_ns,switch_p99_ns,"
        "filler_median_ns\n");
  }
  EmulatedAccelerator accelerator(options.accelerator_cpu);
  for (const std::string &name : options.filler_names) {
    std::mt19937_64 input_random(options.seed);
    std::unique_ptr<Filler> filler = find_filler_kind(name)->make(options.size_bytes, input_random);
    std::mt19937_64 offload_random(options.seed);
    measure_latency(name.c_str(), *filler, options, clock, accelerator, offload_random, samples,
                    switch_parts_file);
    fflush(stdout);
  }
  if (samples != nullptr) {
    fclose(samples);
  }
  if (switch_parts_file != nullptr) {
    fclose(switch_parts_file);
  }
  return 0;
}
