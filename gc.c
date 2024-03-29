/**********************************************************************

  gc.c -

  $Author$
  created at: Tue Oct  5 09:44:46 JST 1993

  Copyright (C) 1993-2007 Yukihiro Matsumoto
  Copyright (C) 2000  Network Applied Communication Laboratory, Inc.
  Copyright (C) 2000  Information-technology Promotion Agency, Japan

**********************************************************************/

#include "ruby/ruby.h"
#include "ruby/st.h"
#include "ruby/re.h"
#include "ruby/io.h"
#include "ruby/util.h"
#include "eval_intern.h"
#include "vm_core.h"
#include "internal.h"
#include "gc.h"
#include "pool_alloc.h"
#include "constant.h"
#include "ruby_atomic.h"
#include "probes.h"
#include <stdio.h>
#include <setjmp.h>
#include <sys/types.h>
#include <assert.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#if defined _WIN32 || defined __CYGWIN__
#include <windows.h>
#elif defined(HAVE_POSIX_MEMALIGN)
#elif defined(HAVE_MEMALIGN)
#include <malloc.h>
#endif
static void aligned_free(void *);
static void *aligned_malloc(size_t alignment, size_t size);

#ifdef HAVE_VALGRIND_MEMCHECK_H
# include <valgrind/memcheck.h>
# ifndef VALGRIND_MAKE_MEM_DEFINED
#  define VALGRIND_MAKE_MEM_DEFINED(p, n) VALGRIND_MAKE_READABLE((p), (n))
# endif
# ifndef VALGRIND_MAKE_MEM_UNDEFINED
#  define VALGRIND_MAKE_MEM_UNDEFINED(p, n) VALGRIND_MAKE_WRITABLE((p), (n))
# endif
#else
# define VALGRIND_MAKE_MEM_DEFINED(p, n) /* empty */
# define VALGRIND_MAKE_MEM_UNDEFINED(p, n) /* empty */
#endif

#define rb_setjmp(env) RUBY_SETJMP(env)
#define rb_jmp_buf rb_jmpbuf_t

/* Make alloca work the best possible way.  */
#ifdef __GNUC__
# ifndef atarist
#  ifndef alloca
#   define alloca __builtin_alloca
#  endif
# endif /* atarist */
#else
# ifdef HAVE_ALLOCA_H
#  include <alloca.h>
# else
#  ifdef _AIX
 #pragma alloca
#  else
#   ifndef alloca /* predefined by HP cc +Olibcalls */
void *alloca ();
#   endif
#  endif /* AIX */
# endif /* HAVE_ALLOCA_H */
#endif /* __GNUC__ */

#ifndef GC_MALLOC_LIMIT
#define GC_MALLOC_LIMIT 8000000
#endif
#define HEAP_MIN_SLOTS 10000
#define FREE_MIN  4096

typedef struct {
    unsigned int initial_malloc_limit;
    unsigned int initial_heap_min_slots;
    unsigned int initial_free_min;
#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
    int gc_stress;
#endif
} ruby_gc_params_t;

static ruby_gc_params_t initial_params = {
    GC_MALLOC_LIMIT,
    HEAP_MIN_SLOTS,
    FREE_MIN,
#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
    FALSE,
#endif
};

#ifndef HAVE_LONG_LONG
#define LONG_LONG long
#endif

#define nomem_error GET_VM()->special_exceptions[ruby_error_nomemory]

#if SIZEOF_LONG == SIZEOF_VOIDP
# define nonspecial_obj_id(obj) (VALUE)((SIGNED_VALUE)(obj)|FIXNUM_FLAG)
# define obj_id_to_ref(objid) ((objid) ^ FIXNUM_FLAG) /* unset FIXNUM_FLAG */
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
# define nonspecial_obj_id(obj) LL2NUM((SIGNED_VALUE)(obj) / 2)
# define obj_id_to_ref(objid) (FIXNUM_P(objid) ? \
   ((objid) ^ FIXNUM_FLAG) : (NUM2PTR(objid) << 1))
#else
# error not supported
#endif

int ruby_gc_debug_indent = 0;

/* for GC profile */
#ifndef GC_PROFILE_MORE_DETAIL
#define GC_PROFILE_MORE_DETAIL 0
#endif

typedef struct gc_profile_record {
    double gc_time;
    double gc_mark_time;
    double gc_sweep_time;
    double gc_invoke_time;

    size_t heap_use_slots;
    size_t heap_live_objects;
    size_t heap_free_objects;
    size_t heap_total_objects;
    size_t heap_use_size;
    size_t heap_total_size;

    int have_finalize;
    int is_marked;

    size_t allocate_increase;
    size_t allocate_limit;
} gc_profile_record;

static double
getrusage_time(void)
{
#ifdef RUSAGE_SELF
    struct rusage usage;
    struct timeval time;
    getrusage(RUSAGE_SELF, &usage);
    time = usage.ru_utime;
    return time.tv_sec + time.tv_usec * 1e-6;
#elif defined _WIN32
    FILETIME creation_time, exit_time, kernel_time, user_time;
    ULARGE_INTEGER ui;
    LONG_LONG q;
    double t;

    if (GetProcessTimes(GetCurrentProcess(),
			&creation_time, &exit_time, &kernel_time, &user_time) == 0)
    {
	return 0.0;
    }
    memcpy(&ui, &user_time, sizeof(FILETIME));
    q = ui.QuadPart / 10L;
    t = (DWORD)(q % 1000000L) * 1e-6;
    q /= 1000000L;
#ifdef __GNUC__
    t += q;
#else
    t += (double)(DWORD)(q >> 16) * (1 << 16);
    t += (DWORD)q & ~(~0 << 16);
#endif
    return t;
#else
    return 0.0;
#endif
}

#define GC_PROF_TIMER_START do {\
  if (RUBY_DTRACE_GC_BEGIN_ENABLED()) {\
      RUBY_DTRACE_GC_BEGIN(); \
  } \
	if (objspace->profile.run) {\
	    if (!objspace->profile.record) {\
		objspace->profile.size = 1000;\
		objspace->profile.record = malloc(sizeof(gc_profile_record) * objspace->profile.size);\
	    }\
	    if (count >= objspace->profile.size) {\
		objspace->profile.size += 1000;\
		objspace->profile.record = realloc(objspace->profile.record, sizeof(gc_profile_record) * objspace->profile.size);\
	    }\
	    if (!objspace->profile.record) {\
		rb_bug("gc_profile malloc or realloc miss");\
	    }\
	    MEMZERO(&objspace->profile.record[count], gc_profile_record, 1);\
	    gc_time = getrusage_time();\
	    objspace->profile.record[count].gc_invoke_time = gc_time - objspace->profile.invoke_time;\
	}\
    } while(0)

#define GC_PROF_TIMER_STOP(marked) do {\
  if (RUBY_DTRACE_GC_END_ENABLED()) {\
      RUBY_DTRACE_GC_END(); \
  } \
	if (objspace->profile.run) {\
	    gc_time = getrusage_time() - gc_time;\
	    if (gc_time < 0) gc_time = 0;\
	    objspace->profile.record[count].gc_time = gc_time;\
	    objspace->profile.record[count].is_marked = !!(marked);\
	    GC_PROF_SET_HEAP_INFO(objspace->profile.record[count]);\
	    objspace->profile.count++;\
	}\
    } while(0)

#if GC_PROFILE_MORE_DETAIL
#define INIT_GC_PROF_PARAMS double gc_time = 0, sweep_time = 0;\
    size_t count = objspace->profile.count, total = 0, live = 0

#define GC_PROF_MARK_TIMER_START double mark_time = 0;\
    do {\
	if (objspace->profile.run) {\
	    mark_time = getrusage_time();\
	}\
    } while(0)

#define GC_PROF_MARK_TIMER_STOP do {\
	if (objspace->profile.run) {\
	    mark_time = getrusage_time() - mark_time;\
	    if (mark_time < 0) mark_time = 0;\
	    objspace->profile.record[objspace->profile.count].gc_mark_time = mark_time;\
	}\
    } while(0)

#define GC_PROF_SWEEP_TIMER_START do {\
	if (objspace->profile.run) {\
	    sweep_time = getrusage_time();\
	}\
    } while(0)

#define GC_PROF_SWEEP_TIMER_STOP do {\
	if (objspace->profile.run) {\
	    sweep_time = getrusage_time() - sweep_time;\
	    if (sweep_time < 0) sweep_time = 0;\
	    objspace->profile.record[count].gc_sweep_time = sweep_time;\
	}\
    } while(0)
#define GC_PROF_SET_MALLOC_INFO do {\
	if (objspace->profile.run) {\
	    gc_profile_record *record = &objspace->profile.record[objspace->profile.count];\
	    record->allocate_increase = malloc_increase;\
	    record->allocate_limit = malloc_limit; \
	}\
    } while(0)
#else
#define INIT_GC_PROF_PARAMS double gc_time = 0;\
    size_t count = objspace->profile.count, total = 0, live = 0
#define GC_PROF_MARK_TIMER_START
#define GC_PROF_MARK_TIMER_STOP
#define GC_PROF_SWEEP_TIMER_START
#define GC_PROF_SWEEP_TIMER_STOP
#define GC_PROF_SET_MALLOC_INFO
#endif

#define GC_PROF_SET_HEAP_INFO(record) do {\
        live = objspace->heap.live_num;\
        total = heaps_used * HEAP_OBJ_LIMIT;\
        (record).heap_use_slots = heaps_used;\
        (record).heap_live_objects = live;\
        (record).heap_free_objects = total - live;\
        (record).heap_total_objects = total;\
        (record).have_finalize = deferred_final_list ? Qtrue : Qfalse;\
        (record).heap_use_size = live * sizeof(RVALUE);\
        (record).heap_total_size = total * sizeof(RVALUE);\
    } while(0)
#define GC_PROF_INC_LIVE_NUM objspace->heap.live_num++
#define GC_PROF_DEC_LIVE_NUM objspace->heap.live_num--

#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CYGWIN__)
#pragma pack(push, 1) /* magic for reducing sizeof(RVALUE): 24 -> 20 */
#endif

typedef struct RVALUE {
    union {
	struct {
	    VALUE flags;		/* always 0 for freed obj */
	    struct RVALUE *next;
	} free;
	struct RBasic  basic;
	struct RObject object;
	struct RClass  klass;
	struct RFloat  flonum;
	struct RString string;
	struct RArray  array;
	struct RRegexp regexp;
	struct RHash   hash;
	struct RData   data;
	struct RTypedData   typeddata;
	struct RStruct rstruct;
	struct RBignum bignum;
	struct RFile   file;
	struct RNode   node;
	struct RMatch  match;
	struct RRational rational;
	struct RComplex complex;
    } as;
#ifdef GC_DEBUG
    VALUE file;
    int   line;
#endif
} RVALUE;


#if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CYGWIN__)
#pragma pack(pop)
#endif

struct heaps_slot {
    struct heaps_header *membase;
    RVALUE *freelist;
    struct heaps_slot *next;
    struct heaps_slot *prev;
    struct heaps_slot *free_next;
    uintptr_t bits[1];
};

struct heaps_header {
    struct heaps_slot *base;
    uintptr_t *bits;
    RVALUE *start;
    RVALUE *end;
    size_t limit;
};

struct gc_list {
    VALUE *varptr;
    struct gc_list *next;
};

#define STACK_CHUNK_SIZE 500

typedef struct stack_chunk {
    VALUE data[STACK_CHUNK_SIZE];
    struct stack_chunk *next;
} stack_chunk_t;

typedef struct mark_stack {
    stack_chunk_t *chunk;
    stack_chunk_t *cache;
    size_t index;
    size_t limit;
    size_t cache_size;
    size_t unused_cache_size;
} mark_stack_t;

#ifndef CALC_EXACT_MALLOC_SIZE
#define CALC_EXACT_MALLOC_SIZE 0
#endif

#ifdef POOL_ALLOC_API
/* POOL ALLOC API */
#define POOL_ALLOC_PART 1
#include "pool_alloc.inc.h"
#undef POOL_ALLOC_PART

typedef struct pool_layout_t pool_layout_t;
struct pool_layout_t {
    pool_header
      p6,  /* st_table && st_table_entry */
      p11;  /* st_table.bins init size */
} pool_layout = {
    INIT_POOL(void*[6]),
    INIT_POOL(void*[11])
};
static void pool_finalize_header(pool_header *header);
#endif

typedef struct rb_objspace {
    struct {
	size_t limit;
	size_t increase;
#if CALC_EXACT_MALLOC_SIZE
	size_t allocated_size;
	size_t allocations;
#endif
    } malloc_params;
#ifdef POOL_ALLOC_API
    pool_layout_t *pool_headers;
#endif
    struct {
	size_t increment;
	struct heaps_slot *ptr;
	struct heaps_slot *sweep_slots;
	struct heaps_slot *free_slots;
	struct heaps_header **sorted;
	size_t length;
	size_t used;
	struct heaps_slot *reserve_slots;
	RVALUE *range[2];
	struct heaps_header *freed;
	size_t live_num;
	size_t free_num;
	size_t free_min;
	size_t final_num;
	size_t do_heap_free;
        unsigned long max_blocks_to_free;
        unsigned long freed_blocks;
    } heap;
    struct {
        unsigned long processed;
        unsigned long freed_objects;
        unsigned long freelist_size;
        unsigned long zombies;
        unsigned long free_counts[T_MASK+1];
        unsigned long live_counts[T_MASK+1];
        unsigned long gc_time_accumulator_before_gc;
        unsigned long live_after_last_mark_phase;
    } stats;
    struct {
	int dont_gc;
	int dont_lazy_sweep;
	int during_gc;
        int gc_statistics;
        int verbose_gc_stats;
	rb_atomic_t finalizing;
    } flags;
    struct {
	st_table *table;
	RVALUE *deferred;
    } final;
    mark_stack_t mark_stack;
    struct {
	int run;
	gc_profile_record *record;
	size_t count;
	size_t size;
	double invoke_time;
    } profile;
    struct gc_list *global_list;
    size_t count;
    int gc_stress;
    long heap_size;
    unsigned LONG_LONG gc_time_accumulator;
    FILE* gc_data_file;
    long gc_collections;
    unsigned LONG_LONG gc_allocated_size;
    unsigned LONG_LONG gc_num_allocations;
    unsigned long live_objects;
    unsigned LONG_LONG allocated_objects;
} rb_objspace_t;


#ifndef HEAP_ALIGN_LOG
/* default tiny heap size: 16kb */
#define HEAP_ALIGN_LOG 14
#endif
#define HEAP_ALIGN (1ul << HEAP_ALIGN_LOG)
#define HEAP_ALIGN_MASK (~(~0ul << HEAP_ALIGN_LOG))
#define REQUIRED_SIZE_BY_MALLOC (sizeof(size_t) * 5)
#define HEAP_SIZE (HEAP_ALIGN - REQUIRED_SIZE_BY_MALLOC)
#define ceildiv(i, mod) (((i) + (mod) - 1)/(mod))

#define HEAP_OBJ_LIMIT (unsigned int)((HEAP_SIZE - sizeof(struct heaps_header))/sizeof(struct RVALUE))
#define HEAP_BITMAP_LIMIT ceildiv(ceildiv(HEAP_SIZE, sizeof(struct RVALUE)), sizeof(uintptr_t)*8)
#define HEAP_SLOT_SIZE (sizeof(struct heaps_slot) + (HEAP_BITMAP_LIMIT-1) * sizeof(uintptr_t))

#define GET_HEAP_HEADER(x) (HEAP_HEADER(((uintptr_t)x) & ~(HEAP_ALIGN_MASK)))
#define GET_HEAP_SLOT(x) (GET_HEAP_HEADER(x)->base)
#define GET_HEAP_BITMAP(x) (GET_HEAP_HEADER(x)->bits)
#define NUM_IN_SLOT(p) (((uintptr_t)p & HEAP_ALIGN_MASK)/sizeof(RVALUE))
#define BITMAP_INDEX(p) (NUM_IN_SLOT(p) / (sizeof(uintptr_t) * 8))
#define BITMAP_OFFSET(p) (NUM_IN_SLOT(p) & ((sizeof(uintptr_t) * 8)-1))
#define MARKED_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] & ((uintptr_t)1 << BITMAP_OFFSET(p)))
#define MARK_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] = bits[BITMAP_INDEX(p)] | ((uintptr_t)1 << BITMAP_OFFSET(p)))
#define CLEAR_IN_BITMAP(bits, p) (bits[BITMAP_INDEX(p)] &= ~((uintptr_t)1 << BITMAP_OFFSET(p)))

static int heap_slots_increment = 10000 / HEAP_OBJ_LIMIT;
static double heap_slots_growth_factor = 1.8;



#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
#define rb_objspace (*GET_VM()->objspace)
#define ruby_initial_gc_stress	initial_params.gc_stress
int *ruby_initial_gc_stress_ptr = &ruby_initial_gc_stress;
#else
#  ifdef POOL_ALLOC_API
static rb_objspace_t rb_objspace = {{GC_MALLOC_LIMIT}, &pool_layout, {HEAP_MIN_SLOTS}};
#  else
static rb_objspace_t rb_objspace = {{GC_MALLOC_LIMIT}, {HEAP_MIN_SLOTS}};
#  endif
int *ruby_initial_gc_stress_ptr = &rb_objspace.gc_stress;
#endif
#define malloc_limit		objspace->malloc_params.limit
#define malloc_increase 	objspace->malloc_params.increase
#define heaps			objspace->heap.ptr
#define heaps_length		objspace->heap.length
#define heaps_used		objspace->heap.used
#define lomem			objspace->heap.range[0]
#define himem			objspace->heap.range[1]
#define heaps_inc		objspace->heap.increment
#define heaps_freed		objspace->heap.freed
#define dont_gc 		objspace->flags.dont_gc
#define during_gc		objspace->flags.during_gc
#define gc_statistics		objspace->flags.gc_statistics
#define verbose_gc_stats	objspace->flags.verbose_gc_stats
#define heap_size		objspace->heap_size
#define gc_time_accumulator	objspace->gc_time_accumulator
#define gc_data_file		objspace->gc_data_file
#define gc_collections		objspace->gc_collections
#define gc_allocated_size	objspace->gc_allocated_size
#define gc_num_allocations	objspace->gc_num_allocations
#define live_objects		objspace->live_objects
#define allocated_objects	objspace->allocated_objects
#define finalizing		objspace->flags.finalizing
#define finalizer_table 	objspace->final.table
#define deferred_final_list	objspace->final.deferred
#define global_List		objspace->global_list
#define ruby_gc_stress		objspace->gc_stress
#define initial_malloc_limit	initial_params.initial_malloc_limit
#define initial_heap_min_slots	initial_params.initial_heap_min_slots
#define initial_free_min	initial_params.initial_free_min
#define free_counts             objspace->stats.free_counts
#define live_counts             objspace->stats.live_counts
#define processed               objspace->stats.processed
#define zombies                 objspace->stats.zombies
#define freelist_size           objspace->stats.freelist_size
#define freed_objects           objspace->stats.freed_objects
#define gc_time_accumulator_before_gc objspace->stats.gc_time_accumulator_before_gc
#define live_after_last_mark_phase objspace->stats.live_after_last_mark_phase

#define is_lazy_sweeping(objspace) ((objspace)->heap.sweep_slots != 0)

#define nonspecial_obj_id(obj) (VALUE)((SIGNED_VALUE)(obj)|FIXNUM_FLAG)

#define HEAP_HEADER(p) ((struct heaps_header *)(p))




static void rb_objspace_call_finalizer(rb_objspace_t *objspace);
static VALUE define_final0(VALUE obj, VALUE block);
VALUE rb_define_final(VALUE obj, VALUE block);
VALUE rb_undefine_final(VALUE obj);

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
rb_objspace_t *
rb_objspace_alloc(void)
{
    rb_objspace_t *objspace = malloc(sizeof(rb_objspace_t));
    memset(objspace, 0, sizeof(*objspace));
    malloc_limit = initial_malloc_limit;
    ruby_gc_stress = ruby_initial_gc_stress;
#ifdef POOL_ALLOC_API
    objspace->pool_headers = (pool_layout_t*) malloc(sizeof(pool_layout));
    memcpy(objspace->pool_headers, &pool_layout, sizeof(pool_layout));
#endif

    return objspace;
}
#endif

static void initial_expand_heap(rb_objspace_t *objspace);
static void init_mark_stack(mark_stack_t *stack);

void
rb_gc_set_params(void)
{
    char *envp;

    rb_objspace_t *objspace = &rb_objspace;

    gc_data_file = stderr;

    if (rb_safe_level() > 0) return;

    envp = getenv("RUBY_GC_DATA_FILE");
    if (envp != NULL) {
        FILE* data_file = fopen(envp, "w");
        if (data_file != NULL) {
            gc_data_file = data_file;
        }
        else {
            fprintf(stderr, "can't open gc log file %s for writing, using default\n", envp);
        }
        /* child processes should not inherit RUBY_GC_DATA_FILE to avoid clobbering */
        ruby_unsetenv("RUBY_GC_DATA_FILE");
    }

    envp = getenv("RUBY_GC_STATS");
    if (envp != NULL) {
        int i = atoi(envp);
        if (i > 0) {
            /* gc_statistics = 1; */
            verbose_gc_stats = 1;
            fprintf(gc_data_file, "RUBY_GC_STATS=%d\n", verbose_gc_stats);
        }
        /* child processes should not inherit RUBY_GC_STATS */
        ruby_unsetenv("RUBY_GC_STATS");
    }

    envp = getenv("RUBY_GC_MALLOC_LIMIT");
    if (envp != NULL) {
	int malloc_limit_i = atoi(envp);
        if (verbose_gc_stats) {
            fprintf(gc_data_file, "RUBY_GC_MALLOC_LIMIT=%s\n", envp);
        }
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "malloc_limit=%d (%d)\n",
		    malloc_limit_i, initial_malloc_limit);
	if (malloc_limit_i > 0) {
	    initial_malloc_limit = malloc_limit_i;
            malloc_limit = initial_malloc_limit;
	}
    }

    envp = getenv("RUBY_HEAP_MIN_SLOTS");
    if (envp != NULL) {
	int heap_min_slots_i = atoi(envp);
        if (verbose_gc_stats) {
            fprintf(gc_data_file, "RUBY_HEAP_MIN_SLOTS=%s\n", envp);
        }
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "heap_min_slots=%d (%d)\n",
		    heap_min_slots_i, initial_heap_min_slots);
	if (heap_min_slots_i > 0) {
	    initial_heap_min_slots = heap_min_slots_i;
            initial_expand_heap(&rb_objspace);
	}
    }

    if (!(envp = getenv("RUBY_FREE_MIN")))
        envp = getenv("RUBY_HEAP_FREE_MIN");
    if (envp != NULL) {
	int free_min_i = atoi(envp);
        if (verbose_gc_stats) {
            fprintf(gc_data_file, "RUBY_HEAP_FREE_MIN=%s\n", envp);
        }
	if (RTEST(ruby_verbose))
	    fprintf(stderr, "free_min=%d (%d)\n", free_min_i, initial_free_min);
	if (free_min_i > 0) {
	    initial_free_min = free_min_i;
	}
    }

    envp = getenv("RUBY_HEAP_SLOTS_INCREMENT");
    if (envp != NULL) {
        int i = atoi(envp);
        if (verbose_gc_stats) {
            fprintf(gc_data_file, "RUBY_HEAP_SLOTS_INCREMENT=%s\n", envp);
        }
        heap_slots_increment = i / HEAP_OBJ_LIMIT;
    }

    envp = getenv("RUBY_HEAP_SLOTS_GROWTH_FACTOR");
    if (envp != NULL) {
        double d = atof(envp);
        if (verbose_gc_stats) {
            fprintf(gc_data_file, "RUBY_HEAP_SLOTS_GROWTH_FACTOR=%s\n", envp);
        }
        if (d > 0) {
            heap_slots_growth_factor = d;
        }
    }

    fflush(gc_data_file);
}

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
static void gc_sweep(rb_objspace_t *);
static void slot_sweep(rb_objspace_t *, struct heaps_slot *);
static void rest_sweep(rb_objspace_t *);
static void free_stack_chunks(mark_stack_t *);

void
rb_objspace_free(rb_objspace_t *objspace)
{
    rest_sweep(objspace);
    if (objspace->profile.record) {
	free(objspace->profile.record);
	objspace->profile.record = 0;
    }
    if (global_List) {
	struct gc_list *list, *next;
	for (list = global_List; list; list = next) {
	    next = list->next;
	    xfree(list);
	}
    }
    if (objspace->heap.reserve_slots) {
        struct heaps_slot *list, *next;
        for (list = objspace->heap.reserve_slots; list; list = next) {
            next = list->free_next;
            free(list);
        }
    }
    if (objspace->heap.sorted) {
	size_t i;
	for (i = 0; i < heaps_used; ++i) {
            free(objspace->heap.sorted[i]->base);
            aligned_free(objspace->heap.sorted[i]);
	}
	free(objspace->heap.sorted);
	heaps_used = 0;
	heaps = 0;
    }
    free_stack_chunks(&objspace->mark_stack);
#ifdef POOL_ALLOC_API
    if (objspace->pool_headers) {
        pool_finalize_header(&objspace->pool_headers->p6);
        pool_finalize_header(&objspace->pool_headers->p11);
        free(objspace->pool_headers);
    }
#endif
    free(objspace);
}
#endif

extern sa_table rb_class_tbl;

int ruby_disable_gc_stress = 0;

static void run_final(rb_objspace_t *objspace, VALUE obj);
static int garbage_collect(rb_objspace_t *objspace);
static int gc_lazy_sweep(rb_objspace_t *objspace);

void
rb_global_variable(VALUE *var)
{
    rb_gc_register_address(var);
}

static void *
ruby_memerror_body(void *dummy)
{
    rb_memerror();
    return 0;
}

static void
ruby_memerror(void)
{
    if (ruby_thread_has_gvl_p()) {
	rb_memerror();
    }
    else {
	if (ruby_native_thread_p()) {
	    rb_thread_call_with_gvl(ruby_memerror_body, 0);
	}
	else {
	    /* no ruby thread */
	    fprintf(stderr, "[FATAL] failed to allocate memory\n");
	    exit(EXIT_FAILURE);
	}
    }
}

void
rb_memerror(void)
{
    rb_thread_t *th = GET_THREAD();
    if (!nomem_error ||
	(rb_thread_raised_p(th, RAISED_NOMEMORY) && rb_safe_level() < 4)) {
	fprintf(stderr, "[FATAL] failed to allocate memory\n");
	exit(EXIT_FAILURE);
    }
    if (rb_thread_raised_p(th, RAISED_NOMEMORY)) {
	rb_thread_raised_clear(th);
	GET_THREAD()->errinfo = nomem_error;
	JUMP_TAG(TAG_RAISE);
    }
    rb_thread_raised_set(th, RAISED_NOMEMORY);
    rb_exc_raise(nomem_error);
}

/*
 *  call-seq:
 *    GC.stress                 -> true or false
 *
 *  returns current status of GC stress mode.
 */

static VALUE
gc_stress_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return ruby_gc_stress ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *    GC.stress = bool          -> bool
 *
 *  Updates the GC stress mode.
 *
 *  When stress mode is enabled the GC is invoked at every GC opportunity:
 *  all memory and object allocations.
 *
 *  Enabling stress mode makes Ruby very slow, it is only for debugging.
 */

static VALUE
gc_stress_set(VALUE self, VALUE flag)
{
    rb_objspace_t *objspace = &rb_objspace;
    rb_secure(2);
    ruby_gc_stress = RTEST(flag);
    return flag;
}

/*
 *  call-seq:
 *    GC::Profiler.enable?                 -> true or false
 *
 *  The current status of GC profile mode.
 */

static VALUE
gc_profile_enable_get(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return objspace->profile.run ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *    GC::Profiler.enable          -> nil
 *
 *  Starts the GC profiler.
 *
 */

static VALUE
gc_profile_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->profile.run = TRUE;
    return Qnil;
}

/*
 *  call-seq:
 *    GC::Profiler.disable          -> nil
 *
 *  Stops the GC profiler.
 *
 */

static VALUE
gc_profile_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->profile.run = FALSE;
    return Qnil;
}

/*
 *  call-seq:
 *    GC::Profiler.clear          -> nil
 *
 *  Clears the GC profiler data.
 *
 */

static VALUE
gc_profile_clear(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    MEMZERO(objspace->profile.record, gc_profile_record, objspace->profile.size);
    objspace->profile.count = 0;
    return Qnil;
}

static void *
negative_size_allocation_error_with_gvl(void *ptr)
{
    rb_raise(rb_eNoMemError, "%s", (const char *)ptr);
    return 0; /* should not be reached */
}

static void
negative_size_allocation_error(const char *msg)
{
    if (ruby_thread_has_gvl_p()) {
	rb_raise(rb_eNoMemError, "%s", msg);
    }
    else {
	if (ruby_native_thread_p()) {
	    rb_thread_call_with_gvl(negative_size_allocation_error_with_gvl, (void *)msg);
	}
	else {
	    fprintf(stderr, "[FATAL] %s\n", msg);
	    exit(EXIT_FAILURE);
	}
    }
}

static void *
gc_with_gvl(void *ptr)
{
    return (void *)(VALUE)garbage_collect((rb_objspace_t *)ptr);
}

static int
garbage_collect_with_gvl(rb_objspace_t *objspace)
{
    if (dont_gc) return TRUE;
    if (ruby_thread_has_gvl_p()) {
	return garbage_collect(objspace);
    }
    else {
	if (ruby_native_thread_p()) {
	    return (int)(VALUE)rb_thread_call_with_gvl(gc_with_gvl, (void *)objspace);
	}
	else {
	    /* no ruby thread */
	    fprintf(stderr, "[FATAL] failed to allocate memory\n");
	    exit(EXIT_FAILURE);
	}
    }
}

static void vm_xfree(rb_objspace_t *objspace, void *ptr);

static inline size_t
vm_malloc_prepare(rb_objspace_t *objspace, size_t size)
{
    if ((ssize_t)size < 0) {
	negative_size_allocation_error("negative allocation size (or too big)");
    }
    if (size == 0) size = 1;

#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(size_t);
#endif

    if ((ruby_gc_stress && !ruby_disable_gc_stress) ||
	(malloc_increase+size) > malloc_limit) {
	garbage_collect_with_gvl(objspace);
    }

    return size;
}

static inline void *
vm_malloc_fixup(rb_objspace_t *objspace, void *mem, size_t size)
{
    malloc_increase += size;

#if CALC_EXACT_MALLOC_SIZE
    objspace->malloc_params.allocated_size += size;
    objspace->malloc_params.allocations++;
    ((size_t *)mem)[0] = size;
    mem = (size_t *)mem + 1;
#endif

    if (gc_statistics) {
        gc_allocated_size += size;
	gc_num_allocations += 1;
    }

    return mem;
}

#define TRY_WITH_GC(alloc) do { \
	if (!(alloc) && \
	    (!garbage_collect_with_gvl(objspace) || \
	     !(alloc))) { \
	    ruby_memerror(); \
	} \
    } while (0)

static void *
vm_xmalloc(rb_objspace_t *objspace, size_t size)
{
    void *mem;

    size = vm_malloc_prepare(objspace, size);
    TRY_WITH_GC(mem = malloc(size));
    return vm_malloc_fixup(objspace, mem, size);
}

static void *
vm_xrealloc(rb_objspace_t *objspace, void *ptr, size_t size)
{
    void *mem;

    if ((ssize_t)size < 0) {
	negative_size_allocation_error("negative re-allocation size");
    }
    if (!ptr) return vm_xmalloc(objspace, size);
    if (size == 0) {
	vm_xfree(objspace, ptr);
	return 0;
    }
    if (ruby_gc_stress && !ruby_disable_gc_stress)
	garbage_collect_with_gvl(objspace);

#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(size_t);
    objspace->malloc_params.allocated_size -= size;
    ptr = (size_t *)ptr - 1;
#endif

    mem = realloc(ptr, size);
    if (!mem) {
	if (garbage_collect_with_gvl(objspace)) {
	    mem = realloc(ptr, size);
	}
	if (!mem) {
	    ruby_memerror();
        }
    }
    malloc_increase += size;

#if CALC_EXACT_MALLOC_SIZE
    objspace->malloc_params.allocated_size += size;
    ((size_t *)mem)[0] = size;
    mem = (size_t *)mem + 1;
#endif

    /* TODO: we can't count correctly unless we store old size on heap
    if (gc_statistics) {
        gc_allocated_size += size;
	gc_num_allocations += 1;
    }
    */

    return mem;
}

static void
vm_xfree(rb_objspace_t *objspace, void *ptr)
{
#if CALC_EXACT_MALLOC_SIZE
    size_t size;
    ptr = ((size_t *)ptr) - 1;
    size = ((size_t*)ptr)[0];
    if (size) {
    objspace->malloc_params.allocated_size -= size;
    objspace->malloc_params.allocations--;
    }
#endif

    free(ptr);
}

void *
ruby_xmalloc(size_t size)
{
    return vm_xmalloc(&rb_objspace, size);
}

static inline size_t
xmalloc2_size(size_t n, size_t size)
{
    size_t len = size * n;
    if (n != 0 && size != len / n) {
	rb_raise(rb_eArgError, "malloc: possible integer overflow");
    }
    return len;
}

void *
ruby_xmalloc2(size_t n, size_t size)
{
    return vm_xmalloc(&rb_objspace, xmalloc2_size(n, size));
}

static void *
vm_xcalloc(rb_objspace_t *objspace, size_t count, size_t elsize)
{
    void *mem;
    size_t size;

    size = xmalloc2_size(count, elsize);
    size = vm_malloc_prepare(objspace, size);

    TRY_WITH_GC(mem = calloc(1, size));
    return vm_malloc_fixup(objspace, mem, size);
}

void *
ruby_xcalloc(size_t n, size_t size)
{
    return vm_xcalloc(&rb_objspace, n, size);
}

void *
ruby_xrealloc(void *ptr, size_t size)
{
    return vm_xrealloc(&rb_objspace, ptr, size);
}

void *
ruby_xrealloc2(void *ptr, size_t n, size_t size)
{
    size_t len = size * n;
    if (n != 0 && size != len / n) {
	rb_raise(rb_eArgError, "realloc: possible integer overflow");
    }
    return ruby_xrealloc(ptr, len);
}

void
ruby_xfree(void *x)
{
    if (x)
	vm_xfree(&rb_objspace, x);
}

/*
 *  call-seq:
 *     GC.enable    -> true or false
 *
 *  Enables garbage collection, returning <code>true</code> if garbage
 *  collection was previously disabled.
 *
 *     GC.disable   #=> false
 *     GC.enable    #=> true
 *     GC.enable    #=> false
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = dont_gc;

    dont_gc = FALSE;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.disable    -> true or false
 *
 *  Disables garbage collection, returning <code>true</code> if garbage
 *  collection was already disabled.
 *
 *     GC.disable   #=> false
 *     GC.disable   #=> true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_disable(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = dont_gc;

    dont_gc = TRUE;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.enable_stats    => true or false
 *
 *  Enables garbage collection statistics, returning <code>true</code> if garbage
 *  collection statistics was already enabled.
 *
 *     GC.enable_stats   #=> false or true
 *     GC.enable_stats   #=> true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_enable_stats()
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = gc_statistics;
    gc_statistics = 1;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.disable_stats    => true or false
 *
 *  Disables garbage collection statistics, returning <code>true</code> if garbage
 *  collection statistics was already disabled.
 *
 *     GC.disable_stats   #=> false or true
 *     GC.disable_stats   #=> true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_disable_stats()
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = gc_statistics;
    gc_statistics = 0;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.stats_enabled?    => true or false
 *
 *  Check whether GC stats have been enabled.
 *
 *     GC.stats_enabled?   #=> false or true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_stats_enabled()
{
    rb_objspace_t *objspace = &rb_objspace;
    return gc_statistics ? Qtrue : Qfalse;
}

#ifdef POOL_ALLOC_API
/* POOL ALLOC API */
#define POOL_ALLOC_PART 2
#include "pool_alloc.inc.h"
#undef POOL_ALLOC_PART

void
ruby_xpool_free(void *ptr)
{
    pool_free_entry((void**)ptr);
}

#define CONCRET_POOL_MALLOC(pnts) \
void * ruby_xpool_malloc_##pnts##p () { \
    return pool_alloc_entry(&rb_objspace.pool_headers->p##pnts ); \
}
CONCRET_POOL_MALLOC(6)
CONCRET_POOL_MALLOC(11)
#undef CONCRET_POOL_MALLOC

#endif

/*
 *  call-seq:
 *     GC.clear_stats    => nil
 *
 *  Clears garbage collection statistics, returning nil. This resets the number
 *  of collections (GC.collections) and the time used (GC.time) to 0.
 *
 *     GC.clear_stats    #=> nil
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_clear_stats()
{
    rb_objspace_t *objspace = &rb_objspace;
    gc_collections = 0;
    gc_time_accumulator = 0;
    gc_time_accumulator_before_gc = 0;
    gc_allocated_size = 0;
    gc_num_allocations = 0;
    return Qnil;
}

/*
 *  call-seq:
 *     GC.allocated_size    => Integer
 *
 *  Returns the size of memory (in bytes) allocated since GC statistics collection
 *  was enabled.
 *
 *     GC.allocated_size    #=> 35
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_allocated_size()
{
    rb_objspace_t *objspace = &rb_objspace;
#if HAVE_LONG_LONG
    return ULL2NUM(gc_allocated_size);
#else
    return ULONG2NUM(gc_allocated_size);
#endif
}

/*
 *  call-seq:
 *     GC.num_allocations    => Integer
 *
 *  Returns the number of memory allocations since GC statistics collection
 *  was enabled.
 *
 *     GC.num_allocations    #=> 150
 *
 */
VALUE
rb_gc_num_allocations()
{
    rb_objspace_t *objspace = &rb_objspace;
#if HAVE_LONG_LONG
    return ULL2NUM(gc_num_allocations);
#else
    return ULONG2NUM(gc_num_allocations);
#endif
}

/*
 *  call-seq:
 *     GC.enable_trace    => true or false
 *
 *  Enables garbage collection tracing, returning <code>true</code> if garbage
 *  collection tracing was already enabled.
 *
 *     GC.enable_trace   #=> false or true
 *     GC.enable_trace   #=> true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_enable_trace()
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = verbose_gc_stats;
    verbose_gc_stats = 1;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.disable_trace    => true or false
 *
 *  Disables garbage collection tracing, returning <code>true</code> if garbage
 *  collection tracing was already disabled.
 *
 *     GC.disable_trace   #=> false or true
 *     GC.disable_trace   #=> true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_disable_trace()
{
    rb_objspace_t *objspace = &rb_objspace;
    int old = verbose_gc_stats;
    verbose_gc_stats = 0;
    return old ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *     GC.trace_enabled?    => true or false
 *
 *  Check whether GC tracing has been enabled.
 *
 *     GC.trace_enabled?   #=> false or true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_trace_enabled()
{
    rb_objspace_t *objspace = &rb_objspace;
    return verbose_gc_stats ? Qtrue : Qfalse;
}


const char* GC_LOGFILE_IVAR = "@gc_logfile_name";

/*
 *  call-seq:
 *     GC.log_file(filename=nil, mode="w")    => boolean
 *
 *  Changes the GC data log file. Closes the currently open logfile.
 *  Returns true if the file was successfully opened for
 *  writing. Returns false if the file could not be opened for
 *  writing. Returns the name of the current logfile (or nil) if no
 *  parameter is given. Restores logging to stderr when given nil as
 *  an argument.
 *
 *     GC.log_file                  #=> nil
 *     GC.log_file "/tmp/gc.log"    #=> true
 *     GC.log_file                  #=> "/tmp/gc.log"
 *     GC.log_file nil              #=> true
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_log_file(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE filename = Qnil;
    VALUE mode_str = Qnil;
    FILE* f = NULL;
    const char* mode = "w";

    VALUE current_logfile_name = rb_iv_get(rb_mGC, GC_LOGFILE_IVAR);

    if (argc==0)
        return current_logfile_name;

    rb_scan_args(argc, argv, "02", &filename, &mode_str);

    if (filename == Qnil) {
        /* close current logfile and reset logfile to stderr */
        if (gc_data_file != stderr) {
            fclose(gc_data_file);
            gc_data_file = stderr;
            rb_iv_set(rb_mGC, GC_LOGFILE_IVAR, Qnil);
        }
        return Qtrue;
    }

    /* we have a real logfile name */
    filename = StringValue(filename);

    if (rb_equal(current_logfile_name, filename) == Qtrue) {
        /* do nothing if we get the file name we're already logging to */
        return Qtrue;
    }

    /* get mode for file opening */
    if (mode_str != Qnil)
    {
      mode = RSTRING_PTR(StringValue(mode_str));
    }

    /* try to open file in given mode */
    if (f = fopen(RSTRING_PTR(filename), mode)) {
        if (gc_data_file != stderr) {
            fclose(gc_data_file);
        }
        gc_data_file = f;
        rb_iv_set(rb_mGC, GC_LOGFILE_IVAR, filename);
    } else {
        return Qfalse;
    }
    return Qtrue;
}

/*
 * Called from process.c before a fork. Flushes the gc log file to
 * avoid writing the buffered output twice (once in the parent, and
 * once in the child).
 */
void
rb_gc_before_fork()
{
    rb_objspace_t *objspace = &rb_objspace;
    fflush(gc_data_file);
}

/*
 * Called from process.c after a fork in the child process. Turns off
 * logging, disables GC stats and resets all gc counters and timing
 * information.
 */
void
rb_gc_after_fork()
{
    rb_objspace_t *objspace = &rb_objspace;
    rb_gc_disable_stats();
    rb_gc_clear_stats();
    rb_gc_disable_trace();
    gc_data_file = stderr;
    rb_iv_set(rb_mGC, GC_LOGFILE_IVAR, Qnil);
}

/*
 *  call-seq:
 *     GC.log String  => String
 *
 *  Logs string to the GC data file and returns it.
 *
 *     GC.log "manual GC call"    #=> "manual GC call"
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_log(self, original_str)
     VALUE self, original_str;
{
    rb_objspace_t *objspace = &rb_objspace;
    if (original_str == Qnil) {
        fprintf(gc_data_file, "\n");
    }
    else {
        VALUE str = StringValue(original_str);
        char *p = RSTRING_PTR(str);
        fprintf(gc_data_file, "%s\n", p);
    }
    return original_str;
}

/*
 *  call-seq:
 *     GC.dump    => nil
 *
 *  dumps information about the current GC data structures to the GC log file
 *
 *     GC.dump    #=> nil
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_dump()
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t i;

    for (i = 0; i < heaps_used; i++) {
        size_t limit = objspace->heap.sorted[i]->limit;
        fprintf(gc_data_file, "HEAP[%2lu]: size=%7lu\n", (unsigned long)i, (unsigned long)limit);
    }

    return Qnil;
}

static const char* obj_type(VALUE tp);

#ifdef GC_DEBUG
/*
 *  call-seq:
 *     GC.dump_file_and_line_info(String, boolean)    => nil
 *
 *  dumps information on which currently allocated object was created by which file and on which line
 *
 *     GC.dump_file_and_line_info(String, boolean)    #=> nil
 *
 *  The second parameter specifies whether class names should be included in the dump.
 *  Note that including class names will allocate additional string objects on the heap.
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_dump_file_and_line_info(int argc, VALUE *argv)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE filename, str, include_classnames = Qnil;
    char *fname = NULL;
    FILE* f = NULL;
    size_t i = 0;

    rb_scan_args(argc, argv, "11", &filename, &include_classnames);

    str = StringValue(filename);
    fname = RSTRING_PTR(str);
    f = fopen(fname, "w");

    for (i = 0; i < heaps_used; i++) {
        RVALUE *p, *pend;

        p = objspace->heap.sorted[i].start; pend = objspace->heap.sorted[i].end;
        for (;p < pend; p++) {
            if (p->as.basic.flags) {
                const char *src_filename = (p->file && p->file != Qnil )? RSTRING_PTR(p->file) : "";
                fprintf(f, "%s:%s:%d", obj_type(p->as.basic.flags & T_MASK), src_filename, (int)p->line);
                // rb_obj_classname will create objects on the heap, we need a better solution
                if (include_classnames == Qtrue) {
                    /* write the class */
                    fprintf(f, ":");
                    switch (BUILTIN_TYPE(p)) {
                    case T_NONE:
                        fprintf(f, "__none__");
                        break;
                    case T_UNDEF:
                        fprintf(f, "__undef__");
                        break;
                    case T_NODE:
                        fprintf(f, "__node__");
                        break;
                    default:
                        if (!p->as.basic.klass) {
                            fprintf(f, "__unknown__");
                        } else {
                            fprintf(f, "%s", rb_obj_classname((VALUE)p));
                        }
                    }
                    /* print object size for some known object types */
                    switch (BUILTIN_TYPE(p)) {
                    case T_STRING:
                        fprintf(f, ":%lu", RSTRING_LEN(p));
                        break;
                    case T_ARRAY:
                        fprintf(f, ":%lu", RARRAY_LEN(p));
                        break;
                    case T_HASH:
                        fprintf(f, ":%lu", (long unsigned int)RHASH_SIZE(p));
                        break;
                    }
                }
                fprintf(f, "\n");
            }
        }
    }
    fclose(f);
    return Qnil;
}
#endif

/*
 *  call-seq:
 *     GC.heap_slots    => Integer
 *
 *  Returns the number of heap slots available for object allocations.
 *
 *     GC.heap_slots    #=> 10000
 *
 */
VALUE
rb_gc_heap_slots()
{
    rb_objspace_t *objspace = &rb_objspace;
    return LONG2NUM(heap_size);
}


/*
 *  call-seq:
 *     GC.collections    => Integer
 *
 *  Returns the number of garbage collections performed while GC statistics collection
 *  was enabled.
 *
 *     GC.collections    #=> 35
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_collections()
{
    rb_objspace_t *objspace = &rb_objspace;
    return LONG2NUM(gc_collections);
}

/*
 *  call-seq:
 *     GC.time    => Integer
 *
 *  Returns the time spent during garbage collection while GC statistics collection
 *  was enabled (in micro seconds).
 *
 *     GC.time    #=> 20000
 *
 */

RUBY_FUNC_EXPORTED
VALUE
rb_gc_time()
{
    rb_objspace_t *objspace = &rb_objspace;
#if HAVE_LONG_LONG
    return LL2NUM(gc_time_accumulator);
#else
    return LONG2NUM(gc_time_accumulator);
#endif
}

/*
 *  call-seq:
 *     GC.heap_slots_live_after_last_gc    => Integer
 *
 *  Returns the number of heap slots which were live after the last garbage collection.
 *
 *     GC.heap_slots_live_after_last_gc    #=> 231223
 *
 */
VALUE
rb_gc_heap_slots_live_after_last_gc()
{
    rb_objspace_t *objspace = &rb_objspace;
    return ULONG2NUM(live_after_last_mark_phase);
}

VALUE
rb_gc_free_count()
{
    rb_objspace_t *objspace = &rb_objspace;
    return ULONG2NUM(heaps_used * HEAP_OBJ_LIMIT - objspace->heap.live_num);
}


VALUE rb_mGC;

void
rb_gc_register_mark_object(VALUE obj)
{
    VALUE ary = GET_THREAD()->vm->mark_object_ary;
    rb_ary_push(ary, obj);
}

void
rb_gc_register_address(VALUE *addr)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct gc_list *tmp;

    tmp = ALLOC(struct gc_list);
    tmp->next = global_List;
    tmp->varptr = addr;
    global_List = tmp;
}

void
rb_gc_unregister_address(VALUE *addr)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct gc_list *tmp = global_List;

    if (tmp->varptr == addr) {
	global_List = tmp->next;
	xfree(tmp);
	return;
    }
    while (tmp->next) {
	if (tmp->next->varptr == addr) {
	    struct gc_list *t = tmp->next;

	    tmp->next = tmp->next->next;
	    xfree(t);
	    break;
	}
	tmp = tmp->next;
    }
}


static void
allocate_sorted_heaps(rb_objspace_t *objspace, size_t next_heaps_length)
{
    struct heaps_header **p;
    struct heaps_slot *slot;
    size_t size, add, i;

    size = next_heaps_length*sizeof(struct heaps_header *);
    add = next_heaps_length - heaps_used;

    if (heaps_used > 0) {
        p = (struct heaps_header **)realloc(objspace->heap.sorted, size);
	if (p) objspace->heap.sorted = p;
    }
    else {
    p = objspace->heap.sorted = (struct heaps_header **)malloc(size);
    }

    if (p == 0) {
	during_gc = 0;
	rb_memerror();
    }

    for (i = 0; i < add; i++) {
        slot = (struct heaps_slot *)malloc(HEAP_SLOT_SIZE);
        if (slot == 0) {
            during_gc = 0;
            rb_memerror();
            return;
        }
        slot->free_next = objspace->heap.reserve_slots;
        objspace->heap.reserve_slots = slot;
    }
}

static void *
aligned_malloc(size_t alignment, size_t size)
{
    void *res;

#if defined __MINGW32__
    res = __mingw_aligned_malloc(size, alignment);
#elif defined _WIN32 && !defined __CYGWIN__
    res = _aligned_malloc(size, alignment);
#elif defined(HAVE_POSIX_MEMALIGN)
    if (posix_memalign(&res, alignment, size) == 0) {
        return res;
    }
    else {
        return NULL;
    }
#elif defined(HAVE_MEMALIGN)
    res = memalign(alignment, size);
#else
    char* aligned;
    res = malloc(alignment + size + sizeof(void*));
    aligned = (char*)res + alignment + sizeof(void*);
    aligned -= ((VALUE)aligned & (alignment - 1));
    ((void**)aligned)[-1] = res;
    res = (void*)aligned;
#endif

#if defined(_DEBUG) || defined(GC_DEBUG)
    /* alignment must be a power of 2 */
    assert((alignment - 1) & alignment == 0);
    assert(alignment % sizeof(void*) == 0);
#endif
    return res;
}

static void
aligned_free(void *ptr)
{
#if defined __MINGW32__
    __mingw_aligned_free(ptr);
#elif defined _WIN32 && !defined __CYGWIN__
    _aligned_free(ptr);
#elif defined(HAVE_MEMALIGN) || defined(HAVE_POSIX_MEMALIGN)
    free(ptr);
#else
    free(((void**)ptr)[-1]);
#endif
}

static void
link_free_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    slot->free_next = objspace->heap.free_slots;
    objspace->heap.free_slots = slot;
}

static void
unlink_free_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    objspace->heap.free_slots = slot->free_next;
    slot->free_next = NULL;
}

static void
assign_heap_slot(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    struct heaps_header *membase;
    struct heaps_slot *slot;
    size_t hi, lo, mid;
    size_t objs;
    /*
    if (gc_statistics & verbose_gc_stats) {
	fprintf(gc_data_file, "assigning heap slot: %d\n", heaps_inc);
    }
    */
    objs = HEAP_OBJ_LIMIT;
    membase = (struct heaps_header*)aligned_malloc(HEAP_ALIGN, HEAP_SIZE);
    if (membase == 0) {
	during_gc = 0;
	rb_memerror();
    }
    assert(objspace->heap.reserve_slots != NULL);
    slot = objspace->heap.reserve_slots;
    objspace->heap.reserve_slots = slot->free_next;
    MEMZERO((void*)slot, struct heaps_slot, 1);

    slot->next = heaps;
    if (heaps) heaps->prev = slot;
    heaps = slot;

    p = (RVALUE*)((VALUE)membase + sizeof(struct heaps_header));
    if ((VALUE)p % sizeof(RVALUE) != 0) {
       p = (RVALUE*)((VALUE)p + sizeof(RVALUE) - ((VALUE)p % sizeof(RVALUE)));
       objs = (HEAP_SIZE - (size_t)((VALUE)p - (VALUE)membase))/sizeof(RVALUE);
    }

    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
	register struct heaps_header *mid_membase;
	mid = (lo + hi) / 2;
        mid_membase = objspace->heap.sorted[mid];
	if (mid_membase < membase) {
	    lo = mid + 1;
	}
	else if (mid_membase > membase) {
	    hi = mid;
	}
	else {
	    rb_bug("same heap slot is allocated: %p at %"PRIuVALUE, (void *)membase, (VALUE)mid);
	}
    }
    if (hi < heaps_used) {
	MEMMOVE(&objspace->heap.sorted[hi+1], &objspace->heap.sorted[hi], struct heaps_header*, heaps_used - hi);
    }
    objspace->heap.sorted[hi] = membase;
    membase->start = p;
    membase->end = (p + objs);
    membase->base = heaps;
    membase->bits = heaps->bits;
    membase->limit = objs;
    heaps->membase = membase;
    memset(heaps->bits, 0, HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
    objspace->heap.free_num += objs;
    pend = p + objs;
    if (lomem == 0 || lomem > p) lomem = p;
    if (himem < pend) himem = pend;
    heaps_used++;
    heap_size += objs;

    while (p < pend) {
	p->as.free.flags = 0;
	p->as.free.next = heaps->freelist;
	heaps->freelist = p;
	p++;
    }
    link_free_heap_slot(objspace, heaps);
}

static void
add_heap_slots(rb_objspace_t *objspace, size_t add)
{
    size_t i;
    size_t next_heaps_length;

    next_heaps_length = heaps_used + add;

    if (next_heaps_length > heaps_length) {
        allocate_sorted_heaps(objspace, next_heaps_length);
        heaps_length = next_heaps_length;
    }

    for (i = 0; i < add; i++) {
        assign_heap_slot(objspace);
        heaps_length = next_heaps_length;
    }
    heaps_inc = 0;
}

static void
init_heap(rb_objspace_t *objspace)
{
    add_heap_slots(objspace, HEAP_MIN_SLOTS / HEAP_OBJ_LIMIT);
    init_mark_stack(&objspace->mark_stack);
#ifdef USE_SIGALTSTACK
    {
	/* altstack of another threads are allocated in another place */
	rb_thread_t *th = GET_THREAD();
	void *tmp = th->altstack;
	th->altstack = malloc(ALT_STACK_SIZE);
	free(tmp); /* free previously allocated area */
    }
#endif

    objspace->profile.invoke_time = getrusage_time();
    finalizer_table = st_init_numtable();
}

static void
initial_expand_heap(rb_objspace_t *objspace)
{
    size_t min_size = initial_heap_min_slots / HEAP_OBJ_LIMIT;

    if (min_size > heaps_used) {
        add_heap_slots(objspace, min_size - heaps_used);
    }
}

static void
set_heaps_increment(rb_objspace_t *objspace)
{
    size_t next_heaps_length = (size_t)(heaps_used * heap_slots_growth_factor);
    size_t next_heaps_length_alt = heaps_used + heap_slots_increment;

    if (next_heaps_length < next_heaps_length_alt) {
        next_heaps_length = next_heaps_length_alt;
    }

    heaps_inc = next_heaps_length - heaps_used;
    /*
    if (gc_statistics & verbose_gc_stats)
	fprintf(gc_data_file, "heaps_inc:%lu, slots_inc: %lu\n", heaps_inc, heaps_inc * HEAP_OBJ_LIMIT);
    */
    if (next_heaps_length > heaps_length) {
	allocate_sorted_heaps(objspace, next_heaps_length);
    }
}

static int
heaps_increment(rb_objspace_t *objspace)
{
    if (heaps_inc > 0) {
        assign_heap_slot(objspace);
	heaps_inc--;
	return TRUE;
    }
    return FALSE;
}

int
rb_during_gc(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    return during_gc;
}

#define RANY(o) ((RVALUE*)(o))
#define has_free_object (objspace->heap.free_slots && objspace->heap.free_slots->freelist)

#ifdef GC_DEBUG
static VALUE
_rb_sourcefile(void)
{
    rb_thread_t *th = GET_THREAD();
    rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(th, th->cfp);

    if (cfp) {
	return cfp->iseq->filename;
    }
    else {
	return Qnil;
    }
}
#endif

VALUE
rb_newobj(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE obj;

    if (UNLIKELY(during_gc)) {
	dont_gc = 1;
	during_gc = 0;
	rb_bug("object allocation during garbage collection phase");
    }

    if (UNLIKELY(ruby_gc_stress && !ruby_disable_gc_stress)) {
	if (!garbage_collect(objspace)) {
	    during_gc = 0;
	    rb_memerror();
	}
    }

    if (UNLIKELY(!has_free_object)) {
	if (!gc_lazy_sweep(objspace)) {
	    during_gc = 0;
	    rb_memerror();
	}
    }

    obj = (VALUE)objspace->heap.free_slots->freelist;
    objspace->heap.free_slots->freelist = RANY(obj)->as.free.next;
    if (objspace->heap.free_slots->freelist == NULL) {
        unlink_free_heap_slot(objspace, objspace->heap.free_slots);
    }

    MEMZERO((void*)obj, RVALUE, 1);
#ifdef GC_DEBUG
    RANY(obj)->file = _rb_sourcefile();
    RANY(obj)->line = rb_sourceline();
#endif
    live_objects++;
    allocated_objects++;
    GC_PROF_INC_LIVE_NUM;

    return obj;
}

NODE*
rb_node_newnode(enum node_type type, VALUE a0, VALUE a1, VALUE a2)
{
    NODE *n = (NODE*)rb_newobj();

    n->flags |= T_NODE;
    nd_set_type(n, type);

    n->u1.value = a0;
    n->u2.value = a1;
    n->u3.value = a2;

    return n;
}

VALUE
rb_data_object_alloc(VALUE klass, void *datap, RUBY_DATA_FUNC dmark, RUBY_DATA_FUNC dfree)
{
    NEWOBJ(data, struct RData);
    if (klass) Check_Type(klass, T_CLASS);
    OBJSETUP(data, klass, T_DATA);
    data->data = datap;
    data->dfree = dfree;
    data->dmark = dmark;

    return (VALUE)data;
}

VALUE
rb_data_typed_object_alloc(VALUE klass, void *datap, const rb_data_type_t *type)
{
    NEWOBJ(data, struct RTypedData);

    if (klass) Check_Type(klass, T_CLASS);

    OBJSETUP(data, klass, T_DATA);

    data->data = datap;
    data->typed_flag = 1;
    data->type = type;

    return (VALUE)data;
}

size_t
rb_objspace_data_type_memsize(VALUE obj)
{
    if (RTYPEDDATA_P(obj) && RTYPEDDATA_TYPE(obj)->function.dsize) {
	return RTYPEDDATA_TYPE(obj)->function.dsize(RTYPEDDATA_DATA(obj));
    }
    else {
	return 0;
    }
}

const char *
rb_objspace_data_type_name(VALUE obj)
{
    if (RTYPEDDATA_P(obj)) {
	return RTYPEDDATA_TYPE(obj)->wrap_struct_name;
    }
    else {
	return 0;
    }
}

#ifdef __ia64
#define SET_STACK_END (SET_MACHINE_STACK_END(&th->machine_stack_end), th->machine_register_stack_end = rb_ia64_bsp())
#else
#define SET_STACK_END SET_MACHINE_STACK_END(&th->machine_stack_end)
#endif

#define STACK_START (th->machine_stack_start)
#define STACK_END (th->machine_stack_end)
#define STACK_LEVEL_MAX (th->machine_stack_maxsize/sizeof(VALUE))

#if STACK_GROW_DIRECTION < 0
# define STACK_LENGTH  (size_t)(STACK_START - STACK_END)
#elif STACK_GROW_DIRECTION > 0
# define STACK_LENGTH  (size_t)(STACK_END - STACK_START + 1)
#else
# define STACK_LENGTH  ((STACK_END < STACK_START) ? (size_t)(STACK_START - STACK_END) \
			: (size_t)(STACK_END - STACK_START + 1))
#endif
#if !STACK_GROW_DIRECTION
int ruby_stack_grow_direction;
int
ruby_get_stack_grow_direction(volatile VALUE *addr)
{
    VALUE *end;
    SET_MACHINE_STACK_END(&end);

    if (end > addr) return ruby_stack_grow_direction = 1;
    return ruby_stack_grow_direction = -1;
}
#endif

/* Marking stack */

static void push_mark_stack(mark_stack_t *, VALUE);
static int pop_mark_stack(mark_stack_t *, VALUE *);
static void shrink_stack_chunk_cache(mark_stack_t *stack);

static stack_chunk_t *
stack_chunk_alloc(void)
{
    stack_chunk_t *res;

    res = malloc(sizeof(stack_chunk_t));
    if (!res)
        rb_memerror();

    return res;
}

static inline int
is_mark_stask_empty(mark_stack_t *stack)
{
    return stack->chunk == NULL;
}

static void
add_stack_chunk_cache(mark_stack_t *stack, stack_chunk_t *chunk)
{
    chunk->next = stack->cache;
    stack->cache = chunk;
    stack->cache_size++;
}

static void
shrink_stack_chunk_cache(mark_stack_t *stack)
{
    stack_chunk_t *chunk;

    if (stack->unused_cache_size > (stack->cache_size/2)) {
        chunk = stack->cache;
        stack->cache = stack->cache->next;
        stack->cache_size--;
        free(chunk);
    }
    stack->unused_cache_size = stack->cache_size;
}

static void
push_mark_stack_chunk(mark_stack_t *stack)
{
    stack_chunk_t *next;

    if (stack->cache_size > 0) {
        next = stack->cache;
        stack->cache = stack->cache->next;
        stack->cache_size--;
        if (stack->unused_cache_size > stack->cache_size)
            stack->unused_cache_size = stack->cache_size;
    }
    else {
        next = stack_chunk_alloc();
    }
    next->next = stack->chunk;
    stack->chunk = next;
    stack->index = 0;
}

static void
pop_mark_stack_chunk(mark_stack_t *stack)
{
    stack_chunk_t *prev;

    prev = stack->chunk->next;
    add_stack_chunk_cache(stack, stack->chunk);
    stack->chunk = prev;
    stack->index = stack->limit;
}

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
static void
free_stack_chunks(mark_stack_t *stack)
{
    stack_chunk_t *chunk = stack->chunk;
    stack_chunk_t *next = NULL;

    while (chunk != NULL) {
        next = chunk->next;
        free(chunk);
        chunk = next;
    }
}
#endif

static void
push_mark_stack(mark_stack_t *stack, VALUE data)
{
    if (stack->index == stack->limit) {
        push_mark_stack_chunk(stack);
    }
    stack->chunk->data[stack->index++] = data;
}

static int
pop_mark_stack(mark_stack_t *stack, VALUE *data)
{
    if (is_mark_stask_empty(stack)) {
        return FALSE;
    }
    if (stack->index == 1) {
        *data = stack->chunk->data[--stack->index];
        pop_mark_stack_chunk(stack);
        return TRUE;
    }
    *data = stack->chunk->data[--stack->index];
    return TRUE;
}

static void
init_mark_stack(mark_stack_t *stack)
{
    int i;

    push_mark_stack_chunk(stack);
    stack->limit = STACK_CHUNK_SIZE;

    for(i=0; i < 4; i++) {
        add_stack_chunk_cache(stack, stack_chunk_alloc());
    }
    stack->unused_cache_size = stack->cache_size;
}


size_t
ruby_stack_length(VALUE **p)
{
    rb_thread_t *th = GET_THREAD();
    SET_STACK_END;
    if (p) *p = STACK_UPPER(STACK_END, STACK_START, STACK_END);
    return STACK_LENGTH;
}

#if !(defined(POSIX_SIGNAL) && defined(SIGSEGV) && defined(HAVE_SIGALTSTACK))
static int
stack_check(int water_mark)
{
    int ret;
    rb_thread_t *th = GET_THREAD();
    SET_STACK_END;
    ret = STACK_LENGTH > STACK_LEVEL_MAX - water_mark;
#ifdef __ia64
    if (!ret) {
        ret = (VALUE*)rb_ia64_bsp() - th->machine_register_stack_start >
              th->machine_register_stack_maxsize/sizeof(VALUE) - water_mark;
    }
#endif
    return ret;
}
#endif

#define STACKFRAME_FOR_CALL_CFUNC 512

int
ruby_stack_check(void)
{
#if defined(POSIX_SIGNAL) && defined(SIGSEGV) && defined(HAVE_SIGALTSTACK)
    return 0;
#else
    return stack_check(STACKFRAME_FOR_CALL_CFUNC);
#endif
}

#define MARK_STACK_EMPTY (mark_stack_ptr == mark_stack)

static void gc_mark(rb_objspace_t *objspace, VALUE ptr);
static void gc_mark_children(rb_objspace_t *objspace, VALUE ptr);

static void
gc_mark_stacked_objects(rb_objspace_t *objspace)
{
    mark_stack_t *mstack = &objspace->mark_stack;
    VALUE obj = 0;

    if (!mstack->index) return;
    while (pop_mark_stack(mstack, &obj)) {
        gc_mark_children(objspace, obj);
    }
    shrink_stack_chunk_cache(mstack);
}

static inline int
is_pointer_to_heap(rb_objspace_t *objspace, void *ptr)
{
    register RVALUE *p = RANY(ptr);
    register struct heaps_header *heap;
    register size_t hi, lo, mid;

    if (p < lomem || p > himem) return FALSE;
    if ((VALUE)p % sizeof(RVALUE) != 0) return FALSE;
    heap = GET_HEAP_HEADER(p);

    /* check if p looks like a pointer using bsearch*/
    lo = 0;
    hi = heaps_used;
    while (lo < hi) {
	mid = (lo + hi) / 2;
        if (heap > objspace->heap.sorted[mid]) {
            lo = mid + 1;
        }
        else if (heap < objspace->heap.sorted[mid]) {
            hi = mid;
        }
        else {
            return (p >= heap->start && p < heap->end) ? TRUE : FALSE;
        }
    }
    return FALSE;
}

static void
mark_locations_array(rb_objspace_t *objspace, register VALUE *x, register long n)
{
    VALUE v;
    while (n--) {
        v = *x;
        VALGRIND_MAKE_MEM_DEFINED(&v, sizeof(v));
	if (is_pointer_to_heap(objspace, (void *)v)) {
	    gc_mark(objspace, v);
	}
	x++;
    }
}

static void
gc_mark_locations(rb_objspace_t *objspace, VALUE *start, VALUE *end)
{
    long n;

    if (end <= start) return;
    n = end - start;
    mark_locations_array(objspace, start, n);
}

void
rb_gc_mark_locations(VALUE *start, VALUE *end)
{
    gc_mark_locations(&rb_objspace, start, end);
}

#define rb_gc_mark_locations(start, end) gc_mark_locations(objspace, (start), (end))

struct mark_tbl_arg {
    rb_objspace_t *objspace;
};

static int
mark_entry(st_data_t key, st_data_t value, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, (VALUE)value);
    return ST_CONTINUE;
}

static void
mark_tbl(rb_objspace_t *objspace, st_table *tbl)
{
    struct mark_tbl_arg arg;
    if (!tbl || tbl->num_entries == 0) return;
    arg.objspace = objspace;
    st_foreach(tbl, mark_entry, (st_data_t)&arg);
}

static void
mark_sa_tbl(rb_objspace_t *objspace, sa_table *tbl)
{
    if (!tbl) return;
    SA_FOREACH_START(tbl);
    gc_mark(objspace, (VALUE)value);
    SA_FOREACH_END();
}

static int
mark_key(st_data_t key, st_data_t value, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, (VALUE)key);
    return ST_CONTINUE;
}

static void
mark_set(rb_objspace_t *objspace, st_table *tbl)
{
    struct mark_tbl_arg arg;
    if (!tbl) return;
    arg.objspace = objspace;
    st_foreach(tbl, mark_key, (st_data_t)&arg);
}

void
rb_mark_set(st_table *tbl)
{
    mark_set(&rb_objspace, tbl);
}

static int
mark_keyvalue(st_data_t key, st_data_t value, st_data_t data)
{
    struct mark_tbl_arg *arg = (void*)data;
    gc_mark(arg->objspace, (VALUE)key);
    gc_mark(arg->objspace, (VALUE)value);
    return ST_CONTINUE;
}

static void
mark_hash(rb_objspace_t *objspace, st_table *tbl)
{
    struct mark_tbl_arg arg;
    if (!tbl) return;
    arg.objspace = objspace;
    st_foreach(tbl, mark_keyvalue, (st_data_t)&arg);
}

void
rb_mark_hash(st_table *tbl)
{
    mark_hash(&rb_objspace, tbl);
}

static void
mark_method_entry(rb_objspace_t *objspace, const rb_method_entry_t *me)
{
    const rb_method_definition_t *def = me->def;

    gc_mark(objspace, me->klass);
    if (!def) return;
    switch (def->type) {
      case VM_METHOD_TYPE_ISEQ:
	gc_mark(objspace, def->body.iseq->self);
	break;
      case VM_METHOD_TYPE_BMETHOD:
	gc_mark(objspace, def->body.proc);
	break;
      case VM_METHOD_TYPE_ATTRSET:
      case VM_METHOD_TYPE_IVAR:
	gc_mark(objspace, def->body.attr.location);
	break;
      default:
	break; /* ignore */
    }
}

void
rb_mark_method_entry(const rb_method_entry_t *me)
{
    mark_method_entry(&rb_objspace, me);
}

static void
mark_m_tbl(rb_objspace_t *objspace, sa_table *tbl)
{
    SA_FOREACH_START(tbl);
    mark_method_entry(objspace, (const rb_method_entry_t*)value);
    SA_FOREACH_END();
}

void
rb_free_m_table(sa_table *tbl)
{
    SA_FOREACH_START(tbl);
    if (!((rb_method_entry_t*)value)->mark) {
	rb_free_method_entry((rb_method_entry_t*)value);
    }
    SA_FOREACH_END();
    sa_clear(tbl);
}

static void
mark_const_tbl(rb_objspace_t *objspace, sa_table *tbl)
{
    SA_FOREACH_START(tbl);
    gc_mark(objspace, ((const rb_const_entry_t*)value)->value);
    SA_FOREACH_END();
}

void
rb_free_const_table(sa_table *tbl)
{
    SA_FOREACH_START(tbl);
    xfree((rb_const_entry_t*)value);
    SA_FOREACH_END();
    sa_clear(tbl);
}

void
rb_mark_tbl(st_table *tbl)
{
    mark_tbl(&rb_objspace, tbl);
}

void
rb_mark_sa_tbl(sa_table *tbl)
{
    mark_sa_tbl(&rb_objspace, tbl);
}

void
rb_gc_mark_maybe(VALUE obj)
{
    if (is_pointer_to_heap(&rb_objspace, (void *)obj)) {
	gc_mark(&rb_objspace, obj);
    }
}

static int
gc_mark_ptr(rb_objspace_t *objspace, VALUE ptr)
{
    register uintptr_t *bits = GET_HEAP_BITMAP(ptr);
    if (MARKED_IN_BITMAP(bits, ptr)) return 0;
    MARK_IN_BITMAP(bits, ptr);
    objspace->heap.live_num++;
    return 1;
}

static void
gc_mark(rb_objspace_t *objspace, VALUE ptr)
{
    register RVALUE *obj;

    obj = RANY(ptr);
    if (rb_special_const_p(ptr)) return; /* special const not marked */
    if (obj->as.basic.flags == 0) return;       /* free cell */
    if (!gc_mark_ptr(objspace, ptr)) return;    /* already marked */

    push_mark_stack(&objspace->mark_stack, ptr);
}

void
rb_gc_mark(VALUE ptr)
{
    gc_mark(&rb_objspace, ptr);
}

static void
gc_mark_children(rb_objspace_t *objspace, VALUE ptr)
{
    register RVALUE *obj = RANY(ptr);
    register uintptr_t *bits;

#ifdef GC_DEBUG
    if (obj->file && obj->file != Qnil && is_pointer_to_heap(objspace, (void*)obj->file)) {
	gc_mark(objspace, obj->file);
    }
#endif

    goto marking;		/* skip */

  again:
    obj = RANY(ptr);
    if (rb_special_const_p(ptr)) return; /* special const not marked */
    if (obj->as.basic.flags == 0) return;       /* free cell */
    bits = GET_HEAP_BITMAP(ptr);
    if (MARKED_IN_BITMAP(bits, ptr)) return;  /* already marked */
    MARK_IN_BITMAP(bits, ptr);
    objspace->heap.live_num++;

#ifdef GC_DEBUG
    if (obj->file && obj->file != Qnil && is_pointer_to_heap(objspace, (void*)obj->file)) {
	gc_mark(objspace, obj->file);
    }
#endif

  marking:
    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_mark_generic_ivar(ptr);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
	rb_bug("rb_gc_mark() called for broken object");
	break;

      case T_NODE:
	switch (nd_type(obj)) {
	  case NODE_IF:		/* 1,2,3 */
	  case NODE_FOR:
	  case NODE_ITER:
	  case NODE_WHEN:
	  case NODE_MASGN:
	  case NODE_RESCUE:
	  case NODE_RESBODY:
	  case NODE_CLASS:
	  case NODE_BLOCK_PASS:
	    gc_mark(objspace, (VALUE)obj->as.node.u2.node);
	    /* fall through */
	  case NODE_BLOCK:	/* 1,3 */
	  case NODE_OPTBLOCK:
	  case NODE_ARRAY:
	  case NODE_DSTR:
	  case NODE_DXSTR:
	  case NODE_DREGX:
	  case NODE_DREGX_ONCE:
	  case NODE_ENSURE:
	  case NODE_CALL:
	  case NODE_DEFS:
	  case NODE_OP_ASGN1:
	  case NODE_ARGS:
	    gc_mark(objspace, (VALUE)obj->as.node.u1.node);
	    /* fall through */
	  case NODE_SUPER:	/* 3 */
	  case NODE_FCALL:
	  case NODE_DEFN:
	  case NODE_ARGS_AUX:
	    ptr = (VALUE)obj->as.node.u3.node;
	    goto again;

	  case NODE_WHILE:	/* 1,2 */
	  case NODE_UNTIL:
	  case NODE_AND:
	  case NODE_OR:
	  case NODE_CASE:
	  case NODE_SCLASS:
	  case NODE_DOT2:
	  case NODE_DOT3:
	  case NODE_FLIP2:
	  case NODE_FLIP3:
	  case NODE_MATCH2:
	  case NODE_MATCH3:
	  case NODE_OP_ASGN_OR:
	  case NODE_OP_ASGN_AND:
	  case NODE_MODULE:
	  case NODE_ALIAS:
	  case NODE_VALIAS:
	  case NODE_ARGSCAT:
	    gc_mark(objspace, (VALUE)obj->as.node.u1.node);
	    /* fall through */
	  case NODE_GASGN:	/* 2 */
	  case NODE_LASGN:
	  case NODE_DASGN:
	  case NODE_DASGN_CURR:
	  case NODE_IASGN:
	  case NODE_IASGN2:
	  case NODE_CVASGN:
	  case NODE_COLON3:
	  case NODE_OPT_N:
	  case NODE_EVSTR:
	  case NODE_UNDEF:
	  case NODE_POSTEXE:
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_HASH:	/* 1 */
	  case NODE_LIT:
	  case NODE_STR:
	  case NODE_XSTR:
	  case NODE_DEFINED:
	  case NODE_MATCH:
	  case NODE_RETURN:
	  case NODE_BREAK:
	  case NODE_NEXT:
	  case NODE_YIELD:
	  case NODE_COLON2:
	  case NODE_SPLAT:
	  case NODE_TO_ARY:
	    ptr = (VALUE)obj->as.node.u1.node;
	    goto again;

	  case NODE_SCOPE:	/* 2,3 */
	  case NODE_CDECL:
	  case NODE_OPT_ARG:
	    gc_mark(objspace, (VALUE)obj->as.node.u3.node);
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  case NODE_ZARRAY:	/* - */
	  case NODE_ZSUPER:
	  case NODE_VCALL:
	  case NODE_GVAR:
	  case NODE_LVAR:
	  case NODE_DVAR:
	  case NODE_IVAR:
	  case NODE_CVAR:
	  case NODE_NTH_REF:
	  case NODE_BACK_REF:
	  case NODE_REDO:
	  case NODE_RETRY:
	  case NODE_SELF:
	  case NODE_NIL:
	  case NODE_TRUE:
	  case NODE_FALSE:
	  case NODE_ERRINFO:
	  case NODE_BLOCK_ARG:
	    break;
	  case NODE_ALLOCA:
	    mark_locations_array(objspace,
				 (VALUE*)obj->as.node.u1.value,
				 obj->as.node.u3.cnt);
	    ptr = (VALUE)obj->as.node.u2.node;
	    goto again;

	  default:		/* unlisted NODE */
	    if (is_pointer_to_heap(objspace, obj->as.node.u1.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u1.node);
	    }
	    if (is_pointer_to_heap(objspace, obj->as.node.u2.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u2.node);
	    }
	    if (is_pointer_to_heap(objspace, obj->as.node.u3.node)) {
		gc_mark(objspace, (VALUE)obj->as.node.u3.node);
	    }
	}
	return;			/* no need to mark class. */
    }

    gc_mark(objspace, obj->as.basic.klass);
    switch (BUILTIN_TYPE(obj)) {
      case T_ICLASS:
      case T_CLASS:
      case T_MODULE:
	mark_m_tbl(objspace, RCLASS_M_TBL(obj));
	if (!RCLASS_EXT(obj)) break;
	mark_sa_tbl(objspace, RCLASS_IV_TBL(obj));
	mark_const_tbl(objspace, RCLASS_CONST_TBL(obj));
	ptr = RCLASS_SUPER(obj);
	goto again;

      case T_ARRAY:
	if (FL_TEST(obj, ELTS_SHARED)) {
	    ptr = obj->as.array.as.heap.aux.shared;
	    goto again;
	}
	else {
	    long i, len = RARRAY_LEN(obj);
	    VALUE *ptr = RARRAY_PTR(obj);
	    for (i=0; i < len; i++) {
		gc_mark(objspace, *ptr++);
	    }
	}
	break;

      case T_HASH:
	mark_hash(objspace, obj->as.hash.ntbl);
	ptr = obj->as.hash.ifnone;
	goto again;

      case T_STRING:
#define STR_ASSOC FL_USER3   /* copied from string.c */
	if (FL_TEST(obj, RSTRING_NOEMBED) && FL_ANY(obj, ELTS_SHARED|STR_ASSOC)) {
	    ptr = obj->as.string.as.heap.aux.shared;
	    goto again;
	}
	break;

      case T_DATA:
	if (RTYPEDDATA_P(obj)) {
	    RUBY_DATA_FUNC mark_func = obj->as.typeddata.type->function.dmark;
	    if (mark_func) (*mark_func)(DATA_PTR(obj));
	}
	else {
	    if (obj->as.data.dmark) (*obj->as.data.dmark)(DATA_PTR(obj));
	}
	break;

      case T_OBJECT:
        {
            long i, len = ROBJECT_NUMIV(obj);
	    VALUE *ptr = ROBJECT_IVPTR(obj);
            for (i  = 0; i < len; i++) {
		gc_mark(objspace, *ptr++);
            }
        }
	break;

      case T_FILE:
        if (obj->as.file.fptr) {
            gc_mark(objspace, obj->as.file.fptr->pathv);
            gc_mark(objspace, obj->as.file.fptr->tied_io_for_writing);
            gc_mark(objspace, obj->as.file.fptr->writeconv_asciicompat);
            gc_mark(objspace, obj->as.file.fptr->writeconv_pre_ecopts);
            gc_mark(objspace, obj->as.file.fptr->encs.ecopts);
            gc_mark(objspace, obj->as.file.fptr->write_lock);
        }
        break;

      case T_REGEXP:
        ptr = obj->as.regexp.src;
        goto again;

      case T_FLOAT:
      case T_BIGNUM:
      case T_ZOMBIE:
	break;

      case T_MATCH:
	gc_mark(objspace, obj->as.match.regexp);
	if (obj->as.match.str) {
	    ptr = obj->as.match.str;
	    goto again;
	}
	break;

      case T_RATIONAL:
	gc_mark(objspace, obj->as.rational.num);
	ptr = obj->as.rational.den;
	goto again;

      case T_COMPLEX:
	gc_mark(objspace, obj->as.complex.real);
	ptr = obj->as.complex.imag;
	goto again;

      case T_STRUCT:
	{
	    long len = RSTRUCT_LEN(obj);
	    VALUE *ptr = RSTRUCT_PTR(obj);

	    while (len--) {
		gc_mark(objspace, *ptr++);
	    }
	}
	break;

      default:
	rb_bug("rb_gc_mark(): unknown data type 0x%x(%p) %s",
	       BUILTIN_TYPE(obj), (void *)obj,
	       is_pointer_to_heap(objspace, obj) ? "corrupted object" : "non object");
    }
}

static int obj_free(rb_objspace_t *, VALUE);

static inline struct heaps_slot *
add_slot_local_freelist(rb_objspace_t *objspace, RVALUE *p)
{
    struct heaps_slot *slot;

    VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
    p->as.free.flags = 0;
    slot = GET_HEAP_SLOT(p);
    p->as.free.next = slot->freelist;
    slot->freelist = p;

    return slot;
}

static void
finalize_list(rb_objspace_t *objspace, RVALUE *p)
{
    while (p) {
	RVALUE *tmp = p->as.free.next;
	run_final(objspace, (VALUE)p);
	if (!FL_TEST(p, FL_SINGLETON)) { /* not freeing page */
            add_slot_local_freelist(objspace, p);
            if (!is_lazy_sweeping(objspace)) {
                GC_PROF_DEC_LIVE_NUM;
            }
	}
	else {
            GET_HEAP_HEADER(p)->limit--;
	}
	p = tmp;
    }
}

static void
unlink_heap_slot(rb_objspace_t *objspace, struct heaps_slot *slot)
{
    if (slot->prev)
        slot->prev->next = slot->next;
    if (slot->next)
        slot->next->prev = slot->prev;
    if (heaps == slot)
        heaps = slot->next;
    if (objspace->heap.sweep_slots == slot)
        objspace->heap.sweep_slots = slot->next;
    slot->prev = NULL;
    slot->next = NULL;
}

static void
free_unused_heaps(rb_objspace_t *objspace)
{
    size_t i, j;
    struct heaps_header *last = 0;

    for (i = j = 1; j < heaps_used; i++) {
    if (objspace->heap.sorted[i]->limit == 0) {
            struct heaps_slot* h = objspace->heap.sorted[i]->base;
            h->free_next = objspace->heap.reserve_slots;
            objspace->heap.reserve_slots = h;
	    if (!last) {
                last = objspace->heap.sorted[i];
	    }
	    else {
	        aligned_free(objspace->heap.sorted[i]);
	    }
	    heaps_used--;
	}
	else {
	    if (i != j) {
		objspace->heap.sorted[j] = objspace->heap.sorted[i];
	    }
	    j++;
	}
    }
    if (last) {
	if (last < heaps_freed) {
        aligned_free(heaps_freed);
	    heaps_freed = last;
	}
	else {
        aligned_free(last);
	}
    }
}

static inline unsigned long
elapsed_musecs(struct timeval since)
{
    struct timeval now;
    struct timeval temp;

    gettimeofday(&now, NULL);

    if ((now.tv_usec-since.tv_usec)<0) {
        temp.tv_sec = now.tv_sec-since.tv_sec-1;
        temp.tv_usec = 1000000+now.tv_usec-since.tv_usec;
    } else {
        temp.tv_sec = now.tv_sec-since.tv_sec;
        temp.tv_usec = now.tv_usec-since.tv_usec;
    }

    return temp.tv_sec*1000000 + temp.tv_usec;
}

static void
gc_clear_slot_bits(struct heaps_slot *slot)
{
    memset(slot->bits, 0, HEAP_BITMAP_LIMIT * sizeof(uintptr_t));
}

static void
slot_sweep(rb_objspace_t *objspace, struct heaps_slot *sweep_slot)
{
    size_t free_num = 0, final_num = 0;
    RVALUE *p, *pend;
    RVALUE *final = deferred_final_list;
    int deferred;
    uintptr_t *bits;
    int do_gc_stats = gc_statistics & verbose_gc_stats;

    struct timeval tv1;
    if (gc_statistics) gettimeofday(&tv1, NULL);

    p = sweep_slot->membase->start; pend = sweep_slot->membase->end;
    bits = sweep_slot->bits;
    while (p < pend) {
        if ((!(MARKED_IN_BITMAP(bits, p))) && BUILTIN_TYPE(p) != T_ZOMBIE) {
            if (p->as.basic.flags) {
                if ((deferred = obj_free(objspace, (VALUE)p)) ||
                    (FL_TEST(p, FL_FINALIZE))) {
                    if (!deferred) {
                        p->as.free.flags = T_ZOMBIE;
                        RDATA(p)->dfree = 0;
                    }
                    p->as.free.next = deferred_final_list;
                    deferred_final_list = p;
                    assert(BUILTIN_TYPE(p) == T_ZOMBIE);
                    final_num++;
                }
                else {
                    VALGRIND_MAKE_MEM_UNDEFINED((void*)p, sizeof(RVALUE));
                    p->as.free.flags = 0;
                    p->as.free.next = sweep_slot->freelist;
                    sweep_slot->freelist = p;
                    free_num++;
                }
            }
            else {
                if (do_gc_stats) {
                    VALUE obt = p->as.basic.flags & T_MASK;
                    if (obt) free_counts[obt]++;
                }
                free_num++;
            }
        }
        else if (BUILTIN_TYPE(p) == T_ZOMBIE) {
            /* objects to be finalized */
            /* do nothing remain marked */
            if (do_gc_stats) zombies++;
        }
        else {
            // JOEL TODO: DELETE THIS TOO???
	    if (do_gc_stats) {
	       live_counts[p->as.basic.flags & T_MASK]++;
	    }
        }
        p++;
        processed++;
    }

    freed_objects += free_num;

    gc_clear_slot_bits(sweep_slot);
    if (objspace->heap.freed_blocks < objspace->heap.max_blocks_to_free &&
        final_num + free_num == sweep_slot->membase->limit &&
        objspace->heap.free_num > objspace->heap.do_heap_free) {
        RVALUE *pp;

        for (pp = deferred_final_list; pp != final; pp = pp->as.free.next) {
	    RDATA(pp)->dmark = 0;
            pp->as.free.flags |= FL_SINGLETON; /* freeing page mark */
        }
        sweep_slot->membase->limit = final_num;
        unlink_heap_slot(objspace, sweep_slot);
        objspace->heap.freed_blocks += 1;
        heap_size -= final_num + free_num;
    }
    else {
        if (free_num > 0) {
            link_free_heap_slot(objspace, sweep_slot);
        }
        else {
            sweep_slot->free_next = NULL;
        }
        objspace->heap.free_num += free_num;
    }
    objspace->heap.final_num += final_num;

    if (deferred_final_list && !finalizing) {
        rb_thread_t *th = GET_THREAD();
        if (th) {
            RUBY_VM_SET_FINALIZER_INTERRUPT(th);
        }
    }

    if (gc_statistics) {
	gc_time_accumulator += elapsed_musecs(tv1);
    }
}

static int
ready_to_gc(rb_objspace_t *objspace)
{
    if (dont_gc || during_gc) {
	if (!has_free_object) {
            if (!heaps_increment(objspace)) {
                set_heaps_increment(objspace);
                heaps_increment(objspace);
            }
	}
	return FALSE;
    }
    return TRUE;
}

static void
before_gc_sweep(rb_objspace_t *objspace)
{
    if (gc_statistics & verbose_gc_stats) {
        /*
	fprintf(gc_data_file, "Sweep started\n");
        */
        freed_objects = 0;
        processed = 0;
        zombies = 0;
        freelist_size = 0;
        MEMZERO((void*)free_counts, unsigned long, T_MASK+1);
        MEMZERO((void*)live_counts, unsigned long, T_MASK+1);
    }

    objspace->heap.max_blocks_to_free = heaps_used - (initial_heap_min_slots / HEAP_OBJ_LIMIT);
    objspace->heap.freed_blocks = 0;

    objspace->heap.do_heap_free = (size_t)((heaps_used * HEAP_OBJ_LIMIT) * 0.65);
    objspace->heap.free_min = (size_t)((heaps_used * HEAP_OBJ_LIMIT)  * 0.2);
    if (objspace->heap.free_min < initial_free_min) {
        /* objspace->heap.do_heap_free = heaps_used * HEAP_OBJ_LIMIT; */
        objspace->heap.free_min = initial_free_min;
    }
    objspace->heap.sweep_slots = heaps;
    objspace->heap.free_num = 0;
    objspace->heap.free_slots = NULL;

    /* sweep unlinked method entries */
    if (GET_VM()->unlinked_method_entry_list) {
	rb_sweep_method_entry(GET_VM());
    }
}

static void
after_gc_sweep(rb_objspace_t *objspace)
{
    int i;
    struct timeval tv1;

    GC_PROF_SET_MALLOC_INFO;

    if (gc_statistics) gettimeofday(&tv1, NULL);

    if (objspace->heap.free_num < objspace->heap.free_min) {
        set_heaps_increment(objspace);
        heaps_increment(objspace);
    }

    if (malloc_increase > malloc_limit) {
	malloc_limit += (size_t)((malloc_increase - malloc_limit) * (double)objspace->heap.live_num / (heaps_used * HEAP_OBJ_LIMIT));
	if (malloc_limit < initial_malloc_limit) malloc_limit = initial_malloc_limit;
    }
    malloc_increase = 0;
    /*
    if (verbose_gc_stats)
        fprintf(gc_data_file, "heap size before freeing unused heaps: %7lu\n",
                (unsigned long)heaps_used*HEAP_OBJ_LIMIT);
    */
    free_unused_heaps(objspace);

    if (gc_statistics) {
	gc_time_accumulator += elapsed_musecs(tv1);

        if (verbose_gc_stats) {
            /* log gc stats if requested */
            fprintf(gc_data_file, "GC time: %lu musec\n", (unsigned long)(gc_time_accumulator-gc_time_accumulator_before_gc));
            fprintf(gc_data_file, "heap size        : %7lu\n", (unsigned long)heaps_used*HEAP_OBJ_LIMIT);
            fprintf(gc_data_file, "objects processed: %7lu\n", (unsigned long)processed);
            fprintf(gc_data_file, "live objects     : %7lu\n", (unsigned long)live_after_last_mark_phase);
            fprintf(gc_data_file, "freelist objects : %7lu\n", (unsigned long)freelist_size);
            fprintf(gc_data_file, "freed objects    : %7lu\n", (unsigned long)freed_objects);
            fprintf(gc_data_file, "zombies          : %7lu\n", (unsigned long)zombies);
            for(i=0; i<T_MASK; i++) {
                if (free_counts[i]>0 || live_counts[i]>0) {
                    fprintf(gc_data_file,
                            "kept %7lu / freed %7lu objects of type %s\n",
                            (unsigned long)live_counts[i], (unsigned long)free_counts[i], obj_type((int)i));
                }
            }
            rb_gc_dump();
            fflush(gc_data_file);
        }
    }
}

static int
lazy_sweep(rb_objspace_t *objspace)
{
    struct heaps_slot *next;

    heaps_increment(objspace);
    while (objspace->heap.sweep_slots) {
        next = objspace->heap.sweep_slots->next;
	slot_sweep(objspace, objspace->heap.sweep_slots);
        objspace->heap.sweep_slots = next;
        if (has_free_object) {
            during_gc = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void
rest_sweep(rb_objspace_t *objspace)
{
    if (objspace->heap.sweep_slots) {
    while (objspace->heap.sweep_slots) {
        lazy_sweep(objspace);
    }
    after_gc_sweep(objspace);
    }
}

static void gc_marks(rb_objspace_t *objspace);

/* only called from rb_newobj */
static int
gc_lazy_sweep(rb_objspace_t *objspace)
{
    struct timeval gctv1;
    int res;
    INIT_GC_PROF_PARAMS;

    if (objspace->flags.dont_lazy_sweep)
        return garbage_collect(objspace);


    if (!ready_to_gc(objspace)) return TRUE;

    during_gc++;
    GC_PROF_TIMER_START;
    if (RUBY_DTRACE_GC_SWEEP_BEGIN_ENABLED()) {
        RUBY_DTRACE_GC_SWEEP_BEGIN();
    }
    GC_PROF_SWEEP_TIMER_START;

    if (objspace->heap.sweep_slots) {
        res = lazy_sweep(objspace);
        if (res) {
            if (RUBY_DTRACE_GC_SWEEP_END_ENABLED()) {
                RUBY_DTRACE_GC_SWEEP_END();
            }
            GC_PROF_SWEEP_TIMER_STOP;
            GC_PROF_SET_MALLOC_INFO;
            GC_PROF_TIMER_STOP(Qfalse);
            return res;
        }
    }
    else {
        if (heaps_increment(objspace)) {
            during_gc = 0;
            return TRUE;
        }
    }
    after_gc_sweep(objspace);

    if (gc_statistics) {
        gc_time_accumulator_before_gc = gc_time_accumulator;
	gc_collections++;
	gettimeofday(&gctv1, NULL);
        /*
	if (verbose_gc_stats) {
	    fprintf(gc_data_file, "Garbage collection started (gc_lazy_sweep)\n");
	}
        */
    }

    gc_marks(objspace);

    before_gc_sweep(objspace);
    if (objspace->heap.free_min > (heaps_used * HEAP_OBJ_LIMIT - objspace->heap.live_num)) {
	set_heaps_increment(objspace);
    }

    if (gc_statistics) {
	gc_time_accumulator += elapsed_musecs(gctv1);
    }

    if (RUBY_DTRACE_GC_SWEEP_BEGIN_ENABLED()) {
        RUBY_DTRACE_GC_SWEEP_BEGIN();
    }

    GC_PROF_SWEEP_TIMER_START;
    if (!(res = lazy_sweep(objspace))) {
        after_gc_sweep(objspace);
        if (has_free_object) {
            res = TRUE;
            during_gc = 0;
        }
    }
    if (RUBY_DTRACE_GC_SWEEP_END_ENABLED()) {
   	    RUBY_DTRACE_GC_SWEEP_END();
    }
    GC_PROF_SWEEP_TIMER_STOP;

    GC_PROF_TIMER_STOP(Qtrue);

    return res;
}

static void
gc_sweep(rb_objspace_t *objspace)
{
    struct heaps_slot *next;

    before_gc_sweep(objspace);

    while (objspace->heap.sweep_slots) {
        next = objspace->heap.sweep_slots->next;
	slot_sweep(objspace, objspace->heap.sweep_slots);
        objspace->heap.sweep_slots = next;
    }

    after_gc_sweep(objspace);

    during_gc = 0;
}

void
rb_gc_force_recycle(VALUE p)
{
    rb_objspace_t *objspace = &rb_objspace;
    struct heaps_slot *slot;

    if (MARKED_IN_BITMAP(GET_HEAP_BITMAP(p), p)) {
        add_slot_local_freelist(objspace, (RVALUE *)p);
    }
    else {
        GC_PROF_DEC_LIVE_NUM;
        slot = add_slot_local_freelist(objspace, (RVALUE *)p);
        if (slot->free_next == NULL) {
            link_free_heap_slot(objspace, slot);
        }
    }
}

static inline void
make_deferred(RVALUE *p)
{
    p->as.basic.flags = (p->as.basic.flags & ~T_MASK) | T_ZOMBIE;
}

static inline void
make_io_deferred(RVALUE *p)
{
    rb_io_t *fptr = p->as.file.fptr;
    make_deferred(p);
    p->as.data.dfree = (void (*)(void*))rb_io_fptr_finalize;
    p->as.data.data = fptr;
}

static int
obj_free(rb_objspace_t *objspace, VALUE obj)
{
    switch (BUILTIN_TYPE(obj)) {
      case T_NIL:
      case T_FIXNUM:
      case T_TRUE:
      case T_FALSE:
	rb_bug("obj_free() called for broken object");
	break;
    }

    if (FL_TEST(obj, FL_EXIVAR)) {
	rb_free_generic_ivar((VALUE)obj);
	FL_UNSET(obj, FL_EXIVAR);
    }

    switch (BUILTIN_TYPE(obj)) {
      case T_OBJECT:
	if (!(RANY(obj)->as.basic.flags & ROBJECT_EMBED) &&
            RANY(obj)->as.object.as.heap.ivptr) {
	    xfree(RANY(obj)->as.object.as.heap.ivptr);
	}
	break;
      case T_MODULE:
      case T_CLASS:
	rb_clear_cache_by_class((VALUE)obj);
	rb_free_m_table(RCLASS_M_TBL(obj));
        sa_clear(RCLASS_IV_TBL(obj));
        rb_free_const_table(RCLASS_CONST_TBL(obj));
        sa_clear(RCLASS_IV_INDEX_TBL(obj));
        xfree(RANY(obj)->as.klass.ptr);
	break;
      case T_STRING:
	rb_str_free(obj);
	break;
      case T_ARRAY:
	rb_ary_free(obj);
	break;
      case T_HASH:
	if (RANY(obj)->as.hash.ntbl) {
	    st_free_table(RANY(obj)->as.hash.ntbl);
	}
	break;
      case T_REGEXP:
	if (RANY(obj)->as.regexp.ptr) {
	    onig_free(RANY(obj)->as.regexp.ptr);
	}
	break;
      case T_DATA:
	if (DATA_PTR(obj)) {
	    if (RTYPEDDATA_P(obj)) {
		RDATA(obj)->dfree = RANY(obj)->as.typeddata.type->function.dfree;
	    }
	    if (RANY(obj)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		xfree(DATA_PTR(obj));
	    }
	    else if (RANY(obj)->as.data.dfree) {
		make_deferred(RANY(obj));
		return 1;
	    }
	}
	break;
      case T_MATCH:
	if (RANY(obj)->as.match.rmatch) {
            struct rmatch *rm = RANY(obj)->as.match.rmatch;
	    onig_region_free(&rm->regs, 0);
            if (rm->char_offset)
		xfree(rm->char_offset);
	    xfree(rm);
	}
	break;
      case T_FILE:
	if (RANY(obj)->as.file.fptr) {
	    make_io_deferred(RANY(obj));
	    return 1;
	}
	break;
      case T_RATIONAL:
      case T_COMPLEX:
	break;
      case T_ICLASS:
	/* iClass shares table with the module */
	break;

      case T_FLOAT:
	break;

      case T_BIGNUM:
	if (!(RBASIC(obj)->flags & RBIGNUM_EMBED_FLAG) && RBIGNUM_DIGITS(obj)) {
	    xfree(RBIGNUM_DIGITS(obj));
	}
	break;
      case T_NODE:
	switch (nd_type(obj)) {
	  case NODE_SCOPE:
	    if (RANY(obj)->as.node.u1.tbl) {
		xfree(RANY(obj)->as.node.u1.tbl);
	    }
	    break;
	  case NODE_ALLOCA:
	    xfree(RANY(obj)->as.node.u1.node);
	    break;
	}
	break;			/* no need to free iv_tbl */

      case T_STRUCT:
	if ((RBASIC(obj)->flags & RSTRUCT_EMBED_LEN_MASK) == 0 &&
	    RANY(obj)->as.rstruct.as.heap.ptr) {
	    xfree(RANY(obj)->as.rstruct.as.heap.ptr);
	}
	break;

      default:
	rb_bug("gc_sweep(): unknown data type 0x%x(%p)",
	       BUILTIN_TYPE(obj), (void*)obj);
    }

    return 0;
}

#define GC_NOTIFY 0

#if STACK_GROW_DIRECTION < 0
#define GET_STACK_BOUNDS(start, end, appendix) ((start) = STACK_END, (end) = STACK_START)
#elif STACK_GROW_DIRECTION > 0
#define GET_STACK_BOUNDS(start, end, appendix) ((start) = STACK_START, (end) = STACK_END+(appendix))
#else
#define GET_STACK_BOUNDS(start, end, appendix) \
    ((STACK_END < STACK_START) ? \
     ((start) = STACK_END, (end) = STACK_START) : ((start) = STACK_START, (end) = STACK_END+(appendix)))
#endif

#define numberof(array) (int)(sizeof(array) / sizeof((array)[0]))

static void
mark_current_machine_context(rb_objspace_t *objspace, rb_thread_t *th)
{
    union {
	rb_jmp_buf j;
	VALUE v[sizeof(rb_jmp_buf) / sizeof(VALUE)];
    } save_regs_gc_mark;
    VALUE *stack_start, *stack_end;

    FLUSH_REGISTER_WINDOWS;
    /* This assumes that all registers are saved into the jmp_buf (and stack) */
    rb_setjmp(save_regs_gc_mark.j);

    SET_STACK_END;
    GET_STACK_BOUNDS(stack_start, stack_end, 1);

    mark_locations_array(objspace, save_regs_gc_mark.v, numberof(save_regs_gc_mark.v));

    rb_gc_mark_locations(stack_start, stack_end);
#ifdef __ia64
    rb_gc_mark_locations(th->machine_register_stack_start, th->machine_register_stack_end);
#endif
#if defined(__mc68000__)
    mark_locations_array(objspace, (VALUE*)((char*)STACK_END + 2),
			 (STACK_START - STACK_END));
#endif
}

static void
gc_marks(rb_objspace_t *objspace)
{
    struct gc_list *list;
    rb_thread_t *th = GET_THREAD();
    if (RUBY_DTRACE_GC_MARK_BEGIN_ENABLED()) {
        RUBY_DTRACE_GC_MARK_BEGIN();
    }
    GC_PROF_MARK_TIMER_START;
    /*
    if (gc_statistics & verbose_gc_stats) {
        fprintf(gc_data_file, "Marking objects\n");
    }
    */
    objspace->heap.live_num = 0;
    objspace->count++;
    live_objects = 0;

    SET_STACK_END;

    th->vm->self ? rb_gc_mark(th->vm->self) : rb_vm_mark(th->vm);

    mark_tbl(objspace, finalizer_table);
    mark_current_machine_context(objspace, th);

    rb_gc_mark_symbols();
    rb_gc_mark_encodings();

    /* mark protected global variables */
    for (list = global_List; list; list = list->next) {
	rb_gc_mark_maybe(*list->varptr);
    }
    rb_mark_end_proc();
    rb_gc_mark_global_tbl();

    mark_sa_tbl(objspace, &rb_class_tbl);

    /* mark generic instance variables for special constants */
    rb_mark_generic_ivar_tbl();

    rb_gc_mark_parser();

    rb_gc_mark_unlinked_live_method_entries(th->vm);

    /* marking-loop */
    gc_mark_stacked_objects(objspace);

    if (RUBY_DTRACE_GC_MARK_END_ENABLED()) {
        RUBY_DTRACE_GC_MARK_END();
    }
    GC_PROF_MARK_TIMER_STOP;

    live_after_last_mark_phase = objspace->heap.live_num;
}

static int
garbage_collect(rb_objspace_t *objspace)
{
    struct timeval gctv1;

    INIT_GC_PROF_PARAMS;

    if (GC_NOTIFY) printf("start garbage_collect()\n");

    if (!heaps) {
	return FALSE;
    }
    if (!ready_to_gc(objspace)) {
        return TRUE;
    }

    GC_PROF_TIMER_START;

    rest_sweep(objspace);

    if (gc_statistics) {
        gc_time_accumulator_before_gc = gc_time_accumulator;
	gc_collections++;
	gettimeofday(&gctv1, NULL);
        /*
	if (verbose_gc_stats) {
	    fprintf(gc_data_file, "Garbage collection started (garbage_collect)\n");
	}
        */
    }

    during_gc++;
    gc_marks(objspace);

    if (gc_statistics) {
	gc_time_accumulator += elapsed_musecs(gctv1);
    }

    if (RUBY_DTRACE_GC_SWEEP_BEGIN_ENABLED()) {
        RUBY_DTRACE_GC_SWEEP_BEGIN();
    }
    GC_PROF_SWEEP_TIMER_START;
    gc_sweep(objspace);
    if (RUBY_DTRACE_GC_SWEEP_END_ENABLED()) {
        RUBY_DTRACE_GC_SWEEP_END();
    }
    GC_PROF_SWEEP_TIMER_STOP;

    GC_PROF_TIMER_STOP(Qtrue);
    if (GC_NOTIFY) printf("end garbage_collect()\n");

    return TRUE;
}

int
rb_garbage_collect(void)
{
    return garbage_collect(&rb_objspace);
}

void
rb_gc_mark_machine_stack(rb_thread_t *th)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE *stack_start, *stack_end;

    GET_STACK_BOUNDS(stack_start, stack_end, 0);
    rb_gc_mark_locations(stack_start, stack_end);
#ifdef __ia64
    rb_gc_mark_locations(th->machine_register_stack_start, th->machine_register_stack_end);
#endif
}


/*
 *  call-seq:
 *     GC.start                     -> nil
 *     gc.garbage_collect           -> nil
 *     ObjectSpace.garbage_collect  -> nil
 *
 *  Initiates garbage collection, unless manually disabled.
 *
 */

VALUE
rb_gc_start(void)
{
    rb_gc();
    return Qnil;
}

#undef Init_stack

void
Init_stack(volatile VALUE *addr)
{
    ruby_init_stack(addr);
}

/*
 * Document-class: ObjectSpace
 *
 *  The <code>ObjectSpace</code> module contains a number of routines
 *  that interact with the garbage collection facility and allow you to
 *  traverse all living objects with an iterator.
 *
 *  <code>ObjectSpace</code> also provides support for object
 *  finalizers, procs that will be called when a specific object is
 *  about to be destroyed by garbage collection.
 *
 *     include ObjectSpace
 *
 *
 *     a = "A"
 *     b = "B"
 *     c = "C"
 *
 *
 *     define_finalizer(a, proc {|id| puts "Finalizer one on #{id}" })
 *     define_finalizer(a, proc {|id| puts "Finalizer two on #{id}" })
 *     define_finalizer(b, proc {|id| puts "Finalizer three on #{id}" })
 *
 *  <em>produces:</em>
 *
 *     Finalizer three on 537763470
 *     Finalizer one on 537763480
 *     Finalizer two on 537763480
 *
 */

void
Init_heap(void)
{
    init_heap(&rb_objspace);
}

static VALUE
lazy_sweep_enable(void)
{
    rb_objspace_t *objspace = &rb_objspace;

    objspace->flags.dont_lazy_sweep = FALSE;
    return Qnil;
}

typedef int each_obj_callback(void *, void *, size_t, void *);

struct each_obj_args {
    each_obj_callback *callback;
    void *data;
};

static VALUE
objspace_each_objects(VALUE arg)
{
    size_t i;
    struct heaps_header *membase = 0;
    RVALUE *pstart, *pend;
    rb_objspace_t *objspace = &rb_objspace;
    struct each_obj_args *args = (struct each_obj_args *)arg;
    volatile VALUE v;

    i = 0;
    while (i < heaps_used) {
        while (0 < i && membase < objspace->heap.sorted[i-1])
	    i--;
        while (i < heaps_used && objspace->heap.sorted[i] <= membase)
	    i++;
	if (heaps_used <= i)
	  break;
        membase = objspace->heap.sorted[i];

        pstart = membase->start;
        pend = membase->end;

	for (; pstart != pend; pstart++) {
	    if (pstart->as.basic.flags) {
		v = (VALUE)pstart; /* acquire to save this object */
		break;
	    }
	}
	if (pstart != pend) {
	    if ((*args->callback)(pstart, pend, sizeof(RVALUE), args->data)) {
		break;
	    }
	}
    }
    RB_GC_GUARD(v);

    return Qnil;
}

/*
 * rb_objspace_each_objects() is special C API to walk through
 * Ruby object space.  This C API is too difficult to use it.
 * To be frank, you should not use it. Or you need to read the
 * source code of this function and understand what this function does.
 *
 * 'callback' will be called several times (the number of heap slot,
 * at current implementation) with:
 *   vstart: a pointer to the first living object of the heap_slot.
 *   vend: a pointer to next to the valid heap_slot area.
 *   stride: a distance to next VALUE.
 *
 * If callback() returns non-zero, the iteration will be stopped.
 *
 * This is a sample callback code to iterate liveness objects:
 *
 *   int
 *   sample_callback(void *vstart, void *vend, int stride, void *data) {
 *     VALUE v = (VALUE)vstart;
 *     for (; v != (VALUE)vend; v += stride) {
 *       if (RBASIC(v)->flags) { // liveness check
 *       // do something with live object 'v'
 *     }
 *     return 0; // continue to iteration
 *   }
 *
 * Note: 'vstart' is not a top of heap_slot.  This point the first
 *       living object to grasp at least one object to avoid GC issue.
 *       This means that you can not walk through all Ruby object slot
 *       including freed object slot.
 *
 * Note: On this implementation, 'stride' is same as sizeof(RVALUE).
 *       However, there are possibilities to pass variable values with
 *       'stride' with some reasons.  You must use stride instead of
 *       use some constant value in the iteration.
 */
void
rb_objspace_each_objects(each_obj_callback *callback, void *data)
{
    struct each_obj_args args;
    rb_objspace_t *objspace = &rb_objspace;

    rest_sweep(objspace);
    objspace->flags.dont_lazy_sweep = TRUE;

    args.callback = callback;
    args.data = data;
    rb_ensure(objspace_each_objects, (VALUE)&args, lazy_sweep_enable, Qnil);
}

struct os_each_struct {
    size_t num;
    VALUE of;
};

static int
os_obj_of_i(void *vstart, void *vend, size_t stride, void *data)
{
    struct os_each_struct *oes = (struct os_each_struct *)data;
    RVALUE *p = (RVALUE *)vstart, *pend = (RVALUE *)vend;
    volatile VALUE v;

    for (; p != pend; p++) {
	if (p->as.basic.flags) {
	    switch (BUILTIN_TYPE(p)) {
	      case T_NONE:
	      case T_ICLASS:
	      case T_NODE:
	      case T_ZOMBIE:
		continue;
	      case T_CLASS:
		if (FL_TEST(p, FL_SINGLETON))
		  continue;
	      default:
		if (!p->as.basic.klass) continue;
		v = (VALUE)p;
		if (!oes->of || rb_obj_is_kind_of(v, oes->of)) {
		    rb_yield(v);
		    oes->num++;
		}
	    }
	}
    }

    return 0;
}

static VALUE
os_obj_of(VALUE of)
{
    struct os_each_struct oes;

    oes.num = 0;
    oes.of = of;
    rb_objspace_each_objects(os_obj_of_i, &oes);
    return SIZET2NUM(oes.num);
}

/*
 *  call-seq:
 *     ObjectSpace.each_object([module]) {|obj| ... } -> fixnum
 *     ObjectSpace.each_object([module])              -> an_enumerator
 *
 *  Calls the block once for each living, nonimmediate object in this
 *  Ruby process. If <i>module</i> is specified, calls the block
 *  for only those classes or modules that match (or are a subclass of)
 *  <i>module</i>. Returns the number of objects found. Immediate
 *  objects (<code>Fixnum</code>s, <code>Symbol</code>s
 *  <code>true</code>, <code>false</code>, and <code>nil</code>) are
 *  never returned. In the example below, <code>each_object</code>
 *  returns both the numbers we defined and several constants defined in
 *  the <code>Math</code> module.
 *
 *  If no block is given, an enumerator is returned instead.
 *
 *     a = 102.7
 *     b = 95       # Won't be returned
 *     c = 12345678987654321
 *     count = ObjectSpace.each_object(Numeric) {|x| p x }
 *     puts "Total count: #{count}"
 *
 *  <em>produces:</em>
 *
 *     12345678987654321
 *     102.7
 *     2.71828182845905
 *     3.14159265358979
 *     2.22044604925031e-16
 *     1.7976931348623157e+308
 *     2.2250738585072e-308
 *     Total count: 7
 *
 */

static VALUE
os_each_obj(int argc, VALUE *argv, VALUE os)
{
    VALUE of;

    rb_secure(4);
    if (argc == 0) {
	of = 0;
    }
    else {
	rb_scan_args(argc, argv, "01", &of);
    }
    RETURN_ENUMERATOR(os, 1, &of);
    return os_obj_of(of);
}

/*
 *  call-seq:
 *     ObjectSpace.undefine_finalizer(obj)
 *
 *  Removes all finalizers for <i>obj</i>.
 *
 */

static VALUE
undefine_final(VALUE os, VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    st_data_t data = obj;
    rb_check_frozen(obj);
    st_delete(finalizer_table, &data, 0);
    FL_UNSET(obj, FL_FINALIZE);
    return obj;
}

/*
 *  call-seq:
 *     ObjectSpace.define_finalizer(obj, aProc=proc())
 *
 *  Adds <i>aProc</i> as a finalizer, to be called after <i>obj</i>
 *  was destroyed.
 *
 */

static VALUE
define_final(int argc, VALUE *argv, VALUE os)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE obj, block, table;
    st_data_t data;

    rb_scan_args(argc, argv, "11", &obj, &block);
    rb_check_frozen(obj);
    if (argc == 1) {
	block = rb_block_proc();
    }
    else if (!rb_respond_to(block, rb_intern("call"))) {
	rb_raise(rb_eArgError, "wrong type argument %s (should be callable)",
		 rb_obj_classname(block));
    }
    if (!FL_ABLE(obj)) {
	rb_raise(rb_eArgError, "cannot define finalizer for %s",
		 rb_obj_classname(obj));
    }
    RBASIC(obj)->flags |= FL_FINALIZE;

    block = rb_ary_new3(2, INT2FIX(rb_safe_level()), block);
    OBJ_FREEZE(block);

    if (st_lookup(finalizer_table, obj, &data)) {
	table = (VALUE)data;
	rb_ary_push(table, block);
    }
    else {
	table = rb_ary_new3(1, block);
	RBASIC(table)->klass = 0;
	st_add_direct(finalizer_table, obj, table);
    }
    return block;
}

void
rb_gc_copy_finalizer(VALUE dest, VALUE obj)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE table;
    st_data_t data;

    if (!FL_TEST(obj, FL_FINALIZE)) return;
    if (st_lookup(finalizer_table, obj, &data)) {
	table = (VALUE)data;
	st_insert(finalizer_table, dest, table);
    }
    FL_SET(dest, FL_FINALIZE);
}

static VALUE
run_single_final(VALUE arg)
{
    VALUE *args = (VALUE *)arg;
    rb_eval_cmd(args[0], args[1], (int)args[2]);
    return Qnil;
}

static void
run_finalizer(rb_objspace_t *objspace, VALUE obj, VALUE table)
{
    long i;
    int status;
    VALUE args[3];
    VALUE objid = nonspecial_obj_id(obj);

    if (RARRAY_LEN(table) > 0) {
	args[1] = rb_obj_freeze(rb_ary_new3(1, objid));
    }
    else {
	args[1] = 0;
    }

    args[2] = (VALUE)rb_safe_level();
    for (i=0; i<RARRAY_LEN(table); i++) {
	VALUE final = RARRAY_PTR(table)[i];
	args[0] = RARRAY_PTR(final)[1];
	args[2] = FIX2INT(RARRAY_PTR(final)[0]);
	status = 0;
	rb_protect(run_single_final, (VALUE)args, &status);
	if (status)
	    rb_set_errinfo(Qnil);
    }
}

static void
run_final(rb_objspace_t *objspace, VALUE obj)
{
    RUBY_DATA_FUNC free_func = 0;
    st_data_t key, table;

    objspace->heap.final_num--;

    RBASIC(obj)->klass = 0;

    if (RTYPEDDATA_P(obj)) {
	free_func = RTYPEDDATA_TYPE(obj)->function.dfree;
    }
    else {
	free_func = RDATA(obj)->dfree;
    }
    if (free_func) {
	(*free_func)(DATA_PTR(obj));
    }

    key = (st_data_t)obj;
    if (st_delete(finalizer_table, &key, &table)) {
	run_finalizer(objspace, obj, (VALUE)table);
    }
}

static void
finalize_deferred(rb_objspace_t *objspace)
{
    RVALUE *p;

    while ((p = ATOMIC_PTR_EXCHANGE(deferred_final_list, 0)) != 0) {
	finalize_list(objspace, p);
    }
}

void
rb_gc_finalize_deferred(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    if (ATOMIC_EXCHANGE(finalizing, 1)) return;
    finalize_deferred(objspace);
    ATOMIC_SET(finalizing, 0);
}

struct force_finalize_list {
    VALUE obj;
    VALUE table;
    struct force_finalize_list *next;
};

static int
force_chain_object(st_data_t key, st_data_t val, st_data_t arg)
{
    struct force_finalize_list **prev = (struct force_finalize_list **)arg;
    struct force_finalize_list *curr = ALLOC(struct force_finalize_list);
    curr->obj = key;
    curr->table = val;
    curr->next = *prev;
    *prev = curr;
    return ST_CONTINUE;
}

void
rb_gc_call_finalizer_at_exit(void)
{
    rb_objspace_call_finalizer(&rb_objspace);
}

static const char* obj_type(VALUE type)
{
    switch (type) {
        case T_NIL    : return "NIL";
        case T_OBJECT : return "OBJECT";
        case T_CLASS  : return "CLASS";
        case T_ICLASS : return "ICLASS";
        case T_MODULE : return "MODULE";
        case T_FLOAT  : return "FLOAT";
        case T_COMPLEX: return "COMPLEX";
        case T_RATIONAL: return "RATIONAL";
        case T_STRING : return "STRING";
        case T_REGEXP : return "REGEXP";
        case T_ARRAY  : return "ARRAY";
        case T_FIXNUM : return "FIXNUM";
        case T_HASH   : return "HASH";
        case T_STRUCT : return "STRUCT";
        case T_BIGNUM : return "BIGNUM";
        case T_FILE   : return "FILE";

        case T_TRUE   : return "TRUE";
        case T_FALSE  : return "FALSE";
        case T_DATA   : return "DATA";
        case T_MATCH  : return "MATCH";
        case T_SYMBOL : return "SYMBOL";
        case T_ZOMBIE : return "ZOMBIE";

        case T_UNDEF  : return "UNDEF";
        case T_NODE   : return "NODE";
        default: return "____";
    }
}

static void
rb_objspace_call_finalizer(rb_objspace_t *objspace)
{
    RVALUE *p, *pend;
    RVALUE *final_list = 0;
    size_t i;

    rest_sweep(objspace);

    /* run finalizers */
    finalize_deferred(objspace);
    assert(deferred_final_list == 0);

    if (ATOMIC_EXCHANGE(finalizing, 1)) return;

    /* force to run finalizer */
    while (finalizer_table->num_entries) {
	struct force_finalize_list *list = 0;
	st_foreach(finalizer_table, force_chain_object, (st_data_t)&list);
	while (list) {
	    struct force_finalize_list *curr = list;
            st_data_t obj = (st_data_t)curr->obj;
            run_finalizer(objspace, curr->obj, curr->table);
            st_delete(finalizer_table, &obj, 0);
	    list = curr->next;
	    xfree(curr);
	}
    }

    /* finalizers are part of garbage collection */
    during_gc++;

    /* run data object's finalizers */
    for (i = 0; i < heaps_used; i++) {
	p = objspace->heap.sorted[i]->start; pend = objspace->heap.sorted[i]->end;
	while (p < pend) {
	    if (BUILTIN_TYPE(p) == T_DATA &&
		DATA_PTR(p) && RANY(p)->as.data.dfree &&
		!rb_obj_is_thread((VALUE)p) && !rb_obj_is_mutex((VALUE)p) &&
		!rb_obj_is_fiber((VALUE)p)) {
		p->as.free.flags = 0;
		if (RTYPEDDATA_P(p)) {
		    RDATA(p)->dfree = RANY(p)->as.typeddata.type->function.dfree;
		}
		if (RANY(p)->as.data.dfree == (RUBY_DATA_FUNC)-1) {
		    xfree(DATA_PTR(p));
		}
		else if (RANY(p)->as.data.dfree) {
		    make_deferred(RANY(p));
		    RANY(p)->as.free.next = final_list;
		    final_list = p;
		}
	    }
	    else if (BUILTIN_TYPE(p) == T_FILE) {
		if (RANY(p)->as.file.fptr) {
		    make_io_deferred(RANY(p));
		    RANY(p)->as.free.next = final_list;
		    final_list = p;
		}
	    }
	    p++;
	}
    }
    during_gc = 0;
    if (final_list) {
	finalize_list(objspace, final_list);
    }

    st_free_table(finalizer_table);
    finalizer_table = 0;
    ATOMIC_SET(finalizing, 0);
}

void
rb_gc(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    garbage_collect(objspace);
    if (!finalizing) finalize_deferred(objspace);
    free_unused_heaps(objspace);
}

static inline int
is_id_value(rb_objspace_t *objspace, VALUE ptr)
{
    if (!is_pointer_to_heap(objspace, (void *)ptr)) return FALSE;
    if (BUILTIN_TYPE(ptr) > T_FIXNUM) return FALSE;
    if (BUILTIN_TYPE(ptr) == T_ICLASS) return FALSE;
    return TRUE;
}

static inline int
is_dead_object(rb_objspace_t *objspace, VALUE ptr)
{
    struct heaps_slot *slot = objspace->heap.sweep_slots;
    if (!is_lazy_sweeping(objspace) || MARKED_IN_BITMAP(GET_HEAP_BITMAP(ptr), ptr))
    return FALSE;
    while (slot) {
    if ((VALUE)slot->membase->start <= ptr && ptr < (VALUE)(slot->membase->end))
        return TRUE;
    slot = slot->next;
    }
    return FALSE;
}

static inline int
is_live_object(rb_objspace_t *objspace, VALUE ptr)
{
    if (BUILTIN_TYPE(ptr) == 0) return FALSE;
    if (RBASIC(ptr)->klass == 0) return FALSE;
    if (is_dead_object(objspace, ptr)) return FALSE;
    return TRUE;
}

/*
 *  call-seq:
 *     ObjectSpace._id2ref(object_id) -> an_object
 *
 *  Converts an object id to a reference to the object. May not be
 *  called on an object id passed as a parameter to a finalizer.
 *
 *     s = "I am a string"                    #=> "I am a string"
 *     r = ObjectSpace._id2ref(s.object_id)   #=> "I am a string"
 *     r == s                                 #=> true
 *
 */

static VALUE
id2ref(VALUE obj, VALUE objid)
{
#if SIZEOF_LONG == SIZEOF_VOIDP
#define NUM2PTR(x) NUM2ULONG(x)
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
#define NUM2PTR(x) NUM2ULL(x)
#endif
    rb_objspace_t *objspace = &rb_objspace;
    VALUE ptr;
    void *p0;

    rb_secure(4);
    ptr = NUM2PTR(objid);
    p0 = (void *)ptr;

    if (ptr == Qtrue) return Qtrue;
    if (ptr == Qfalse) return Qfalse;
    if (ptr == Qnil) return Qnil;
    if (FIXNUM_P(ptr)) return (VALUE)ptr;
    ptr = obj_id_to_ref(objid);

    if ((ptr % sizeof(RVALUE)) == (4 << 2)) {
        ID symid = ptr / sizeof(RVALUE);
        if (rb_id2name(symid) == 0)
	    rb_raise(rb_eRangeError, "%p is not symbol id value", p0);
	return ID2SYM(symid);
    }

    if (!is_id_value(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is not id value", p0);
    }
    if (!is_live_object(objspace, ptr)) {
	rb_raise(rb_eRangeError, "%p is recycled object", p0);
    }
    return (VALUE)ptr;
}

/*
 *  Document-method: __id__
 *  Document-method: object_id
 *
 *  call-seq:
 *     obj.__id__       -> fixnum
 *     obj.object_id    -> fixnum
 *
 *  Returns an integer identifier for <i>obj</i>. The same number will
 *  be returned on all calls to <code>id</code> for a given object, and
 *  no two active objects will share an id.
 *  <code>Object#object_id</code> is a different concept from the
 *  <code>:name</code> notation, which returns the symbol id of
 *  <code>name</code>. Replaces the deprecated <code>Object#id</code>.
 */

/*
 *  call-seq:
 *     obj.hash    -> fixnum
 *
 *  Generates a <code>Fixnum</code> hash value for this object. This
 *  function must have the property that <code>a.eql?(b)</code> implies
 *  <code>a.hash == b.hash</code>. The hash value is used by class
 *  <code>Hash</code>. Any hash value that exceeds the capacity of a
 *  <code>Fixnum</code> will be truncated before being used.
 */

VALUE
rb_obj_id(VALUE obj)
{
    /*
     *                32-bit VALUE space
     *          MSB ------------------------ LSB
     *  false   00000000000000000000000000000000
     *  true    00000000000000000000000000000010
     *  nil     00000000000000000000000000000100
     *  undef   00000000000000000000000000000110
     *  symbol  ssssssssssssssssssssssss00001110
     *  object  oooooooooooooooooooooooooooooo00        = 0 (mod sizeof(RVALUE))
     *  fixnum  fffffffffffffffffffffffffffffff1
     *
     *                    object_id space
     *                                       LSB
     *  false   00000000000000000000000000000000
     *  true    00000000000000000000000000000010
     *  nil     00000000000000000000000000000100
     *  undef   00000000000000000000000000000110
     *  symbol   000SSSSSSSSSSSSSSSSSSSSSSSSSSS0        S...S % A = 4 (S...S = s...s * A + 4)
     *  object   oooooooooooooooooooooooooooooo0        o...o % A = 0
     *  fixnum  fffffffffffffffffffffffffffffff1        bignum if required
     *
     *  where A = sizeof(RVALUE)/4
     *
     *  sizeof(RVALUE) is
     *  20 if 32-bit, double is 4-byte aligned
     *  24 if 32-bit, double is 8-byte aligned
     *  40 if 64-bit
     */
    if (SYMBOL_P(obj)) {
        return (SYM2ID(obj) * sizeof(RVALUE) + (4 << 2)) | FIXNUM_FLAG;
    }
    if (SPECIAL_CONST_P(obj)) {
        return LONG2NUM((SIGNED_VALUE)obj);
    }
    return nonspecial_obj_id(obj);
}

static int
set_zero(st_data_t key, st_data_t val, st_data_t arg)
{
    VALUE k = (VALUE)key;
    VALUE hash = (VALUE)arg;
    rb_hash_aset(hash, k, INT2FIX(0));
    return ST_CONTINUE;
}

/*
 *  call-seq:
 *     ObjectSpace.count_objects([result_hash]) -> hash
 *
 *  Counts objects for each type.
 *
 *  It returns a hash as:
 *  {:TOTAL=>10000, :FREE=>3011, :T_OBJECT=>6, :T_CLASS=>404, ...}
 *
 *  If the optional argument, result_hash, is given,
 *  it is overwritten and returned.
 *  This is intended to avoid probe effect.
 *
 *  The contents of the returned hash is implementation defined.
 *  It may be changed in future.
 *
 *  This method is not expected to work except C Ruby.
 *
 */

static VALUE
count_objects(int argc, VALUE *argv, VALUE os)
{
    rb_objspace_t *objspace = &rb_objspace;
    size_t counts[T_MASK+1];
    size_t freed = 0;
    size_t total = 0;
    size_t i;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    for (i = 0; i <= T_MASK; i++) {
        counts[i] = 0;
    }

    for (i = 0; i < heaps_used; i++) {
        RVALUE *p, *pend;

        p = objspace->heap.sorted[i]->start; pend = objspace->heap.sorted[i]->end;
        for (;p < pend; p++) {
            if (p->as.basic.flags) {
                counts[BUILTIN_TYPE(p)]++;
            }
            else {
                freed++;
            }
        }
        total += objspace->heap.sorted[i]->limit;
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }
    else if (!RHASH_EMPTY_P(hash)) {
        st_foreach(RHASH_TBL(hash), set_zero, hash);
    }
    rb_hash_aset(hash, ID2SYM(rb_intern("TOTAL")), SIZET2NUM(total));
    rb_hash_aset(hash, ID2SYM(rb_intern("FREE")), SIZET2NUM(freed));

    for (i = 0; i <= T_MASK; i++) {
        VALUE type;
        switch (i) {
#define COUNT_TYPE(t) case (t): type = ID2SYM(rb_intern(#t)); break;
	    COUNT_TYPE(T_NONE);
	    COUNT_TYPE(T_OBJECT);
	    COUNT_TYPE(T_CLASS);
	    COUNT_TYPE(T_MODULE);
	    COUNT_TYPE(T_FLOAT);
	    COUNT_TYPE(T_STRING);
	    COUNT_TYPE(T_REGEXP);
	    COUNT_TYPE(T_ARRAY);
	    COUNT_TYPE(T_HASH);
	    COUNT_TYPE(T_STRUCT);
	    COUNT_TYPE(T_BIGNUM);
	    COUNT_TYPE(T_FILE);
	    COUNT_TYPE(T_DATA);
	    COUNT_TYPE(T_MATCH);
	    COUNT_TYPE(T_COMPLEX);
	    COUNT_TYPE(T_RATIONAL);
	    COUNT_TYPE(T_NIL);
	    COUNT_TYPE(T_TRUE);
	    COUNT_TYPE(T_FALSE);
	    COUNT_TYPE(T_SYMBOL);
	    COUNT_TYPE(T_FIXNUM);
	    COUNT_TYPE(T_UNDEF);
	    COUNT_TYPE(T_NODE);
	    COUNT_TYPE(T_ICLASS);
	    COUNT_TYPE(T_ZOMBIE);
#undef COUNT_TYPE
          default:              type = INT2NUM(i); break;
        }
        if (counts[i])
            rb_hash_aset(hash, type, SIZET2NUM(counts[i]));
    }

    return hash;
}

/* call-seq:
 *  ObjectSpace.live_objects => number
 *
 * Returns the count of objects currently allocated in the system. This goes
 * down after the garbage collector runs.
 */
static
VALUE os_live_objects(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    return ULONG2NUM(live_objects);
}

unsigned long rb_os_live_objects()
{
    rb_objspace_t *objspace = &rb_objspace;
    return live_objects;
}

/* call-seq:
 *  ObjectSpace.allocated_objects => number
 *
 * Returns the count of objects allocated since the Ruby interpreter has
 * started.  This number can only increase. To know how many objects are
 * currently allocated, use ObjectSpace::live_objects
 */
static
VALUE os_allocated_objects(VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
#if defined(HAVE_LONG_LONG)
    return ULL2NUM(allocated_objects);
#else
    return ULONG2NUM(allocated_objects);
#endif
}

RUBY_FUNC_EXPORTED
unsigned LONG_LONG rb_os_allocated_objects()
{
    rb_objspace_t *objspace = &rb_objspace;
    return allocated_objects;
}

/*
 *  call-seq:
 *     GC.count -> Integer
 *
 *  The number of times GC occurred.
 *
 *  It returns the number of times GC occurred since the process started.
 *
 */

static VALUE
gc_count(VALUE self)
{
    return UINT2NUM((&rb_objspace)->count);
}

/*
 *  call-seq:
 *     GC.stat -> Hash
 *
 *  Returns a Hash containing information about the GC.
 *
 *  The hash includes information about internal statistics about GC such as:
 *
 *    {
 *      :count          => 18,
 *      :heap_used      => 77,
 *      :heap_length    => 77,
 *      :heap_increment => 0,
 *      :heap_live_num  => 23287,
 *      :heap_free_num  => 8115,
 *      :heap_final_num => 0,
 *    }
 *
 *  The contents of the hash are implementation defined and may be changed in
 *  the future.
 *
 *  This method is only expected to work on C Ruby.
 *
 */

static VALUE
gc_stat(int argc, VALUE *argv, VALUE self)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE hash;

    if (rb_scan_args(argc, argv, "01", &hash) == 1) {
        if (!RB_TYPE_P(hash, T_HASH))
            rb_raise(rb_eTypeError, "non-hash given");
    }

    if (hash == Qnil) {
        hash = rb_hash_new();
    }

    rest_sweep(objspace);

    rb_hash_aset(hash, ID2SYM(rb_intern("count")), SIZET2NUM(objspace->count));

    /* implementation dependent counters */
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_used")), SIZET2NUM(objspace->heap.used));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_length")), SIZET2NUM(objspace->heap.length));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_increment")), SIZET2NUM(objspace->heap.increment));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_live_num")), SIZET2NUM(objspace->heap.live_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_free_num")), SIZET2NUM(objspace->heap.free_num));
    rb_hash_aset(hash, ID2SYM(rb_intern("heap_final_num")), SIZET2NUM(objspace->heap.final_num));
    return hash;
}


#if CALC_EXACT_MALLOC_SIZE
/*
 *  call-seq:
 *     GC.malloc_allocated_size -> Integer
 *
 *  The allocated size by malloc().
 *
 *  It returns the allocated size by malloc().
 */

static VALUE
gc_malloc_allocated_size(VALUE self)
{
    return UINT2NUM((&rb_objspace)->malloc_params.allocated_size);
}

/*
 *  call-seq:
 *     GC.malloc_allocations -> Integer
 *
 *  The number of allocated memory object by malloc().
 *
 *  It returns the number of allocated memory object by malloc().
 */

static VALUE
gc_malloc_allocations(VALUE self)
{
    return UINT2NUM((&rb_objspace)->malloc_params.allocations);
}
#else

static VALUE
gc_malloc_growth(VALUE self)
{
    return UINT2NUM(rb_objspace.malloc_params.increase);
}

static VALUE
gc_malloc_limit(VALUE self)
{
    return UINT2NUM(rb_objspace.malloc_params.limit);
}
#endif

static VALUE
gc_profile_record_get(void)
{
    VALUE prof;
    VALUE gc_profile = rb_ary_new();
    size_t i;
    rb_objspace_t *objspace = (&rb_objspace);

    if (!objspace->profile.run) {
	return Qnil;
    }

    for (i =0; i < objspace->profile.count; i++) {
	prof = rb_hash_new();
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_TIME")), DBL2NUM(objspace->profile.record[i].gc_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_INVOKE_TIME")), DBL2NUM(objspace->profile.record[i].gc_invoke_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_SIZE")), SIZET2NUM(objspace->profile.record[i].heap_use_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_SIZE")), SIZET2NUM(objspace->profile.record[i].heap_total_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_OBJECTS")), SIZET2NUM(objspace->profile.record[i].heap_total_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_IS_MARKED")), objspace->profile.record[i].is_marked);
#if GC_PROFILE_MORE_DETAIL
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_MARK_TIME")), DBL2NUM(objspace->profile.record[i].gc_mark_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_SWEEP_TIME")), DBL2NUM(objspace->profile.record[i].gc_sweep_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_INCREASE")), SIZET2NUM(objspace->profile.record[i].allocate_increase));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_LIMIT")), SIZET2NUM(objspace->profile.record[i].allocate_limit));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_SLOTS")), SIZET2NUM(objspace->profile.record[i].heap_use_slots));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_LIVE_OBJECTS")), SIZET2NUM(objspace->profile.record[i].heap_live_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_FREE_OBJECTS")), SIZET2NUM(objspace->profile.record[i].heap_free_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("HAVE_FINALIZE")), objspace->profile.record[i].have_finalize);
#endif
	rb_ary_push(gc_profile, prof);
    }

    return gc_profile;
}

/*
 *  call-seq:
 *     GC::Profiler.result -> String
 *
 *  Returns a profile data report such as:
 *
 *    GC 1 invokes.
 *    Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC time(ms)
 *        1               0.012               159240               212940                10647         0.00000000000001530000
 */

static VALUE
gc_profile_result(void)
{
    rb_objspace_t *objspace = &rb_objspace;
    VALUE record;
    VALUE result;
    int i, index;

    record = gc_profile_record_get();
    if (objspace->profile.run && objspace->profile.count) {
	result = rb_sprintf("GC %d invokes.\n", NUM2INT(gc_count(0)));
        index = 1;
	rb_str_cat2(result, "Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC Time(ms)\n");
	for (i = 0; i < (int)RARRAY_LEN(record); i++) {
	    VALUE r = RARRAY_PTR(record)[i];
#if !GC_PROFILE_MORE_DETAIL
            if (rb_hash_aref(r, ID2SYM(rb_intern("GC_IS_MARKED")))) {
#endif
	    rb_str_catf(result, "%5d %19.3f %20"PRIuSIZE" %20"PRIuSIZE" %20"PRIuSIZE" %30.20f\n",
			index++, NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_INVOKE_TIME")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_USE_SIZE")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_TOTAL_SIZE")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_TOTAL_OBJECTS")))),
			NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_TIME"))))*1000);
#if !GC_PROFILE_MORE_DETAIL
            }
#endif
	}
#if GC_PROFILE_MORE_DETAIL
	rb_str_cat2(result, "\n\n");
	rb_str_cat2(result, "More detail.\n");
	rb_str_cat2(result, "Index Allocate Increase    Allocate Limit  Use Slot  Have Finalize             Mark Time(ms)            Sweep Time(ms)\n");
        index = 1;
	for (i = 0; i < (int)RARRAY_LEN(record); i++) {
	    VALUE r = RARRAY_PTR(record)[i];
	    rb_str_catf(result, "%5d %17"PRIuSIZE" %17"PRIuSIZE" %9"PRIuSIZE" %14s %25.20f %25.20f\n",
			index++, (size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("ALLOCATE_INCREASE")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("ALLOCATE_LIMIT")))),
			(size_t)NUM2SIZET(rb_hash_aref(r, ID2SYM(rb_intern("HEAP_USE_SLOTS")))),
			rb_hash_aref(r, ID2SYM(rb_intern("HAVE_FINALIZE")))? "true" : "false",
			NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_MARK_TIME"))))*1000,
			NUM2DBL(rb_hash_aref(r, ID2SYM(rb_intern("GC_SWEEP_TIME"))))*1000);
	}
#endif
    }
    else {
	result = rb_str_new2("");
    }
    return result;
}


/*
 *  call-seq:
 *     GC::Profiler.report
 *     GC::Profiler.report io
 *
 *  Writes the GC::Profiler#result to <tt>$stdout</tt> or the given IO object.
 *
 */

static VALUE
gc_profile_report(int argc, VALUE *argv, VALUE self)
{
    VALUE out;

    if (argc == 0) {
	out = rb_stdout;
    }
    else {
	rb_scan_args(argc, argv, "01", &out);
    }
    rb_io_write(out, gc_profile_result());

    return Qnil;
}

/*
 *  call-seq:
 *     GC::Profiler.total_time -> float
 *
 *  The total time used for garbage collection in milliseconds
 */

static VALUE
gc_profile_total_time(VALUE self)
{
    double time = 0;
    rb_objspace_t *objspace = &rb_objspace;
    size_t i;

    if (objspace->profile.run && objspace->profile.count) {
	for (i = 0; i < objspace->profile.count; i++) {
	    time += objspace->profile.record[i].gc_time;
	}
    }
    return DBL2NUM(time);
}

/*  Document-class: GC::Profiler
 *
 *  The GC profiler provides access to information on GC runs including time,
 *  length and object space size.
 *
 *  Example:
 *
 *    GC::Profiler.enable
 *
 *    require 'rdoc/rdoc'
 *
 *    puts GC::Profiler.result
 *
 *    GC::Profiler.disable
 *
 *  See also GC.count, GC.malloc_allocated_size and GC.malloc_allocations
 */

/*
 *  The <code>GC</code> module provides an interface to Ruby's mark and
 *  sweep garbage collection mechanism. Some of the underlying methods
 *  are also available via the ObjectSpace module.
 *
 *  You may obtain information about the operation of the GC through
 *  GC::Profiler.
 */

void
Init_GC(void)
{
    VALUE rb_mObSpace;
    VALUE rb_mProfiler;

    rb_mGC = rb_define_module("GC");
    rb_define_singleton_method(rb_mGC, "start", rb_gc_start, 0);
    rb_define_singleton_method(rb_mGC, "enable", rb_gc_enable, 0);
    rb_define_singleton_method(rb_mProfiler, "raw_data", gc_profile_record_get, 0);
    rb_define_singleton_method(rb_mGC, "disable", rb_gc_disable, 0);
    rb_define_singleton_method(rb_mGC, "stress", gc_stress_get, 0);
    rb_define_singleton_method(rb_mGC, "stress=", gc_stress_set, 1);
    rb_define_singleton_method(rb_mGC, "count", gc_count, 0);
    rb_define_singleton_method(rb_mGC, "stat", gc_stat, -1);
    rb_define_method(rb_mGC, "garbage_collect", rb_gc_start, 0);

    rb_define_singleton_method(rb_mGC, "enable_stats", rb_gc_enable_stats, 0);
    rb_define_singleton_method(rb_mGC, "disable_stats", rb_gc_disable_stats, 0);
    rb_define_singleton_method(rb_mGC, "stats_enabled?", rb_gc_stats_enabled, 0);
    rb_define_singleton_method(rb_mGC, "clear_stats", rb_gc_clear_stats, 0);
    rb_define_singleton_method(rb_mGC, "allocated_size", rb_gc_allocated_size, 0);
    rb_define_singleton_method(rb_mGC, "num_allocations", rb_gc_num_allocations, 0);
    rb_define_singleton_method(rb_mGC, "heap_slots", rb_gc_heap_slots, 0);
    rb_define_singleton_method(rb_mGC, "heap_slots_live_after_last_gc", rb_gc_heap_slots_live_after_last_gc, 0);
    rb_define_singleton_method(rb_mGC, "free_slots", rb_gc_free_count, 0);

    rb_define_const(rb_mGC, "HEAP_SLOT_SIZE", INT2FIX(sizeof(RVALUE)));
    rb_define_const(rb_mGC, "HEAP_OBJ_LIMIT", INT2FIX(HEAP_OBJ_LIMIT));
    rb_define_const(rb_mGC, "HEAP_SIZE", INT2FIX(HEAP_SIZE));

    rb_define_singleton_method(rb_mGC, "log", rb_gc_log, 1);
    rb_define_singleton_method(rb_mGC, "log_file", rb_gc_log_file, -1);
    rb_define_singleton_method(rb_mGC, "enable_trace", rb_gc_enable_trace, 0);
    rb_define_singleton_method(rb_mGC, "disable_trace", rb_gc_disable_trace, 0);
    rb_define_singleton_method(rb_mGC, "trace_enabled?", rb_gc_trace_enabled, 0);

    rb_define_singleton_method(rb_mGC, "collections", rb_gc_collections, 0);
    rb_define_singleton_method(rb_mGC, "time", rb_gc_time, 0);
    rb_define_singleton_method(rb_mGC, "dump", rb_gc_dump, 0);
#ifdef GC_DEBUG
    rb_define_singleton_method(rb_mGC, "dump_file_and_line_info", rb_gc_dump_file_and_line_info, -1);
#endif

    rb_mProfiler = rb_define_module_under(rb_mGC, "Profiler");
    rb_define_singleton_method(rb_mProfiler, "enabled?", gc_profile_enable_get, 0);
    rb_define_singleton_method(rb_mProfiler, "enable", gc_profile_enable, 0);
    rb_define_singleton_method(rb_mProfiler, "disable", gc_profile_disable, 0);
    rb_define_singleton_method(rb_mProfiler, "clear", gc_profile_clear, 0);
    rb_define_singleton_method(rb_mProfiler, "result", gc_profile_result, 0);
    rb_define_singleton_method(rb_mProfiler, "report", gc_profile_report, -1);
    rb_define_singleton_method(rb_mProfiler, "total_time", gc_profile_total_time, 0);

    rb_mObSpace = rb_define_module("ObjectSpace");
    rb_define_module_function(rb_mObSpace, "each_object", os_each_obj, -1);
    rb_define_module_function(rb_mObSpace, "garbage_collect", rb_gc_start, 0);

    rb_define_module_function(rb_mObSpace, "live_objects", os_live_objects, 0);
    rb_define_module_function(rb_mObSpace, "allocated_objects", os_allocated_objects, 0);

    rb_define_module_function(rb_mObSpace, "define_finalizer", define_final, -1);
    rb_define_module_function(rb_mObSpace, "undefine_finalizer", undefine_final, 1);

    rb_define_module_function(rb_mObSpace, "_id2ref", id2ref, 1);

    nomem_error = rb_exc_new3(rb_eNoMemError,
			      rb_obj_freeze(rb_str_new2("failed to allocate memory")));
    OBJ_TAINT(nomem_error);
    OBJ_FREEZE(nomem_error);

    rb_define_method(rb_cBasicObject, "__id__", rb_obj_id, 0);
    rb_define_method(rb_mKernel, "object_id", rb_obj_id, 0);

    rb_define_module_function(rb_mObSpace, "count_objects", count_objects, -1);

#if CALC_EXACT_MALLOC_SIZE
    rb_define_singleton_method(rb_mGC, "malloc_allocated_size", gc_malloc_allocated_size, 0);
    rb_define_singleton_method(rb_mGC, "malloc_allocations", gc_malloc_allocations, 0);
#else
    rb_define_singleton_method(rb_mGC, "growth", gc_malloc_growth, 0);
    rb_define_singleton_method(rb_mGC, "limit", gc_malloc_limit, 0);
#endif
}
