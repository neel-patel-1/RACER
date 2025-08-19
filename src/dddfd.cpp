#include "numa_mem.h"
#include "payload_gen.h"
#include "print_utils.h"
#include "stats.h"
#include "pointer_chase.h"
#include "iaa_offloads.h"
#include "dsa_offloads.h"
#include <x86intrin.h>
#include <immintrin.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include "src/protobuf/generated/src/protobuf/router.pb.h"
#include "ch3_hash.h"
#include "gather.h"
#include "matmul_histogram.h"
#include "payload_gen.h"
#include "gpcore_compress.h"
#include "lzdatagen.h"
#include "decrypt.h"
#include "dotproduct.h"
#include "test.h"
#include "gather.h"
#include "pointer_chase.h"
#include "timer_utils.h"
#include "iaa_offloads.h"
#include "dsa_offloads.h"
#include <x86intrin.h>
#include <immintrin.h>
#include "racer_opts.h"
#include "gather.h"
#include <atomic>
#include <cmath>
extern "C" {
  #include "fcontext.h"
  #include "idxd.h"
  #include "accel_test.h"
}

#define COMP_STATUS_COMPLETED 1
#define COMP_STATUS_PENDING 0
#define REQUEST_STATUS_NONE -1
#define REQUEST_COMPLETED COMP_STATUS_COMPLETED
#define REQUEST_PREEMPTED COMP_STATUS_PENDING
#define REQUEST_YIELDED 2
typedef struct _request_return_args{
  int status;
} request_return_args_t;

typedef struct hw_desc idxd_desc;
typedef struct completion_record idxd_comp;

extern struct acctest_context *iaa;

std::atomic<bool> do_demote = false;
__thread volatile uint8_t *preempt_signal;
__thread IppsAES_GCMState *pState = NULL;

__thread request_return_args_t ret_args;
__thread fcontext_transfer_t scheduler_xfer;

extern "C" void do_yield()
{
  LOG_PRINT(LOG_DEBUG, "Yielding\n");
  ret_args.status = REQUEST_PREEMPTED;
  scheduler_xfer = fcontext_swap(scheduler_xfer.prev_context, NULL);
}

int
cpu_pin(uint32_t cpu)
{
	cpu_set_t *cpuset;
	size_t cpusetsize;

	cpusetsize = CPU_ALLOC_SIZE(get_nprocs());
	cpuset = CPU_ALLOC(get_nprocs());
	CPU_ZERO_S(cpusetsize, cpuset);
	CPU_SET_S(cpu, cpusetsize, cpuset);

	pthread_setaffinity_np(pthread_self(), cpusetsize, cpuset);

	CPU_FREE(cpuset);

	return 0;
}

static __always_inline uint64_t
rdtsc(void)
{
	uint64_t tsc;
	unsigned int dummy;

	/*
	 * https://www.felixcloutier.com/x86/rdtscp
	 * The RDTSCP instruction is not a serializing instruction, but it
	 * does wait until all previous instructions have executed and all
	 * previous loads are globally visible
	 *
	 * If software requires RDTSCP to be executed prior to execution of
	 * any subsequent instruction (including any memory accesses), it can
	 * execute LFENCE immediately after RDTSCP
	 */
  _mm_mfence();
	tsc = __rdtscp(&dummy);
  _mm_mfence();
	// __builtin_ia32_lfence();

	return tsc;
}

static inline void decrypt_feature(void *cipher_inp, void *plain_out, int input_size){
  Ipp8u *pKey = (Ipp8u *)"0123456789abcdef";
  Ipp8u *pIV = (Ipp8u *)"0123456789ab";
  int keysize = 16;
  int ivsize = 12;
  int aadSize = 16;
  Ipp8u aad[aadSize];
  IppStatus status;

  status = ippsAES_GCMReset(pState);
  if(status != ippStsNoErr){
    LOG_PRINT(LOG_ERR, "Failed to reset AES GCM\n");
  }
  status = ippsAES_GCMStart(pIV, ivsize, aad, aadSize, pState);
  if(status != ippStsNoErr){
    LOG_PRINT(LOG_ERR, "Failed to start AES GCM\n");
  }
  status = ippsAES_GCMDecrypt((Ipp8u *)cipher_inp, (Ipp8u *)plain_out, input_size, pState);
  if(status != ippStsNoErr){
    LOG_PRINT(LOG_ERR, "Failed to decrypt AES GCM: %d\n", status);
  }

  LOG_PRINT(LOG_TOO_VERBOSE, "Decrypted: %x\n", *(uint32_t *)plain_out);
}


static __always_inline void flush_range(void *start, size_t len)
{
  char *ptr = (char *)start;
  char *end = ptr + len;

  for (; ptr < end; ptr += 64) {
    _mm_clflush(ptr);
  }
}

static __always_inline void demote_buf(char *buf, int size){
  for(int i = 0; i < size; i+=64){
    _cldemote((void *)&buf[i]);
  }

}

static __always_inline void write_to_buf(char *buf, int size){
  for(int i = 0; i < size; i+=64){
    ((volatile char *)(buf))[i] = 'a';
  }
}

static __always_inline void prefault_pages(void *buf, int size){
  volatile char *ptr = (char *)buf;
  for(int i = 0; i < size; i+=4096){
    ptr[i] = 'a'; // Touch each page to ensure it's faulted in
  }
}

void gen_ser_buf(int avail_out, char *p_val, char *dst, int *msg_size, int insize){
  router::RouterRequest req;

  req.set_key("/region/cluster/foo:key|#|etc"); // key is 32B string, value gets bigger up to 2MB
  req.set_operation(0);

  std::string valstring(p_val, insize);

  req.set_value(valstring);

  *msg_size = req.ByteSizeLong();
  if(*msg_size > avail_out){
    throw std::runtime_error("Not enough space to serialize");
    return;
  }
  req.SerializeToArray(dst, *msg_size);
}

void gen_comp_buf(int des_psize, int avail_out, void *compbuf, int *outsize, double target_ratio){
  double len_exp = 3.0; // linear distribution of lengths
  double lit_exp = 3.0; // linear distribution of literals

  char *valbuf;
  uint64_t msgsize;
  int compsize = avail_out;
  int rc;

  int req_uncompsize = des_psize * target_ratio;
  valbuf = (char *)malloc(req_uncompsize);

  lzdg_generate_reuse_buffers((void *)valbuf, req_uncompsize, target_ratio, len_exp, lit_exp);

  /* get compress bound*/
  rc = gpcore_do_compress(compbuf, (void *)valbuf, req_uncompsize, &compsize);
  if(rc != Z_OK){
    LOG_PRINT(LOG_ERR, "Compression failed\n");
    return;
  }
  LOG_PRINT(LOG_DEBUG, "CompSize: %d\n", compsize);

  *outsize = compsize;
}

void gen_ser_comp_payload(
  void *out, /* the output buffer */
  int des_psize, /*how big we want the payload */
  int max_comp_size, /* how big to alloccate for the comp buf */
  int avail_ser_out, /* how much space is available in the output */
  int *outsize, /* how big the serialized payload is */
  double target_ratio){

  void *comp_buf = (void *)malloc(max_comp_size);
  int comp_size;
  int ser_size;

  gen_comp_buf(des_psize, max_comp_size, comp_buf, &comp_size, target_ratio);

  gen_ser_buf(avail_ser_out, (char *)comp_buf, (char *)out, &ser_size, comp_size);
  LOG_PRINT(LOG_DEBUG, "Serialized Payload Size: %d\n", ser_size);

  *outsize = ser_size;

  free(comp_buf);
}

void gpcore_do_extract(uint8_t *inp, uint8_t *outp, int low_val, int high_val, uint8_t *aecs){
  uint32_t num_inputs = high_val - low_val;

  uint32_t iaa_filter_flags = 28;

  struct iaa_filter_aecs_t iaa_filter_aecs =
  {
    .rsvd = 0,
    .rsvd2 = 0,
    .rsvd3 = 0,
    .rsvd4 = 0,
    .rsvd5 = 0,
    .rsvd6 = 0
  };

  /* prepare aecs */
  memset(aecs, 0, IAA_FILTER_AECS_SIZE);
  iaa_filter_aecs.low_filter_param = low_val;
  iaa_filter_aecs.high_filter_param = high_val;
  memcpy(aecs, (void *)&iaa_filter_aecs, IAA_FILTER_AECS_SIZE);

  iaa_do_extract(outp, inp, (void *)aecs, num_inputs, iaa_filter_flags);
}

bool gDebugParam = false;
int *glob_indir_arr = NULL; // TODO
int num_accesses = 0; // TODO

int main(int argc, char **argv){

  uint64_t start, end;

  int iaa_wq_id = 0;
  int wq_type = SHARED;
  int iaa_dev_id = 1;

  struct numa_mem  *nm = NULL;
  int nb_numa_node = 1;
  int node = 0;

  int rc;
  uint64_t iter = 1;
  idxd_desc *desc = NULL;
  idxd_comp *comp = NULL;
  int pg_size = 1024 * 1024 * 1024;
  int p_off = 0;
  char *srcs = NULL;
  char *dsts = NULL;
  char *dsts_2 = NULL;
  int batch_size = 64;

  char *tmp_plain_buf = NULL;
  uint64_t src_buf_space;

  uint8_t *aecs = NULL;

  get_opts(argc, argv);

  double target_ratio = 3.0;
  uint64_t decomp_size = buf_size * target_ratio;
  uint64_t decomp_out_space = IAA_DECOMPRESS_MAX_DEST_SIZE;
  uint64_t max_comp_size = get_compress_bound(decomp_size);
  uint64_t max_payload_expansion = max_comp_size + 64;

  uint64_t phase1_array_start[total_requests];
  uint64_t phase1_array_end[total_requests];

  uint64_t demote1_array_start[total_requests];
  uint64_t demote1_array_end[total_requests];
  uint64_t phase2_array_start[total_requests];
  uint64_t phase2_array_end[total_requests];

  uint64_t pref1_array_start[total_requests];
  uint64_t pref1_array_end[total_requests];
  uint64_t phase3_array_start[total_requests];
  uint64_t phase3_array_end[total_requests];

  uint64_t demote2_array_start[total_requests];
  uint64_t demote2_array_end[total_requests];
  uint64_t phase4_array_start[total_requests];
  uint64_t phase4_array_end[total_requests];

  uint64_t pref2_array_start[total_requests];
  uint64_t pref2_array_end[total_requests];
  uint64_t phase5_array_start[total_requests];
  uint64_t phase5_array_end[total_requests];

  memset(demote1_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(demote1_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(pref1_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(pref1_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(phase1_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(phase1_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(phase2_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(phase2_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(phase3_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(phase3_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(demote2_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(demote2_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(phase4_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(phase4_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(pref2_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(pref2_array_start, 0, sizeof(uint64_t) * total_requests);
  memset(phase5_array_end, 0, sizeof(uint64_t) * total_requests);
  memset(phase5_array_start, 0, sizeof(uint64_t) * total_requests);


  uint64_t diffs[total_requests];
  uint64_t start_times[iter];
  uint64_t end_times[iter];

  uint64_t bytes = batch_size;

  int ippAES_GCM_ctx_size;
  IppStatus status;

  status = ippsAES_GCMGetSize(&ippAES_GCM_ctx_size);
  if(status != ippStsNoErr){
    LOG_PRINT(LOG_ERR, "Failed to get AES GCM size\n");
  }

  if(pState != NULL){
    free(pState);
  }
  pState = (IppsAES_GCMState *)malloc(ippAES_GCM_ctx_size);
  int keysize = 16;
  int ivsize = 12;
  int aadSize = 16;
  int taglen = 16;
  Ipp8u *pKey = (Ipp8u *)"0123456789abcdef";
  Ipp8u *pIV = (Ipp8u *)"0123456789ab";
  Ipp8u *pAAD = (Ipp8u *)malloc(aadSize);
  Ipp8u *pTag = (Ipp8u *)malloc(taglen);
  status = ippsAES_GCMInit(pKey, keysize, pState, ippAES_GCM_ctx_size);
  if(status != ippStsNoErr){
    LOG_PRINT(LOG_ERR, "Failed to init AES GCM\n");
  }

  struct completion_record *dummy = (struct completion_record *)malloc(sizeof(struct completion_record));
  dummy->status = IAX_COMP_NONE;
  preempt_signal = (uint8_t *)dummy;

  nm = (struct numa_mem *)calloc(nb_numa_node, sizeof(struct numa_mem));
  desc = (idxd_desc *)alloc_numa_offset(nm, sizeof(idxd_desc) * total_requests, 0);
  comp = (idxd_comp *)alloc_numa_offset(nm, sizeof(idxd_comp) * total_requests, 0);
  srcs = (char *)alloc_numa_offset(nm, max_payload_expansion * total_requests, 0);
  dsts = (char *)alloc_numa_offset(nm, total_requests * IAA_DECOMPRESS_MAX_DEST_SIZE, 0);
  dsts_2 = (char *)alloc_numa_offset(nm, total_requests * IAA_DECOMPRESS_MAX_DEST_SIZE, 0);
  aecs = (uint8_t *)alloc_numa_offset(nm, IAA_FILTER_AECS_SIZE * 2 * total_requests, 0);


  rc = alloc_numa_mem(nm, pg_size, node);
  if(rc){
    LOG_PRINT(LOG_ERR,"Failed to allocate memory\n");
    return -1;
  }
  add_base_addr(nm, (void **)&srcs);
  add_base_addr(nm, (void **)&dsts);
  add_base_addr(nm, (void **)&dsts_2);
  add_base_addr(nm, (void **)&desc);
  add_base_addr(nm, (void **)&comp);
  add_base_addr(nm, (void **)&aecs);
  memset(comp, 0, sizeof(idxd_comp) * total_requests);
  memset(desc, 0, sizeof(idxd_desc) * total_requests);
  initialize_iaa_wq(iaa_dev_id, iaa_wq_id, wq_type);

  /* pin to SMT threads on same core */
  cpu_pin(3);

  uint64_t buf_offset = 0;

  int avail_out;
  gen_ser_comp_payload((char *)srcs, buf_size,
    max_comp_size, max_payload_expansion,
    &avail_out, target_ratio);
  LOG_PRINT(LOG_DEBUG, "ser'd bufsize: %d\n", avail_out);

  for(int i=1; i<total_requests; i++){
    memcpy((void *)&srcs[i * max_payload_expansion], (void *)srcs, avail_out);
  }

  uLong avDSz = IAA_DECOMPRESS_MAX_DEST_SIZE;

  flush_range((void *)srcs, max_payload_expansion * total_requests);
  flush_range((void *)dsts, IAA_DECOMPRESS_MAX_DEST_SIZE * total_requests); /* flush decrypt dsts */
  prefault_pages((void *)dsts_2, IAA_DECOMPRESS_MAX_DEST_SIZE * total_requests); /* flush source, write prefault dsat dsts */

  for(int i=0; i<total_requests; i++){
    uint8_t *src = (uint8_t *)&srcs[i * max_payload_expansion];
    uint8_t *dst = (uint8_t *)&dsts[i * IAA_DECOMPRESS_MAX_DEST_SIZE];
    uint8_t *filter_dst = (uint8_t *)&dsts_2[i * IAA_DECOMPRESS_MAX_DEST_SIZE];

    uint8_t *m_aecs = (uint8_t *)&aecs[i * IAA_FILTER_AECS_SIZE * 2];

    idxd_comp *m_comp = &comp[i];
    idxd_desc *m_desc = &desc[i];
    router::RouterRequest req;
    p_off = (p_off + 64) % 4096;

    /* Core Phase 1*/
    #ifdef EXETIME
    start = rdtsc();
    #endif

    req.ParseFromArray((void *)src, avail_out);

    #ifdef EXETIME
    end = rdtsc();
    #endif

    #ifdef EXETIME
    phase1_array_start[i] = start;
    phase1_array_end[i] = end;
    #endif

    /* Acc Phase 1*/
    if(sync_demote){
      #ifdef EXETIME
      start = rdtsc();
      #endif
      demote_buf((char *)&(req.value()[0]), decomp_size);
      #ifdef EXETIME
      end = rdtsc();
      demote1_array_end[i] = end;
      demote1_array_start[i] = start;
      #endif
    } else {
      #ifdef EXETIME
      demote1_array_end[i] = 0;
      demote1_array_start[i] = 0;
      #endif
    }

    if(!noAcc){
      /* offload memcpy to dsa */
      prepare_iaa_decompress_desc_with_preallocated_comp(
        m_desc, (uint64_t)(&(req.value())[0]), (uint64_t)dst,
        (uint64_t)(m_comp), req.value().size());
      while(enqcmd((void *)((char *)(iaa->wq_reg) + p_off), m_desc) ){
        /* retry submit */
      }

      #ifdef EXETIME
      start = rdtsc();
      #endif
      while(m_comp->status == 0){

      }
      #ifdef EXETIME
      end = rdtsc();
      #endif
      if(m_comp->status != IAX_COMP_SUCCESS){
        LOG_PRINT(LOG_ERR,"Failed to copy: %x\n", comp[i].status);
        return -1;
      }
      if( ((struct completion_record *)m_comp) ->iax_output_size != decomp_size){
        LOG_PRINT(LOG_ERR, "Decompression failed: %u\n", ((struct completion_record *)m_comp) ->iax_output_size);
        return -1;
      }
    }
    else {
      #ifdef EXETIME
      start = rdtsc();
      #endif
      gpcore_do_decompress(
        (void *)dst,
        (void *)&(req.value()[0]),
        req.value().size(),
        &avDSz
      );
      #ifdef EXETIME
      end = rdtsc();
      #endif
      if(avDSz != decomp_size){
        LOG_PRINT(LOG_ERR, "Decompression failed: %lu\n", avDSz);
        return -1;
      }
    }

    #ifdef EXETIME
    phase2_array_start[i] = start;
    phase2_array_end[i] = end;
    #endif


    if(sync_prefetch){
      LOG_PRINT(LOG_TOO_VERBOSE, "Sync prefetch real buffers\n");
      char *fetch_buf = (char *)dst;
      #ifdef EXETIME
      start = rdtsc();
      #endif
      for(int j=0; j<decomp_size; j+=64){
        _mm_prefetch((void *)(fetch_buf + j), _MM_HINT_T1);
      }
      #ifdef EXETIME
      end = rdtsc();
      pref1_array_start[i] = start;
      pref1_array_end[i] = end;
    #endif
    }

    #ifdef EXETIME
    start = rdtsc();
    #endif
    uint32_t hash;
    LOG_PRINT(LOG_DEBUG, "Please fetch: %lx\n", (uintptr_t)dst);
    #ifdef EXETIME
    end = rdtsc();
    #endif
    #ifdef EXETIME
    phase3_array_start[i] = start;
    phase3_array_end[i] = end;
    #endif

    /* Acc Phase 2*/
    if (sync_demote){
      #ifdef EXETIME
      start = rdtsc();
      #endif
      demote_buf((char *)dst, decomp_size);
      #ifdef EXETIME
      end = rdtsc();
      demote2_array_start[i] = start;
      demote2_array_end[i] = end;
      #endif
    } else {
      #ifdef EXETIME
      demote2_array_start[i] = 0;
      demote2_array_end[i] = 0;
      #endif
    }

    if(!noAcc){
      prepare_iaa_filter_desc_with_preallocated_comp(
        m_desc, (uint64_t)dst, (uint64_t)filter_dst, (uint64_t)m_comp, decomp_size);
      while(enqcmd((void *)((char *)(iaa->wq_reg) + p_off), m_desc)){
      }
      #ifdef EXETIME
      start = rdtsc();
      #endif
      while(m_comp->status == 0){
        /* wait for completion */
      }
      #ifdef EXETIME
      end = rdtsc();
      #endif
      if(m_comp->status != IAX_COMP_SUCCESS){
        LOG_PRINT(LOG_ERR, "Failed to filter: %x\n", m_comp->status);
        return -1;
      }
    } else {
      #ifdef EXETIME
      start = rdtsc();
      #endif
      gpcore_do_extract(
        dst,
        filter_dst,
        0,
        decomp_size,
        m_aecs
      );
      #ifdef EXETIME
      end = rdtsc();
      #endif
    }
    #ifdef EXETIME
    phase4_array_start[i] = start;
    phase4_array_end[i] = end;
    #endif

    if(sync_prefetch){
      LOG_PRINT(LOG_TOO_VERBOSE, "Sync prefetch filtered buffers\n");
      char *fetch_buf = (char *)filter_dst;
      #ifdef EXETIME
      start = rdtsc();
      #endif
      for(int j=0; j<decomp_size; j+=64){
        _mm_prefetch((void *)(fetch_buf + j), _MM_HINT_T1);
      }
      #ifdef EXETIME
      end = rdtsc();
      pref2_array_start[i] = start;
      pref2_array_end[i] = end;
      #endif
    }

    float score;
    int sz;
    #ifdef EXETIME
    start = rdtsc();
    #endif
    dotproduct((void *)filter_dst, (void *)&score, buf_size/2, &sz);

    #ifdef EXETIME
    end = rdtsc();
    phase5_array_end[i] = end;
    phase5_array_start[i] = start;
    #endif

  }

  #ifdef EXETIME
  uint64_t phase1Avg;
  uint64_t MemcpyAvg;
  uint64_t DemoteAvg;
  uint64_t SwPrftchAvg;
  uint64_t DotProdAvg;
  uint64_t Demote2Avg;
  uint64_t phase4Avg;
  uint64_t Prefetch2Avg;
  uint64_t phase5Avg;
  avg_samples_from_arrays(diffs,phase1Avg, phase1_array_end, phase1_array_start, total_requests);
  avg_samples_from_arrays(diffs,MemcpyAvg, phase2_array_end, phase2_array_start, total_requests);
  avg_samples_from_arrays(diffs,DemoteAvg, demote1_array_end, demote1_array_start, total_requests);
  avg_samples_from_arrays(diffs,SwPrftchAvg, pref1_array_end, pref1_array_start, total_requests);
  avg_samples_from_arrays(diffs,DotProdAvg, phase3_array_end, phase3_array_start, total_requests);
  avg_samples_from_arrays(diffs,Demote2Avg, demote2_array_end, demote2_array_start, total_requests);
  avg_samples_from_arrays(diffs,phase4Avg, phase4_array_end, phase4_array_start, total_requests);
  avg_samples_from_arrays(diffs,Prefetch2Avg, pref2_array_end, pref2_array_start, total_requests);
  avg_samples_from_arrays(diffs,phase5Avg, phase5_array_end, phase5_array_start, total_requests);


  PRINT("%lu\n%lu\n%lu\n%lu\n%lu\n%lu\n%lu\n%lu\n%lu\n",phase1Avg, DemoteAvg, MemcpyAvg, SwPrftchAvg, DotProdAvg, Demote2Avg, phase4Avg, Prefetch2Avg, phase5Avg);
  #endif


  return 0;
}