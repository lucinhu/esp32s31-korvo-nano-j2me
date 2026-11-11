#include "korvo_jit.h"
#include "jit_code_pool.h"
#include <stdio.h>
#include <string.h>
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if NJ_ENABLE_JIT
#if !CONFIG_IDF_TARGET_ESP32S31 || CONFIG_ESP_SYSTEM_MEMPROT
#error "Korvo JIT needs ESP32-S31 and CONFIG_ESP_SYSTEM_MEMPROT=n for writable executable SRAM"
#endif

/* Retain the original internal-memory backend for controlled comparisons.
 * The mapped backend below publishes code through a separate executable alias. */
#define CODE_BUDGET (32 * 1024)
#define DRIVER_RESERVE (64 * 1024)
/* IDF rejects EXEC|8BIT generically, although S31's shared HP TCM supports
 * byte writes. EXEC|INTERNAL returns the same byte-accessible S31 SRAM. */
#define CODE_CAPS (MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL)
typedef struct { uint32_t bytes, reserved[3]; } code_header;
/* Game workloads also depend on interpreted/native work. Keep the execution
 * switch independent of the code-memory backend for reproducible comparisons. */
static bool enabled;
static bool probe_only;
static bool mapped_memory = true;

/* Leave heap space for sampled audio while retaining a bounded code cache.
 * Exhausting this pool already falls back to interpreted blocks. */
#define POOL_BYTES (256 * 1024)
#define POOL_UNIT 64
_Static_assert(CONFIG_CACHE_L1_ICACHE_LINE_SIZE <= POOL_UNIT &&
               CONFIG_CACHE_L1_DCACHE_LINE_SIZE <= POOL_UNIT,
               "Code allocations must own complete cache lines");
struct korvo_jit_arena {
    void *data, *exec;
    size_t bytes;
    bool owned;
    jit_code_pool pool;
    uint16_t spans[POOL_BYTES / POOL_UNIT];
};

/* All code writes and calls use the executable alias. The original allocation
 * is only the owner of the physical pages; it is never a code entry pointer. */
static int mapped_sync(void *ptr, size_t bytes)
{
    esp_err_t rc = esp_cache_msync(ptr, bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (rc == ESP_OK) rc = esp_cache_msync(ptr, bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    __asm__ volatile("fence rw,rw\n\tfence.i" ::: "memory");
    return rc == ESP_OK ? 0 : -1;
}

static void arena_release(struct korvo_jit_arena *a)
{
    if (!a) return;
    if (a->exec) {
        /* Do not leave dirty data behind when the same physical pages are
         * subsequently returned to the general data heap. */
        ESP_ERROR_CHECK(esp_cache_msync(a->exec, a->bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE));
        ESP_ERROR_CHECK(esp_cache_msync(a->exec, a->bytes,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST));
        __asm__ volatile("fence.i" ::: "memory");
        if (a->owned) ESP_ERROR_CHECK(esp_mmu_unmap(a->exec));
    }
    heap_caps_free(a->data);
    heap_caps_free(a);
}

static struct korvo_jit_arena *arena_create(void)
{
    struct korvo_jit_arena *a = heap_caps_calloc(1, sizeof(*a),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) return NULL;
    size_t page = CONFIG_MMU_PAGE_SIZE;
    a->bytes = (POOL_BYTES + page - 1) & ~(page - 1);
    a->data = heap_caps_aligned_alloc(page, a->bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a->data) goto fail;
    /* Flush the previous data alias before the executable alias can write. */
    if (esp_cache_msync(a->data, a->bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M |
        ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK) goto fail;
    esp_paddr_t physical;
    mmu_target_t target;
    if (esp_mmu_vaddr_to_paddr(a->data, &physical, &target) != ESP_OK ||
        target != MMU_TARGET_PSRAM0) goto fail;
    esp_err_t rc = esp_mmu_map(physical, a->bytes, target, MMU_MEM_CAP_EXEC,
                              ESP_MMU_MMAP_FLAG_PADDR_SHARED, &a->exec);
    a->owned = rc == ESP_OK;
    if (rc != ESP_OK && !(rc == ESP_ERR_INVALID_STATE && a->exec)) {
        a->exec = NULL;
        goto fail;
    }
    if (!jit_code_pool_init(&a->pool, a->exec, POOL_BYTES, POOL_UNIT,
                            a->spans, POOL_BYTES / POOL_UNIT)) goto fail;
    /* Verify both initial publication and reuse of an instruction-cache line
     * before handing any allocation to the VM. */
    void *ptr = jit_code_pool_alloc(&a->pool, 8);
    if (!ptr) goto fail;
    int (*entry)(void);
    _Static_assert(sizeof(entry) == sizeof(ptr), "Function pointer size");
    memcpy(&entry, &ptr, sizeof(entry));
    volatile uint32_t *code = ptr;
    code[0] = 0x02a00513; code[1] = 0x00008067; /* return 42 */
    if (mapped_sync(ptr, POOL_UNIT) || entry() != 42) goto fail;
    code[0] = 0x02b00513; /* return 43 from the same address */
    if (mapped_sync(ptr, POOL_UNIT) || entry() != 43) goto fail;
    jit_code_pool_free(&a->pool, ptr);
    printf("JIT_POOL_READY bytes=%u publication=42 reuse=43\n", (unsigned)a->bytes);
    return a;
fail:
    arena_release(a);
    return NULL;
}

static void *code_alloc(void *ud, nj_size n)
{
    korvo_port_state *s = ud;
    if (s->jit_mapped) {
        if (!n || n > NJ_JIT_MAX_CODE_BYTES || s->jit_pool_failed) {
            ++s->jit_refused; return NULL;
        }
        if (!s->jit_arena) {
            s->jit_arena = arena_create();
            if (!s->jit_arena) {
                s->jit_pool_failed = true;
                ++s->jit_map_failures; ++s->jit_refused;
                puts("JIT_POOL_UNAVAILABLE falling back to interpreter");
                return NULL;
            }
        }
        void *code = jit_code_pool_alloc(&s->jit_arena->pool, n);
        if (!code) { ++s->jit_refused; return NULL; }
        s->jit_code_live = s->jit_arena->pool.live_bytes;
        if (s->jit_code_live > s->jit_code_peak) s->jit_code_peak = s->jit_code_live;
        ++s->jit_allocations;
        return code;
    }
    if (!s->jit_allocations && !s->jit_refused)
        printf("JIT_HEAP exec_free=%u internal_free=%u first_block=%u\n",
               (unsigned)heap_caps_get_free_size(CODE_CAPS),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)n);
    if (!n || n > NJ_JIT_MAX_CODE_BYTES) { ++s->jit_refused; return NULL; }
    size_t bytes = ((n + 15) & ~(size_t)15) + sizeof(code_header);
    if (s->jit_code_live + bytes > CODE_BUDGET ||
        /* IDF's separate reserved DMA pool is intentionally not executable,
         * but does count toward driver headroom. */
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < bytes + DRIVER_RESERVE) {
        ++s->jit_refused; return NULL;
    }
    code_header *p = heap_caps_aligned_alloc(16, bytes, CODE_CAPS);
    if (!p) { ++s->jit_refused; return NULL; }
    p->bytes = bytes; s->jit_code_live += bytes;
    if (s->jit_code_live > s->jit_code_peak) s->jit_code_peak = s->jit_code_live;
    ++s->jit_allocations;
    return p + 1;
}

static int code_commit(void *ud, void *p, nj_size n)
{
    korvo_port_state *s = ud;
    if (!p || !n || n > NJ_JIT_MAX_CODE_BYTES) return -1;
    if (s->jit_mapped) {
        size_t span = s->jit_arena ? jit_code_pool_span(&s->jit_arena->pool, p) : 0;
        if (n > span) return -1;
        int64_t begin = esp_timer_get_time();
        int rc = mapped_sync(p, span);
        s->jit_commit_us += esp_timer_get_time() - begin;
        if (rc) return rc;
    } else {
        /* Internal HP TCM is uncached. VM execution stays on core 0; fence.i
         * synchronizes this hart's instruction fetch after the code writes. */
        __asm__ volatile("fence rw,rw\n\tfence.i" ::: "memory");
    }
    const uint8_t *bytes = p;
    for (size_t i = 0; i < n; ++i) s->jit_code_hash = (s->jit_code_hash ^ bytes[i]) * 16777619u;
    ++s->jit_commits;
    return 0;
}

static void code_free(void *ud, void *p, nj_size n)
{
    (void)n;
    if (!p) return;
    korvo_port_state *s = ud;
    if (s->jit_mapped) {
        if (s->jit_arena) {
            jit_code_pool_free(&s->jit_arena->pool, p);
            s->jit_code_live = s->jit_arena->pool.live_bytes;
        }
        return;
    }
    code_header *header = (code_header *)p - 1;
    ((korvo_port_state *)ud)->jit_code_live -= header->bytes;
    heap_caps_free(header);
}

bool korvo_jit_enabled(void) { return enabled; }
void korvo_jit_enable(bool value) { enabled = value; probe_only = false; }
void korvo_jit_probe(void) { enabled = true; probe_only = true; }
const char *korvo_jit_mode_name(void) { return !enabled ? "off" : probe_only ? "probe" : "on"; }
void korvo_jit_port(nj_port *port)
{
    ((korvo_port_state *)port->ud)->jit_mapped = mapped_memory;
    ((korvo_port_state *)port->ud)->jit_code_hash = 2166136261u;
    port->code_alloc = code_alloc; port->code_commit = code_commit; port->code_free = code_free;
}
bool korvo_jit_memory_set(const char *name)
{
    if (strcmp(name, "internal") && strcmp(name, "mapped")) return false;
    mapped_memory = !strcmp(name, "mapped");
    return true;
}
const char *korvo_jit_memory_name(void) { return mapped_memory ? "mapped" : "internal"; }
void korvo_jit_shutdown(korvo_port_state *s)
{
    size_t bytes = s->jit_arena ? s->jit_arena->bytes : 0;
    arena_release(s->jit_arena);
    s->jit_arena = NULL;
    printf("JIT_POOL memory=%s reserved=%u live=%u map_failures=%u commit_us=%llu code_hash=%08lx released=1\n",
           s->jit_mapped ? "mapped" : "internal", (unsigned)bytes, (unsigned)s->jit_code_live,
           s->jit_map_failures, (unsigned long long)s->jit_commit_us, (unsigned long)s->jit_code_hash);
}

nj_vm *__real_nj_vm_create(const nj_port *port, nj_size heap_bytes);
nj_vm *__real_nj_vm_create_with_heap_limit(const nj_port *port, nj_size heap_bytes, nj_size heap_limit);
nj_u4 __real_nj_vm_destroy(nj_vm *vm);

static nj_vm *configure_vm(nj_vm *vm)
{
    if (vm && !enabled) nj_jit_disable(vm);
    if (vm && enabled && probe_only) (void)nj_jit_set_mode(vm,NJ_JIT_MODE_PROBE_ONLY);
    nj_jit_stats stats;
    printf("JIT_BEGIN requested=%d active=%d isa=M%dA%dF%dD%dC%d budget=%u mode=%s min_block=%d ways=%d reg_cache=%d\n",
           enabled, vm ? nj_jit_get_stats(vm, &stats) : 0,
           NJ_JIT_RV32_M, NJ_JIT_RV32_A, NJ_JIT_RV32_F, NJ_JIT_RV32_D, NJ_JIT_RV32_C,
           mapped_memory ? POOL_BYTES : CODE_BUDGET,
           korvo_jit_mode_name(),NJ_JIT_MIN_BLOCK_INSNS,NJ_JIT_CACHE_WAYS,NJ_JIT_RV32_REG_CACHE);
    printf("JIT_STORAGE memory=%s budget=%u blocks=%u methods=%u map_bytes=%u\n",
           korvo_jit_memory_name(), mapped_memory ? POOL_BYTES : CODE_BUDGET,
           NJ_JIT_CACHE_ENTRIES, NJ_JIT_METHOD_ENTRIES, NJ_JIT_METHOD_ARENA_BYTES);
    return vm;
}

nj_vm *__wrap_nj_vm_create(const nj_port *port, nj_size heap_bytes)
{
    return configure_vm(__real_nj_vm_create(port, heap_bytes));
}

nj_vm *__wrap_nj_vm_create_with_heap_limit(const nj_port *port, nj_size heap_bytes,
                                          nj_size heap_limit)
{
    return configure_vm(__real_nj_vm_create_with_heap_limit(port, heap_bytes, heap_limit));
}

nj_u4 __wrap_nj_vm_destroy(nj_vm *vm)
{
    nj_jit_stats stats;
    int active = nj_jit_get_stats(vm, &stats);
    printf("JIT_STATS active=%d compiled=%llu executed=%llu bytecodes=%llu declined=%llu code_bytes=%u\n",
           active, (unsigned long long)stats.compiled_blocks, (unsigned long long)stats.executed_blocks,
           (unsigned long long)stats.bytecodes, (unsigned long long)stats.declined_blocks, (unsigned)stats.code_bytes);
    printf("JIT_DETAIL evictions=%llu method_map_evictions=%llu recompiled=%llu unsupported=%llu short=%llu faults=%llu memory_failures=%llu commit_failures=%llu peak_code=%u metadata=%u stats=%d detailed=%d\n",
           (unsigned long long)stats.code_evictions,(unsigned long long)stats.method_map_evictions,
           (unsigned long long)stats.recompiled_blocks,(unsigned long long)stats.unsupported_blocks,
           (unsigned long long)stats.short_blocks,(unsigned long long)stats.faults,(unsigned long long)stats.memory_failures,
           (unsigned long long)stats.commit_failures,(unsigned)stats.peak_code_bytes,(unsigned)stats.metadata_bytes,
           stats.stats_enabled,stats.detailed_stats_enabled);
    return __real_nj_vm_destroy(vm);
}
#else
bool korvo_jit_enabled(void) { return false; }
void korvo_jit_enable(bool enabled) { (void)enabled; }
void korvo_jit_probe(void) {}
const char *korvo_jit_mode_name(void) { return "off"; }
void korvo_jit_port(nj_port *port) { (void)port; }
bool korvo_jit_memory_set(const char *name) { return !strcmp(name, "internal"); }
const char *korvo_jit_memory_name(void) { return "internal"; }
void korvo_jit_shutdown(korvo_port_state *s) { (void)s; }
#endif
