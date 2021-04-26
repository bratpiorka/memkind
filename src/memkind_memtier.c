// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (C) 2021 Intel Corporation. */

#include <memkind_memtier.h>

#include <memkind/internal/memkind_arena.h>
#include <memkind/internal/memkind_log.h>

#include "config.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_STDATOMIC_H
#include <stdatomic.h>
#define MEMKIND_ATOMIC _Atomic
#else
#define MEMKIND_ATOMIC
#endif

#if defined(MEMKIND_ATOMIC_C11_SUPPORT)
#define memkind_atomic_increment(counter, val)                                 \
    atomic_fetch_add_explicit(&counter, val, memory_order_relaxed)
#define memkind_atomic_decrement(counter, val)                                 \
    atomic_fetch_sub_explicit(&counter, val, memory_order_relaxed)
#define memkind_atomic_get(src, dest)                                          \
    do {                                                                       \
        dest = atomic_load_explicit(&src, memory_order_relaxed);               \
    } while (0)
#elif defined(MEMKIND_ATOMIC_BUILTINS_SUPPORT)
#define memkind_atomic_increment(counter, val)                                 \
    __atomic_add_fetch(&counter, val, __ATOMIC_RELAXED)
#define memkind_atomic_decrement(counter, val)                                 \
    __atomic_sub_fetch(&counter, val, __ATOMIC_RELAXED)
#define memkind_atomic_get(src, dest)                                          \
    do {                                                                       \
        dest = __atomic_load_n(&src, __ATOMIC_RELAXED);                        \
    } while (0)
#elif defined(MEMKIND_ATOMIC_SYNC_SUPPORT)
#define memkind_atomic_increment(counter, val)                                 \
    __sync_add_and_fetch(&counter, val)
#define memkind_atomic_decrement(counter, val)                                 \
    __sync_sub_and_fetch(&counter, val)
#define memkind_atomic_get(src, dest)                                          \
    do {                                                                       \
        dest = __sync_sub_and_fetch(&src, 0)                                   \
    } while (0)
#else
#error "Missing atomic implementation."
#endif

// NOTE: constants defined below are used only in DYNAMIC_THRESHOLD policy.
//       These are default values that can be changed using CTL API.
// TRIGGER       - threshold between tiers will be updated if a difference
//                 between current and desired ratio between these tiers is
//                 greater than TRIGGER value (in percents)
// CHANGE        - if an update is triggered, CHANGE is the value (in percents)
//                 by which threshold will change
// CHECK_CNT     - minimum number of memory management operations that has to
//                 be made between ratio checks
// STEP          - default step (in bytes) between thresholds
#define THRESHOLD_TRIGGER   0.1  // 10%
#define THRESHOLD_CHANGE    0.25 // 25%
#define THRESHOLD_CHECK_CNT 5
#define THRESHOLD_STEP      1024

// Macro to get number of thresholds from parent object
#define THRESHOLD_NUM(obj) ((obj->size) - 1)

struct memtier_tier_cfg {
    memkind_t kind;   // Memory kind
    float kind_ratio; // Memory kind ratio
};

// Thresholds configuration - valid only for DYNAMIC_THRESHOLD policy
struct memtier_threshold_cfg {
    size_t val;       // Actual threshold level
    size_t min;       // Minimum threshold level
    size_t max;       // Maximum threshold level
    float norm_ratio; // Normalized ratio between two adjacent tiers
};

struct memtier_builder {
    memtier_policy_t policy;             // Tiering policy
    struct memtier_tier_cfg *cfg;        // Memory Tier configurations
    unsigned cfg_size;                   // Number of Memory Tier configurations
    struct memtier_threshold_cfg *thres; // Thresholds configuration for
                                         // DYNAMIC_THRESHOLD policy
    unsigned thres_size; // Number of Memory threshold configurations
    unsigned check_cnt;  // Minimum number of memory management operations
                         // that has to be made between ratio checks
    float trigger;       // Difference between ratios to update threshold
    float change;        // % of threshold change in case of update
    size_t step;         // Default step (in bytes) between thresholds
};

struct memtier_memory {
    unsigned size;                       // Number of memory kinds
    memtier_policy_t policy;             // Tiering policy
    struct memtier_tier_cfg *cfg;        // Memory Tier configuration
    struct memtier_threshold_cfg *thres; // Thresholds configuration for
                                         // DYNAMIC_THRESHOLD policy
    unsigned thres_size;           // Number of Memory threshold configurations
    unsigned thres_check_cnt;      // Counter for doing thresholds check
    unsigned thres_init_check_cnt; // Initial value of check_cnt
    float thres_trigger;           // Difference between ratios to update
                                   // threshold
    float thres_change;            // % of threshold change in case of update
};

static MEMKIND_ATOMIC size_t kind_alloc_size[MEMKIND_MAX_KIND];

void memtier_reset_size(unsigned id)
{
    kind_alloc_size[id] = 0;
}

static memkind_t
memtier_policy_static_threshold_get_kind(struct memtier_memory *memory)
{
    struct memtier_tier_cfg *cfg = memory->cfg;

    int i;
    int dest_tier = 0;

    for (i = 1; i < memory->size; ++i) {
        if ((memtier_kind_allocated_size(cfg[i].kind) * cfg[i].kind_ratio) <
            memtier_kind_allocated_size(cfg[0].kind)) {
            dest_tier = i;
        }
    }

    return cfg[dest_tier].kind;
}

static memkind_t
memtier_policy_dynamic_threshold_get_kind(struct memtier_memory *memory,
                                          size_t size)
{
    struct memtier_threshold_cfg *thres = memory->thres;
    int i;

    int dest_tier = THRESHOLD_NUM(memory); // init to last tier
    for (i = 0; i < THRESHOLD_NUM(memory); ++i) {
        if (size < thres[i].val) {
            dest_tier = i;
            break;
        }
    }

    return memory->cfg[dest_tier].kind;
}

static void
memtier_policy_dynamic_threshold_update_config(struct memtier_memory *memory)
{
    struct memtier_tier_cfg *cfg = memory->cfg;
    struct memtier_threshold_cfg *thres = memory->thres;
    int i;

    // do the ratio checks only every each thres_check_cnt
    if (--memory->thres_check_cnt > 0) {
        return;
    }

    // for every pair of adjacent tiers, check if distance between actual vs
    // desired ratio between them is above TRIGGER level and if so, change
    // threshold by CHANGE val
    for (i = 0; i < THRESHOLD_NUM(memory); ++i) {
        size_t prev_alloc_size = memtier_kind_allocated_size(cfg[i].kind);
        size_t next_alloc_size = memtier_kind_allocated_size(cfg[i + 1].kind);

        float current_ratio = -1;
        if (prev_alloc_size > 0) {
            current_ratio = (float)next_alloc_size / prev_alloc_size;
            float ratio_diff = current_ratio - thres[i].norm_ratio;
            if (fabs(ratio_diff) < memory->thres_trigger) {
                // threshold needn't to be changed
                continue;
            }
        }

        // increase/decrease threshold value by THRESHOLD_CHANGE and clamp it
        // to (min, max) range
        size_t threshold = thres[i].val * memory->thres_change;
        if ((prev_alloc_size == 0) || (current_ratio > thres[i].norm_ratio)) {
            size_t higher_threshold = thres[i].val + threshold;
            if (higher_threshold <= thres[i].max) {
                thres[i].val = higher_threshold;
            }
        } else {
            size_t lower_threshold = thres[i].val - threshold;
            if (lower_threshold >= thres[i].min) {
                thres[i].val = lower_threshold;
            }
        }
    }

    // reset threshold check counter
    memory->thres_check_cnt = memory->thres_init_check_cnt;
}

MEMKIND_EXPORT struct memtier_builder *memtier_builder_new(void)
{
    struct memtier_builder *builder =
        jemk_calloc(1, sizeof(struct memtier_builder));

    // set default values for POLICY_DYNAMIC_THRESHOLD
    builder->check_cnt = THRESHOLD_CHECK_CNT;
    builder->trigger = THRESHOLD_TRIGGER;
    builder->change = THRESHOLD_CHANGE;
    builder->step = THRESHOLD_STEP;
    return builder;
}

MEMKIND_EXPORT void memtier_builder_delete(struct memtier_builder *builder)
{
    jemk_free(builder->thres);
    jemk_free(builder->cfg);
    jemk_free(builder);
}

MEMKIND_EXPORT int memtier_builder_add_tier(struct memtier_builder *builder,
                                            memkind_t kind, unsigned kind_ratio)
{
    int i;

    if (!kind) {
        log_err("Kind is empty.");
        return -1;
    }

    for (i = 0; i < builder->cfg_size; ++i) {
        if (kind == builder->cfg[i].kind) {
            log_err("Kind is already in builder.");
            return -1;
        }
    }

    struct memtier_tier_cfg *cfg =
        jemk_realloc(builder->cfg, sizeof(*cfg) * (builder->cfg_size + 1));

    if (!cfg) {
        log_err("realloc() failed.");
        return -1;
    }

    builder->cfg = cfg;
    builder->cfg[builder->cfg_size].kind = kind;
    builder->cfg[builder->cfg_size].kind_ratio = kind_ratio;
    builder->cfg_size += 1;
    return 0;
}

int memtier_builder_create_threshold(struct memtier_builder *builder,
                                     unsigned id)
{
    if (builder->thres_size > id) {
        // nothing to do
        return 0;
    }

    struct memtier_threshold_cfg *thres =
        jemk_realloc(builder->thres, sizeof(*thres) * (id + 1));

    if (!thres) {
        log_err("realloc() failed.");
        return -1;
    }

    int old_size = builder->thres_size;
    builder->thres_size = id + 1;
    builder->thres = thres;
    int i;
    for (i = old_size; i < builder->thres_size; ++i) {
        if (i == 0) {
            builder->thres[i].val = builder->step;
            builder->thres[i].min = (float)builder->step * 0.5;
            builder->thres[i].max = (float)builder->step * 1.5 - 1;
        } else {
            builder->thres[i].val =
                builder->thres[i - i].max + (float)builder->step * 0.5;
            builder->thres[i].min = builder->thres[i - 1].max + 1;
            builder->thres[i].max =
                builder->thres[i].val + (float)builder->step * 0.5 - 1;
        }
        // NOTE: don't set norm_ratio here - it will be calculated during
        // memtier memory construction
    }

    return 0;
}

MEMKIND_EXPORT int memtier_builder_set_policy(struct memtier_builder *builder,
                                              memtier_policy_t policy)
{
    // TODO possible optimisation - use function pointer for decision
    switch (policy) {
        case MEMTIER_POLICY_STATIC_THRESHOLD:
        case MEMTIER_POLICY_DYNAMIC_THRESHOLD:
            builder->policy = policy;
            return 0;

        default:
            log_err("Unrecognized memory policy %u", policy);
            return -1;
    }
}

static inline memkind_t get_kind(struct memtier_memory *memory, size_t size)
{
    if (memory->policy == MEMTIER_POLICY_STATIC_THRESHOLD) {
        return memtier_policy_static_threshold_get_kind(memory);
    } else if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        return memtier_policy_dynamic_threshold_get_kind(memory, size);
    }

    log_err("Unrecognized memory policy %u", memory->policy);
    return NULL;
}

int memtier_policy_dynamic_threshold_construct_memtier_memory(
    struct memtier_builder *builder, struct memtier_memory *memory)
{
    int i;

    if (builder->cfg_size < 2) {
        log_err("There should be at least 2 tiers added to builder "
                "to use POLICY_DYNAMIC_THRESHOLD");
        return -1;
    }

    memory->thres_init_check_cnt = builder->check_cnt;
    memory->thres_check_cnt = builder->check_cnt;
    memory->thres_trigger = builder->trigger;
    memory->thres_change = builder->change;
    memory->thres = jemk_calloc(builder->cfg_size - 1,
                                sizeof(struct memtier_threshold_cfg));

    for (i = 0; i < builder->thres_size; ++i) {
        memory->thres[i].val = builder->thres[i].val;
        memory->thres[i].min = builder->thres[i].min;
        memory->thres[i].max = builder->thres[i].max;
    }

    // if there are less configured thresholds than tiers, add them now
    if ((builder->cfg_size - 1) > builder->thres_size) {
        int old_size = builder->thres_size;
        int ret =
            memtier_builder_create_threshold(builder, builder->cfg_size - 2);
        if (ret != 0) {
            return -1;
        }

        for (i = old_size; i < builder->thres_size; ++i) {
            memory->thres[i].val = builder->thres[i].val;
            memory->thres[i].min = builder->thres[i].min;
            memory->thres[i].max = builder->thres[i].max;
        }
    }

    memory->thres_size = builder->thres_size;
    for (i = 0; i < memory->thres_size; ++i) {
        memory->thres[i].norm_ratio =
            builder->cfg[i + 1].kind_ratio / builder->cfg[i].kind_ratio;
    }

    // Validate threshold configuration:
    // * check if values of thresholds are in ascending order - each Nth
    //   threshold value has to be lower than (N+1)th value
    // * each threshold value has to be greater than min and lower than max
    //   value defined for this thresholds
    // * min/max ranges of adjacent threshold should not overlap - max
    //   value of Nth threshold has to be lower than min value of (N+1)th
    //   threshold
    // * threshold trigger and change values has to be positive values
    for (i = 0; i < memory->thres_size; ++i) {
        if (memory->thres[i].min > memory->thres[i].val) {
            log_err("Minimum value of threshold %d "
                    "is too high (min = %zu, val = %zu)",
                    i, memory->thres[i].min, memory->thres[i].val);
            return -1;
        } else if (memory->thres[i].val > memory->thres[i].max) {
            log_err("Maximum value of threshold %d "
                    "is too low (val = %zu, max = %zu)",
                    i, memory->thres[i].val, memory->thres[i].max);
            return -1;
        }

        if ((i > 0) && (memory->thres[i - 1].max > memory->thres[i].min)) {
            log_err("Maximum value of threshold %d "
                    "should be less than minimum value of threshold %d",
                    i - 1, i);
            return -1;
        }
    }

    if (memory->thres_change < 0) {
        log_err("Threshold change value has to be >= 0");
        return -1;
    }

    if (memory->thres_trigger < 0) {
        log_err("Threshold trigger value has to be >= 0");
        return -1;
    }

    return 0;
}
MEMKIND_EXPORT struct memtier_memory *
memtier_builder_construct_memtier_memory(struct memtier_builder *builder)
{
    unsigned i;
    struct memtier_memory *memory;

    if (builder->cfg_size == 0) {
        log_err("No tier in builder.");
        return NULL;
    }

    memory = jemk_malloc(sizeof(struct memtier_memory));
    if (!memory) {
        log_err("malloc() failed.");
        return NULL;
    }

    // perform deep copy but store normalized (to kind[0]) ratio instead of
    // original
    memory->cfg =
        jemk_calloc(builder->cfg_size, sizeof(struct memtier_tier_cfg));
    if (!memory->cfg) {
        log_err("calloc() failed.");
        goto failure_calloc;
    }

    if (builder->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        int ret = memtier_policy_dynamic_threshold_construct_memtier_memory(
            builder, memory);
        if (ret != 0) {
            goto failure_cfg;
        }
    } else {
        memory->thres = NULL;
    }

    for (i = 1; i < builder->cfg_size; ++i) {
        memory->cfg[i].kind = builder->cfg[i].kind;
        memory->cfg[i].kind_ratio =
            builder->cfg[0].kind_ratio / builder->cfg[i].kind_ratio;
    }
    memory->cfg[0].kind = builder->cfg[0].kind;
    memory->cfg[0].kind_ratio = 1.0;

    memory->size = builder->cfg_size;
    memory->policy = builder->policy;
    return memory;

failure_cfg:
    jemk_free(memory->cfg);

failure_calloc:
    jemk_free(memory);

    return NULL;
}

MEMKIND_EXPORT void memtier_delete_memtier_memory(struct memtier_memory *memory)
{
    jemk_free(memory->thres);
    jemk_free(memory->cfg);
    jemk_free(memory);
}

// TODO - create "get" version for builder
// TODO - create "get" version for memtier_memory obj (this will be read-only)
// TODO - how to validate val type? e.g. provide function with explicit size_t
//        type of val for thresholds[ID].val/min/max
MEMKIND_EXPORT int memtier_ctl_set(struct memtier_builder *builder,
                                   const char *path, const void *val)
{
    // NOTE: currently the only supported queries are:
    // * policy.dynamic_threshold.thresholds[ID].val (size_t)
    // * policy.dynamic_threshold.thresholds[ID].min (size_t)
    // * policy.dynamic_threshold.thresholds[ID].max (size_t)
    // * policy.dynamic_threshold.check_cnt (unsigned)
    // * policy.dynamic_threshold.trigger (float)
    // * policy.dynamic_threshold.change (float)
    // * policy.dynamic_threshold.step (size_t)

    const char *query = path;
    char res_str[256] = {0};
    int chr_read = 0;

    int ret = sscanf(query, "%[^.].%n", res_str, &chr_read);
    if (ret && strcmp(res_str, "policy") == 0) {
        query += chr_read;
        ret = sscanf(query, "%[^.].%n", res_str, &chr_read);
        if (ret && strcmp(res_str, "dynamic_threshold") == 0) {
            query += chr_read;
            ret = sscanf(query, "%[^\[]%n", res_str, &chr_read);
            if (ret && strcmp(res_str, "thresholds") == 0) {
                int th_id = -1;
                query += chr_read;
                ret = sscanf(query, "[%d]%n", &th_id, &chr_read);
                if (th_id >= 0) {
                    query += chr_read;

                    // make sure that threshold configuration of th_id exist
                    int ret = memtier_builder_create_threshold(builder, th_id);
                    if (ret != 0) {
                        return -1;
                    }

                    struct memtier_threshold_cfg *thres =
                        &builder->thres[th_id];

                    ret = sscanf(query, ".%s", res_str);
                    if (ret && strcmp(res_str, "val") == 0) {
                        thres->val = *(size_t *)val;
                        return 0;
                    } else if (ret && strcmp(res_str, "min") == 0) {
                        thres->min = *(size_t *)val;
                        return 0;
                    } else if (ret && strcmp(res_str, "max") == 0) {
                        thres->max = *(size_t *)val;
                        return 0;
                    }
                }
            } else if (ret && strcmp(res_str, "check_cnt") == 0) {
                builder->check_cnt = *(unsigned *)val;
                return 0;
            } else if (ret && strcmp(res_str, "trigger") == 0) {
                builder->trigger = *(float *)val;
                return 0;
            } else if (ret && strcmp(res_str, "change") == 0) {
                builder->change = *(float *)val;
                return 0;
            } else if (ret && strcmp(res_str, "step") == 0) {
                builder->step = *(size_t *)val;
                return 0;
            }
        }
    }

    log_err("Invalid path: %s", query);
    return -1;
}

MEMKIND_EXPORT void *memtier_malloc(struct memtier_memory *memory, size_t size)
{
    void *ptr = memtier_kind_malloc(get_kind(memory, size), size);

    // TODO possible optimization - use function pointer for decision
    if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        memtier_policy_dynamic_threshold_update_config(memory);
    }

    return ptr;
}

MEMKIND_EXPORT void *memtier_kind_malloc(memkind_t kind, size_t size)
{
    void *ptr = memkind_malloc(kind, size);
    memkind_atomic_increment(kind_alloc_size[kind->partition],
                             jemk_malloc_usable_size(ptr));
    return ptr;
}

MEMKIND_EXPORT void *memtier_calloc(struct memtier_memory *memory, size_t num,
                                    size_t size)
{
    void *ptr = memtier_kind_calloc(get_kind(memory, size), num, size);

    if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        memtier_policy_dynamic_threshold_update_config(memory);
    }

    return ptr;
}

MEMKIND_EXPORT void *memtier_kind_calloc(memkind_t kind, size_t num,
                                         size_t size)
{
    void *ptr = memkind_calloc(kind, num, size);
    memkind_atomic_increment(kind_alloc_size[kind->partition],
                             jemk_malloc_usable_size(ptr));
    return ptr;
}

MEMKIND_EXPORT void *memtier_realloc(struct memtier_memory *memory, void *ptr,
                                     size_t size)
{
    // reallocate inside same kind
    if (ptr) {
        struct memkind *kind = memkind_detect_kind(ptr);
        ptr = memtier_kind_realloc(kind, ptr, size);

        if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
            memtier_policy_dynamic_threshold_update_config(memory);
        }

        return ptr;
    }

    ptr = memtier_kind_malloc(get_kind(memory, size), size);

    if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        memtier_policy_dynamic_threshold_update_config(memory);
    }

    return ptr;
}

MEMKIND_EXPORT void *memtier_kind_realloc(memkind_t kind, void *ptr,
                                          size_t size)
{
    if (size == 0 && ptr != NULL) {
        memkind_atomic_decrement(kind_alloc_size[kind->partition],
                                 jemk_malloc_usable_size(ptr));
        memkind_free(kind, ptr);
        return NULL;
    } else if (ptr == NULL) {
        void *n_ptr = memkind_malloc(kind, size);
        memkind_atomic_increment(kind_alloc_size[kind->partition],
                                 jemk_malloc_usable_size(n_ptr));
        return n_ptr;
    } else {
        memkind_atomic_decrement(kind_alloc_size[kind->partition],
                                 jemk_malloc_usable_size(ptr));
        void *n_ptr = memkind_realloc(kind, ptr, size);
        memkind_atomic_increment(kind_alloc_size[kind->partition],
                                 jemk_malloc_usable_size(n_ptr));
        return n_ptr;
    }
}

MEMKIND_EXPORT int memtier_posix_memalign(struct memtier_memory *memory,
                                          void **memptr, size_t alignment,
                                          size_t size)
{
    int ret = memtier_kind_posix_memalign(get_kind(memory, size), memptr,
                                          alignment, size);

    if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        memtier_policy_dynamic_threshold_update_config(memory);
    }

    return ret;
}

MEMKIND_EXPORT int memtier_kind_posix_memalign(memkind_t kind, void **memptr,
                                               size_t alignment, size_t size)
{
    int res = memkind_posix_memalign(kind, memptr, alignment, size);
    memkind_atomic_increment(kind_alloc_size[kind->partition],
                             jemk_malloc_usable_size(*memptr));

    return res;
}

MEMKIND_EXPORT size_t memtier_usable_size(void *ptr)
{
    return jemk_malloc_usable_size(ptr);
}

MEMKIND_EXPORT void memtier_memory_free(struct memtier_memory *memory,
                                        void *ptr)
{
    memtier_free(ptr);

    if (memory->policy == MEMTIER_POLICY_DYNAMIC_THRESHOLD) {
        memtier_policy_dynamic_threshold_update_config(memory);
    }
}

MEMKIND_EXPORT void memtier_free(void *ptr)
{
    memkind_t kind = memkind_detect_kind(ptr);
    if (!kind)
        return;
    memkind_atomic_decrement(kind_alloc_size[kind->partition],
                             jemk_malloc_usable_size(ptr));
    memkind_free(kind, ptr);
}

MEMKIND_EXPORT size_t memtier_kind_allocated_size(memkind_t kind)
{
    size_t size;
    memkind_atomic_get(kind_alloc_size[kind->partition], size);
    return size;
}
