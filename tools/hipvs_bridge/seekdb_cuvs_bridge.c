/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Build/search knobs. Defaults are the contest-tuned values; the environment
 * overrides exist so a parameter sweep needs only one compile. */
typedef struct {
  enum cuvsCagraGraphBuildAlgo build_algo;
  size_t graph_degree;
  size_t intermediate_graph_degree;
  size_t nn_descent_niter;
  size_t itopk_factor;
  size_t itopk_min;
  size_t search_width;
} sk_cuvs_conf;

static size_t sk_env_size(const char *name, size_t def)
{
  const char *v = getenv(name);
  if (!v || !*v) return def;
  char *end = 0;
  unsigned long long x = strtoull(v, &end, 10);
  return (end == v || x == 0) ? def : (size_t)x;
}

static void sk_cuvs_conf_load(sk_cuvs_conf *c)
{
  c->build_algo = (enum cuvsCagraGraphBuildAlgo)sk_env_size("SEEKDB_CAGRA_BUILD_ALGO", NN_DESCENT);
  c->graph_degree = sk_env_size("SEEKDB_CAGRA_DEGREE", 64);
  c->intermediate_graph_degree = sk_env_size("SEEKDB_CAGRA_INTERMEDIATE", 128);
  c->nn_descent_niter = sk_env_size("SEEKDB_CAGRA_NITER", 20);
  c->itopk_factor = sk_env_size("SEEKDB_CAGRA_ITOPK_FACTOR", 4);
  c->itopk_min = sk_env_size("SEEKDB_CAGRA_ITOPK_MIN", 512);
  c->search_width = sk_env_size("SEEKDB_CAGRA_WIDTH", 4);
}

static int sk_trace(void) { static int on = -1; if (on < 0) { const char *v = getenv("SEEKDB_CAGRA_TRACE"); on = (v && *v && strcmp(v, "0") != 0); } return on; }

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  long graph_degree;
  /* Device scratch handed to cuvsCagraIndexFromArgs(). Kept alive for the handle's
   * lifetime: whether cuVS copies or references the tensors is version dependent,
   * and freeing early would risk a dangling dataset. */
  void  *import_graph;  size_t import_graph_bytes;
  float *import_dataset; size_t import_dataset_bytes;
  /* Reusable search scratch: one RMM alloc/free and one params object per
   * (handle, capacity) instead of per query. */
  cuvsCagraSearchParams_t sp;
  size_t itopk;
  float *qd;  size_t qcap;
  unsigned *nd; size_t ocap;
  float *dd;
} sk_cuvs_index;

static void sk_cuvs_index_reset_scratch(sk_cuvs_index *h)
{
  if (h->dd && h->ocap) { cuvsRMMFree(h->res, h->dd, sizeof(float) * h->ocap); }
  if (h->nd && h->ocap) { cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * h->ocap); }
  if (h->qd && h->qcap) { cuvsRMMFree(h->res, h->qd, sizeof(float) * h->qcap); }
  if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); h->sp = 0; }
  h->qd = 0; h->nd = 0; h->dd = 0; h->qcap = 0; h->ocap = 0; h->itopk = 0;
}

static int sk_cuvs_prepare_search(sk_cuvs_index *h, long nq, long topk)
{
  sk_cuvs_conf c;
  sk_cuvs_conf_load(&c);
  size_t itopk = (size_t)topk * c.itopk_factor;
  if (itopk < c.itopk_min) itopk = c.itopk_min;
  const size_t qcap = sizeof(float) * (size_t)nq * (size_t)h->dim;
  const size_t ocap = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  const size_t dcap = sizeof(float) * (size_t)nq * (size_t)topk;
  if (h->sp == 0 || h->itopk != itopk) {
    if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); h->sp = 0; }
    if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { return 20; }
    h->sp->itopk_size = itopk;
    h->sp->search_width = c.search_width;
    h->itopk = itopk;
  }
  if (qcap > h->qcap) {
    if (h->qd) cuvsRMMFree(h->res, h->qd, h->qcap);
    h->qd = 0; h->qcap = 0;
    if (cuvsRMMAlloc(h->res, (void **)&h->qd, qcap) != CUVS_SUCCESS) { return 21; }
    h->qcap = qcap;
  }
  if (ocap > h->ocap || dcap > h->ocap) {
    const size_t want = ocap > dcap ? ocap : dcap;
    if (h->nd) cuvsRMMFree(h->res, h->nd, h->ocap);
    if (h->dd) cuvsRMMFree(h->res, h->dd, h->ocap);
    h->nd = 0; h->dd = 0; h->ocap = 0;
    if (cuvsRMMAlloc(h->res, (void **)&h->nd, want) != CUVS_SUCCESS) { return 22; }
    if (cuvsRMMAlloc(h->res, (void **)&h->dd, want) != CUVS_SUCCESS) { return 23; }
    h->ocap = want;
  }
  return 0;
}

static void sk_fill_tensor(DLManagedTensor *t, int64_t *shape, void *data, int device,
                           long rows, long cols, int code, int bits)
{
  shape[0] = (int64_t)rows; shape[1] = (int64_t)cols;
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = device;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = code;
  t->dl_tensor.dtype.bits = bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = shape;
  t->dl_tensor.strides = 0;
  t->dl_tensor.byte_offset = 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  sk_cuvs_conf c;
  int64_t dshape[2];
  DLManagedTensor dset;
  if (!h) return NULL;
  sk_cuvs_conf_load(&c);
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  sk_fill_tensor(&dset, dshape, (void*)base, kDLCPU, n, dim, kDLFloat, 32);

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = c.build_algo;
  h->ip->graph_degree = c.graph_degree;
  h->ip->intermediate_graph_degree = c.intermediate_graph_degree;
  h->ip->nn_descent_niter = c.nn_descent_niter;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (sk_trace()) {
    fprintf(stderr, "[seekdb_cuvs] build n=%ld dim=%ld algo=%d degree=%zu inter=%zu niter=%zu\n",
            n, dim, (int)c.build_algo, c.graph_degree, c.intermediate_graph_degree, c.nn_descent_niter);
  }
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t deg = 0;
    if (cuvsCagraIndexGetGraphDegree(h->idx, &deg) == CUVS_SUCCESS && deg > 0) {
      h->graph_degree = (long)deg;
    } else {
      h->graph_degree = (long)c.graph_degree;
    }
  }
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  int rc = 0;
  int64_t qshape[2], nshape[2], dshape[2];
  DLManagedTensor qt, nt, dt;
  cuvsFilter f;
  size_t qbytes = 0, obytes = 0;
  if (!h) return 1;
  if (nq <= 0 || topk <= 0 || topk > h->n) return 2;
  qbytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
  obytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  if ((rc = sk_cuvs_prepare_search(h, nq, topk)) != 0) return rc;
  /* The adaptor serializes every search of one handle under its registry mutex,
   * so the cached scratch needs no lock of its own. */
  sk_fill_tensor(&qt, qshape, h->qd, kDLCUDA, nq, h->dim, kDLFloat, 32);
  sk_fill_tensor(&nt, nshape, h->nd, kDLCUDA, nq, topk, kDLUInt, 32);
  sk_fill_tensor(&dt, dshape, h->dd, kDLCUDA, nq, topk, kDLFloat, 32);
  if (cudaMemcpy(h->qd, query, qbytes, cudaMemcpyDefault) != cudaSuccess) { rc = 13; }
  else {
    f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; }
    else if (cudaMemcpy(out_ids, h->nd, obytes, cudaMemcpyDefault) != cudaSuccess) { rc = 16; }
    else if (out_dist && cudaMemcpy(out_dist, h->dd, obytes, cudaMemcpyDefault) != cudaSuccess) { rc = 17; }
  }
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_cuvs_index_reset_scratch(h);
  if (h->import_dataset && h->import_dataset_bytes) {
    cuvsRMMFree(h->res, h->import_dataset, h->import_dataset_bytes);
  }
  if (h->import_graph && h->import_graph_bytes) {
    cuvsRMMFree(h->res, h->import_graph, h->import_graph_bytes);
  }
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

long seekdb_cuvs_graph_info(void* handle, long* n, long* dim, long* degree)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || h->n <= 0 || h->dim <= 0 || h->graph_degree <= 0) return -1;
  if (n) *n = h->n;
  if (dim) *dim = h->dim;
  if (degree) *degree = h->graph_degree;
  return h->n * h->graph_degree * (long)sizeof(unsigned);
}

/* Copy the CAGRA knn graph (n x degree, uint32 device tensor) into a host buffer.
 * The dataset travels separately in seekdb's own snapshot container, so the graph
 * is the only cuVS-owned state that has to be persisted. */
int seekdb_cuvs_export_graph(void* handle, void* dst, long dst_len)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  DLManagedTensor graph;
  long want = 0;
  int rc = 0;
  if (!h || !dst) return 1;
  want = h->n * h->graph_degree * (long)sizeof(unsigned);
  if (want <= 0 || dst_len < want) return 2;
  memset(&graph, 0, sizeof(graph));
  if (cuvsCagraIndexGetGraph(h->idx, &graph) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] get graph failed: %s\n", cuvsGetLastErrorText());
    return 3;
  }
  if (graph.dl_tensor.data == 0 ||
      (long)(graph.dl_tensor.shape[0] * graph.dl_tensor.shape[1] * sizeof(unsigned)) != want) {
    rc = 4;
  } else if (cudaMemcpy(dst, graph.dl_tensor.data, (size_t)want, cudaMemcpyDeviceToHost) != cudaSuccess) {
    rc = 5;
  }
  if (graph.deleter) graph.deleter(&graph);
  if (rc) fprintf(stderr, "[seekdb_cuvs] export graph rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

/* Rebuild a searchable CAGRA index from a persisted graph plus the dataset that
 * seekdb already keeps, without any cuVS file round trip. */
void* seekdb_cuvs_index_from_graph(const void* graph_host, long graph_len,
                                   const float* base, long n, long dim, long degree)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  void *gdev = 0;
  float *bdev = 0;
  int64_t gshape[2], bshape[2];
  DLManagedTensor gt, dt;
  if (!h || !graph_host || !base || n <= 0 || dim <= 0 || degree <= 0) { free(h); return NULL; }
  if (graph_len != n * degree * (long)sizeof(unsigned)) {
    fprintf(stderr, "[seekdb_cuvs] graph size mismatch: %ld, expected %ld\n",
            graph_len, n * degree * (long)sizeof(unsigned));
    free(h); return NULL;
  }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsRMMAlloc(h->res, &gdev, (size_t)graph_len) != CUVS_SUCCESS) goto fail_idx;
  if (cuvsRMMAlloc(h->res, (void **)&bdev, sizeof(float) * (size_t)n * (size_t)dim) != CUVS_SUCCESS) goto fail_g;
  if (cudaMemcpy(gdev, graph_host, (size_t)graph_len, cudaMemcpyHostToDevice) != cudaSuccess) goto fail_b;
  if (cudaMemcpy(bdev, base, sizeof(float) * (size_t)n * (size_t)dim, cudaMemcpyHostToDevice) != cudaSuccess) goto fail_b;
  sk_fill_tensor(&gt, gshape, gdev, kDLCUDA, n, degree, kDLUInt, 32);
  sk_fill_tensor(&dt, bshape, bdev, kDLCUDA, n, dim, kDLFloat, 32);
  if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dt, h->idx) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] index from graph failed: %s\n", cuvsGetLastErrorText());
    goto fail_b;
  }
  h->import_graph = gdev;         h->import_graph_bytes = (size_t)graph_len;
  h->import_dataset = bdev;       h->import_dataset_bytes = sizeof(float) * (size_t)n * (size_t)dim;
  h->n = n; h->dim = dim; h->graph_degree = degree;
  return h;

fail_b:
  if (bdev) cuvsRMMFree(h->res, bdev, sizeof(float) * (size_t)n * (size_t)dim);
fail_g:
  if (gdev) cuvsRMMFree(h->res, gdev, (size_t)graph_len);
fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res);
fail:
  free(h);
  return NULL;
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    int64_t degree = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
    if (cuvsCagraIndexGetGraphDegree(index->idx, &degree) == CUVS_SUCCESS && degree > 0) {
      index->graph_degree = (long)degree;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
