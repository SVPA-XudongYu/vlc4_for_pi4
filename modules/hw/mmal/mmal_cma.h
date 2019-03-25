struct cma_pool_fixed_s;
typedef struct cma_pool_fixed_s cma_pool_fixed_t;

typedef void * cma_pool_alloc_fn(void * v, size_t size);
typedef void cma_pool_free_fn(void * v, void * el, size_t size);

void cma_pool_fixed_unref(cma_pool_fixed_t * const p);
void cma_pool_fixed_ref(cma_pool_fixed_t * const p);
void * cma_pool_fixed_get(cma_pool_fixed_t * const p, const size_t req_el_size);
void cma_pool_fixed_put(cma_pool_fixed_t * const p, void * v, const size_t el_size);
void cma_pool_fixed_kill(cma_pool_fixed_t * const p);
cma_pool_fixed_t* cma_pool_fixed_new(const unsigned int pool_size, void * const alloc_v,
    cma_pool_alloc_fn * const alloc_fn, cma_pool_free_fn * const free_fn);

typedef struct cma_buf_s {
    size_t size;
    unsigned int vcsm_h;   // VCSM handle from initial alloc
    unsigned int vc_h;     // VC handle for ZC mmal buffers
    int fd;                // dmabuf handle for GL
    void * mmap;           // ARM mapped address
} cma_buf_t;

void cma_buf_pool_delete(cma_pool_fixed_t * const p);
cma_pool_fixed_t * cma_buf_pool_new(void);



