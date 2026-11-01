#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success. The handle caches its
 *    device scratch and search parameters, so repeated calls must be serialized
 *    by the caller (ob_vsag_adaptor does, under its registry mutex).
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);

/* Graph-only persistence. seekdb's own snapshot container already carries the
 * external ids and the fp32 dataset, so the only cuVS state that has to be
 * written out is the CAGRA knn graph (n x graph_degree, uint32). That avoids
 * embedding a second copy of the dataset in a cuVS file and the extra
 * tmpfile write+read that goes with it.
 *  - seekdb_cuvs_graph_info: returns the graph byte size, or -1 when the handle
 *    cannot report a shape; n/dim/degree are filled in when non-NULL.
 *  - seekdb_cuvs_export_graph: device->host copy of the graph into dst.
 *  - seekdb_cuvs_index_from_graph: host->device copy of graph + dataset, then
 *    cuvsCagraIndexFromArgs(). Returns an opaque handle usable for search. */
long seekdb_cuvs_graph_info(void* handle, long* n, long* dim, long* degree);
int seekdb_cuvs_export_graph(void* handle, void* dst, long dst_len);
void* seekdb_cuvs_index_from_graph(const void* graph_host, long graph_len,
                                   const float* base, long n, long dim, long degree);

/* Legacy whole-index cuVS file format (graph + dataset), kept so snapshots written
 * by an older build still load. */
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);
#ifdef __cplusplus
}
#endif
#endif
