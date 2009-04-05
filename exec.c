/*
 *  virtual page mapping and translated block handling
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "config.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/mman.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

#include "cpu.h"
#include "exec-all.h"
#include "qemu-common.h"
#include "tcg.h"
#include "hw/hw.h"
#if defined(CONFIG_USER_ONLY)
#include <qemu.h>
#endif

//#define DEBUG_TB_INVALIDATE
//#define DEBUG_FLUSH
//#define DEBUG_TLB
//#define DEBUG_UNASSIGNED

/* make various TB consistency checks */
//#define DEBUG_TB_CHECK
//#define DEBUG_TLB_CHECK

//#define DEBUG_IOPORT
//#define DEBUG_SUBPAGE

#if !defined(CONFIG_USER_ONLY)
/* TB consistency checks only implemented for usermode emulation.  */
#undef DEBUG_TB_CHECK
#endif

#define SMC_BITMAP_USE_THRESHOLD 10

#define MMAP_AREA_START        0x00000000
#define MMAP_AREA_END          0xa8000000

#if defined(TARGET_SPARC64)
#define TARGET_PHYS_ADDR_SPACE_BITS 41
#elif defined(TARGET_SPARC)
#define TARGET_PHYS_ADDR_SPACE_BITS 36
#elif defined(TARGET_ALPHA)
#define TARGET_PHYS_ADDR_SPACE_BITS 42
#define TARGET_VIRT_ADDR_SPACE_BITS 42
#elif defined(TARGET_PPC64)
#define TARGET_PHYS_ADDR_SPACE_BITS 42
#elif defined(TARGET_X86_64) && !defined(USE_KQEMU)
#define TARGET_PHYS_ADDR_SPACE_BITS 42
#elif defined(TARGET_I386) && !defined(USE_KQEMU)
#define TARGET_PHYS_ADDR_SPACE_BITS 36
#else
/* Note: for compatibility with kqemu, we use 32 bits for x86_64 */
#define TARGET_PHYS_ADDR_SPACE_BITS 32
#endif

TranslationBlock *tbs;
int code_gen_max_blocks;
TranslationBlock *tb_phys_hash[CODE_GEN_PHYS_HASH_SIZE];
int nb_tbs;
/* any access to the tbs or the page table must use this lock */
spinlock_t tb_lock = SPIN_LOCK_UNLOCKED;

#if defined(__arm__) || defined(__sparc_v9__)
/* The prologue must be reachable with a direct jump. ARM and Sparc64
 have limited branch ranges (possibly also PPC) so place it in a
 section close to code segment. */
#define code_gen_section                                \
    __attribute__((__section__(".gen_code")))           \
    __attribute__((aligned (32)))
#else
#define code_gen_section                                \
    __attribute__((aligned (32)))
#endif

uint8_t code_gen_prologue[1024] code_gen_section;
uint8_t *code_gen_buffer;
unsigned long code_gen_buffer_size;
/* threshold to flush the translated code buffer */
unsigned long code_gen_buffer_max_size; 
uint8_t *code_gen_ptr;

#if !defined(CONFIG_USER_ONLY)
ram_addr_t phys_ram_size;
int phys_ram_fd;
uint8_t *phys_ram_base;
uint8_t *phys_ram_dirty;
static ram_addr_t phys_ram_alloc_offset = 0;
#endif

CPUState *first_cpu;
/* current CPU in the current thread. It is only valid inside
   cpu_exec() */
CPUState *cpu_single_env;
/* 0 = Do not count executed instructions.
   1 = Precise instruction counting.
   2 = Adaptive rate instruction counting.  */
int use_icount = 0;
/* Current instruction counter.  While executing translated code this may
   include some instructions that have not yet been executed.  */
int64_t qemu_icount;

typedef struct PageDesc {
    /* list of TBs intersecting this ram page */
    TranslationBlock *first_tb;
    /* in order to optimize self modifying code, we count the number
       of lookups we do to a given page to use a bitmap */
    unsigned int code_write_count;
    uint8_t *code_bitmap;
#if defined(CONFIG_USER_ONLY)
    unsigned long flags;
#endif
} PageDesc;

typedef struct PhysPageDesc {
    /* offset in host memory of the page + io_index in the low bits */
    ram_addr_t phys_offset;
} PhysPageDesc;

#define L2_BITS 10
#if defined(CONFIG_USER_ONLY) && defined(TARGET_VIRT_ADDR_SPACE_BITS)
/* XXX: this is a temporary hack for alpha target.
 *      In the future, this is to be replaced by a multi-level table
 *      to actually be able to handle the complete 64 bits address space.
 */
#define L1_BITS (TARGET_VIRT_ADDR_SPACE_BITS - L2_BITS - TARGET_PAGE_BITS)
#else
#define L1_BITS (32 - L2_BITS - TARGET_PAGE_BITS)
#endif

#define L1_SIZE (1 << L1_BITS)
#define L2_SIZE (1 << L2_BITS)

unsigned long qemu_real_host_page_size;
unsigned long qemu_host_page_bits;
unsigned long qemu_host_page_size;
unsigned long qemu_host_page_mask;

/* XXX: for system emulation, it could just be an array */
static PageDesc *l1_map[L1_SIZE];
PhysPageDesc **l1_phys_map;

#if !defined(CONFIG_USER_ONLY)
static void io_mem_init(void);

/* io memory support */
CPUWriteMemoryFunc *io_mem_write[IO_MEM_NB_ENTRIES][4];
CPUReadMemoryFunc *io_mem_read[IO_MEM_NB_ENTRIES][4];
void *io_mem_opaque[IO_MEM_NB_ENTRIES];
static int io_mem_nb;
static int io_mem_watch;
#endif

/* log support */
const char *logfilename = "/tmp/qemu.log";
FILE *logfile;
int loglevel;
static int log_append = 0;

/* statistics */
static int tlb_flush_count;
static int tb_flush_count;
static int tb_phys_invalidate_count;

#define SUBPAGE_IDX(addr) ((addr) & ~TARGET_PAGE_MASK)
typedef struct subpage_t {
    target_phys_addr_t base;
    CPUReadMemoryFunc **mem_read[TARGET_PAGE_SIZE][4];
    CPUWriteMemoryFunc **mem_write[TARGET_PAGE_SIZE][4];
    void *opaque[TARGET_PAGE_SIZE][2][4];
} subpage_t;

#ifdef _WIN32
static void map_exec(void *addr, long size)
{
    DWORD old_protect;
    VirtualProtect(addr, size,
                   PAGE_EXECUTE_READWRITE, &old_protect);
    
}
#else
static void map_exec(void *addr, long size)
{
    unsigned long start, end, page_size;
    
    page_size = getpagesize();
    start = (unsigned long)addr;
    start &= ~(page_size - 1);
    
    end = (unsigned long)addr + size;
    end += page_size - 1;
    end &= ~(page_size - 1);
    
    mprotect((void *)start, end - start,
             PROT_READ | PROT_WRITE | PROT_EXEC);
}
#endif

static void page_init(void)
{
    /* NOTE: we can always suppose that qemu_host_page_size >=
       TARGET_PAGE_SIZE */
#ifdef _WIN32
    {
        SYSTEM_INFO system_info;
        DWORD old_protect;

        GetSystemInfo(&system_info);
        qemu_real_host_page_size = system_info.dwPageSize;
    }
#else
    qemu_real_host_page_size = getpagesize();
#endif
    if (qemu_host_page_size == 0)
        qemu_host_page_size = qemu_real_host_page_size;
    if (qemu_host_page_size < TARGET_PAGE_SIZE)
        qemu_host_page_size = TARGET_PAGE_SIZE;
    qemu_host_page_bits = 0;
    while ((1 << qemu_host_page_bits) < qemu_host_page_size)
        qemu_host_page_bits++;
    qemu_host_page_mask = ~(qemu_host_page_size - 1);
    l1_phys_map = qemu_vmalloc(L1_SIZE * sizeof(void *));
    memset(l1_phys_map, 0, L1_SIZE * sizeof(void *));

#if !defined(_WIN32) && defined(CONFIG_USER_ONLY)
    {
        long long startaddr, endaddr;
        FILE *f;
        int n;

        mmap_lock();
        last_brk = (unsigned long)sbrk(0);
        f = fopen("/proc/self/maps", "r");
        if (f) {
            do {
                n = fscanf (f, "%llx-%llx %*[^\n]\n", &startaddr, &endaddr);
                if (n == 2) {
                    startaddr = MIN(startaddr,
                                    (1ULL << TARGET_PHYS_ADDR_SPACE_BITS) - 1);
                    endaddr = MIN(endaddr,
                                    (1ULL << TARGET_PHYS_ADDR_SPACE_BITS) - 1);
                    page_set_flags(startaddr & TARGET_PAGE_MASK,
                                   TARGET_PAGE_ALIGN(endaddr),
                                   PAGE_RESERVED); 
                }
            } while (!feof(f));
            fclose(f);
        }
        mmap_unlock();
    }
#endif
}

static inline PageDesc **page_l1_map(target_ulong index)
{
#if TARGET_LONG_BITS > 32
    /* Host memory outside guest VM.  For 32-bit targets we have already
       excluded high addresses.  */
    if (index > ((target_ulong)L2_SIZE * L1_SIZE))
        return NULL;
#endif
    return &l1_map[index >> L2_BITS];
}

static inline PageDesc *page_find_alloc(target_ulong index)
{
    PageDesc **lp, *p;
    lp = page_l1_map(index);
    if (!lp)
        return NULL;

    p = *lp;
    if (!p) {
        /* allocate if not found */
#if defined(CONFIG_USER_ONLY)
        unsigned long addr;
        size_t len = sizeof(PageDesc) * L2_SIZE;
        /* Don't use qemu_malloc because it may recurse.  */
        p = mmap(0, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        *lp = p;
        addr = h2g(p);
        if (addr == (target_ulong)addr) {
            page_set_flags(addr & TARGET_PAGE_MASK,
                           TARGET_PAGE_ALIGN(addr + len),
                           PAGE_RESERVED); 
        }
#else
        p = qemu_mallocz(sizeof(PageDesc) * L2_SIZE);
        *lp = p;
#endif
    }
    return p + (index & (L2_SIZE - 1));
}

static inline PageDesc *page_find(target_ulong index)
{
    PageDesc **lp, *p;
    lp = page_l1_map(index);
    if (!lp)
        return NULL;

    p = *lp;
    if (!p)
        return 0;
    return p + (index & (L2_SIZE - 1));
}

static PhysPageDesc *phys_page_find_alloc(target_phys_addr_t index, int alloc)
{
    void **lp, **p;
    PhysPageDesc *pd;

    p = (void **)l1_phys_map;
#if TARGET_PHYS_ADDR_SPACE_BITS > 32

#if TARGET_PHYS_ADDR_SPACE_BITS > (32 + L1_BITS)
#error unsupported TARGET_PHYS_ADDR_SPACE_BITS
#endif
    lp = p + ((index >> (L1_BITS + L2_BITS)) & (L1_SIZE - 1));
    p = *lp;
    if (!p) {
        /* allocate if not found */
        if (!alloc)
            return NULL;
        p = qemu_vmalloc(sizeof(void *) * L1_SIZE);
        memset(p, 0, sizeof(void *) * L1_SIZE);
        *lp = p;
    }
#endif
    lp = p + ((index >> L2_BITS) & (L1_SIZE - 1));
    pd = *lp;
    if (!pd) {
        int i;
        /* allocate if not found */
        if (!alloc)
            return NULL;
        pd = qemu_vmalloc(sizeof(PhysPageDesc) * L2_SIZE);
        *lp = pd;
        for (i = 0; i < L2_SIZE; i++)
          pd[i].phys_offset = IO_MEM_UNASSIGNED;
    }
    return ((PhysPageDesc *)pd) + (index & (L2_SIZE - 1));
}

static inline PhysPageDesc *phys_page_find(target_phys_addr_t index)
{
    return phys_page_find_alloc(index, 0);
}

#if !defined(CONFIG_USER_ONLY)
static void tlb_protect_code(ram_addr_t ram_addr);
static void tlb_unprotect_code_phys(CPUState *env, ram_addr_t ram_addr,
                                    target_ulong vaddr);
#define mmap_lock() do { } while(0)
#define mmap_unlock() do { } while(0)
#endif

#define DEFAULT_CODE_GEN_BUFFER_SIZE (32 * 1024 * 1024)

#if defined(CONFIG_USER_ONLY)
/* Currently it is not recommanded to allocate big chunks of data in
   user mode. It will change when a dedicated libc will be used */
#define USE_STATIC_CODE_GEN_BUFFER
#endif

#ifdef USE_STATIC_CODE_GEN_BUFFER
static uint8_t static_code_gen_buffer[DEFAULT_CODE_GEN_BUFFER_SIZE];
#endif

static void code_gen_alloc(unsigned long tb_size)
{
#ifdef USE_STATIC_CODE_GEN_BUFFER
    code_gen_buffer = static_code_gen_buffer;
    code_gen_buffer_size = DEFAULT_CODE_GEN_BUFFER_SIZE;
    map_exec(code_gen_buffer, code_gen_buffer_size);
#else
    code_gen_buffer_size = tb_size;
    if (code_gen_buffer_size == 0) {
#if defined(CONFIG_USER_ONLY)
        /* in user mode, phys_ram_size is not meaningful */
        code_gen_buffer_size = DEFAULT_CODE_GEN_BUFFER_SIZE;
#else
        /* XXX: needs ajustments */
        code_gen_buffer_size = (int)(phys_ram_size / 4);
#endif
    }
    if (code_gen_buffer_size < MIN_CODE_GEN_BUFFER_SIZE)
        code_gen_buffer_size = MIN_CODE_GEN_BUFFER_SIZE;
    /* The code gen buffer location may have constraints depending on
       the host cpu and OS */
#if defined(__linux__) 
    {
        int flags;
        void *start = NULL;

        flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
        flags |= MAP_32BIT;
        /* Cannot map more than that */
        if (code_gen_buffer_size > (800 * 1024 * 1024))
            code_gen_buffer_size = (800 * 1024 * 1024);
#elif defined(__sparc_v9__)
        // Map the buffer below 2G, so we can use direct calls and branches
        flags |= MAP_FIXED;
        start = (void *) 0x60000000UL;
        if (code_gen_buffer_size > (512 * 1024 * 1024))
            code_gen_buffer_size = (512 * 1024 * 1024);
#endif
        code_gen_buffer = mmap(start, code_gen_buffer_size,
                               PROT_WRITE | PROT_READ | PROT_EXEC,
                               flags, -1, 0);
        if (code_gen_buffer == MAP_FAILED) {
            fprintf(stderr, "Could not allocate dynamic translator buffer\n");
            exit(1);
        }
    }
#else
    code_gen_buffer = qemu_malloc(code_gen_buffer_size);
    if (!code_gen_buffer) {
        fprintf(stderr, "Could not allocate dynamic translator buffer\n");
        exit(1);
    }
    map_exec(code_gen_buffer, code_gen_buffer_size);
#endif
#endif /* !USE_STATIC_CODE_GEN_BUFFER */
    map_exec(code_gen_prologue, sizeof(code_gen_prologue));
    code_gen_buffer_max_size = code_gen_buffer_size - 
        code_gen_max_block_size();
    code_gen_max_blocks = code_gen_buffer_size / CODE_GEN_AVG_BLOCK_SIZE;
    tbs = qemu_malloc(code_gen_max_blocks * sizeof(TranslationBlock));
}

/* Must be called before using the QEMU cpus. 'tb_size' is the size
   (in bytes) allocated to the translation buffer. Zero means default
   size. */
void cpu_exec_init_all(unsigned long tb_size)
{
    cpu_gen_init();
    code_gen_alloc(tb_size);
    code_gen_ptr = code_gen_buffer;
    page_init();
#if !defined(CONFIG_USER_ONLY)
    io_mem_init();
#endif
}

#if defined(CPU_SAVE_VERSION) && !defined(CONFIG_USER_ONLY)

#define CPU_COMMON_SAVE_VERSION 1

static void cpu_common_save(QEMUFile *f, void *opaque)
{
    CPUState *env = opaque;

    qemu_put_be32s(f, &env->halted);
    qemu_put_be32s(f, &env->interrupt_request);
}

static int cpu_common_load(QEMUFile *f, void *opaque, int version_id)
{
    CPUState *env = opaque;

    if (version_id != CPU_COMMON_SAVE_VERSION)
        return -EINVAL;

    qemu_get_be32s(f, &env->halted);
    qemu_get_be32s(f, &env->interrupt_request);
    tlb_flush(env, 1);

    return 0;
}
#endif

void cpu_exec_init(CPUState *env)
{
    CPUState **penv;
    int cpu_index;

    env->next_cpu = NULL;
    penv = &first_cpu;
    cpu_index = 0;
    while (*penv != NULL) {
        penv = (CPUState **)&(*penv)->next_cpu;
        cpu_index++;
    }
    env->cpu_index = cpu_index;
    env->nb_watchpoints = 0;
    *penv = env;
#if defined(CPU_SAVE_VERSION) && !defined(CONFIG_USER_ONLY)
    register_savevm("cpu_common", cpu_index, CPU_COMMON_SAVE_VERSION,
                    cpu_common_save, cpu_common_load, env);
    register_savevm("cpu", cpu_index, CPU_SAVE_VERSION,
                    cpu_save, cpu_load, env);
#endif
}

static inline void invalidate_page_bitmap(PageDesc *p)
{
    if (p->code_bitmap) {
        qemu_free(p->code_bitmap);
        p->code_bitmap = NULL;
    }
    p->code_write_count = 0;
}

/* set to NULL all the 'first_tb' fields in all PageDescs */
static void page_flush_tb(void)
{
    int i, j;
    PageDesc *p;

    for(i = 0; i < L1_SIZE; i++) {
        p = l1_map[i];
        if (p) {
            for(j = 0; j < L2_SIZE; j++) {
                p->first_tb = NULL;
                invalidate_page_bitmap(p);
                p++;
            }
        }
    }
}

/* flush all the translation blocks */
/* XXX: tb_flush is currently not thread safe */
void tb_flush(CPUState *env1)
{
    CPUState *env;
#if defined(DEBUG_FLUSH)
    printf("qemu: flush code_size=%ld nb_tbs=%d avg_tb_size=%ld\n",
           (unsigned long)(code_gen_ptr - code_gen_buffer),
           nb_tbs, nb_tbs > 0 ?
           ((unsigned long)(code_gen_ptr - code_gen_buffer)) / nb_tbs : 0);
#endif
    if ((unsigned long)(code_gen_ptr - code_gen_buffer) > code_gen_buffer_size)
        cpu_abort(env1, "Internal error: code buffer overflow\n");

    nb_tbs = 0;

    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        memset (env->tb_jmp_cache, 0, TB_JMP_CACHE_SIZE * sizeof (void *));
    }

    memset (tb_phys_hash, 0, CODE_GEN_PHYS_HASH_SIZE * sizeof (void *));
    page_flush_tb();

    code_gen_ptr = code_gen_buffer;
    /* XXX: flush processor icache at this point if cache flush is
       expensive */
    tb_flush_count++;
}

#ifdef DEBUG_TB_CHECK

static void tb_invalidate_check(target_ulong address)
{
    TranslationBlock *tb;
    int i;
    address &= TARGET_PAGE_MASK;
    for(i = 0;i < CODE_GEN_PHYS_HASH_SIZE; i++) {
        for(tb = tb_phys_hash[i]; tb != NULL; tb = tb->phys_hash_next) {
            if (!(address + TARGET_PAGE_SIZE <= tb->pc ||
                  address >= tb->pc + tb->size)) {
                printf("ERROR invalidate: address=%08lx PC=%08lx size=%04x\n",
                       address, (long)tb->pc, tb->size);
            }
        }
    }
}

/* verify that all the pages have correct rights for code */
static void tb_page_check(void)
{
    TranslationBlock *tb;
    int i, flags1, flags2;

    for(i = 0;i < CODE_GEN_PHYS_HASH_SIZE; i++) {
        for(tb = tb_phys_hash[i]; tb != NULL; tb = tb->phys_hash_next) {
            flags1 = page_get_flags(tb->pc);
            flags2 = page_get_flags(tb->pc + tb->size - 1);
            if ((flags1 & PAGE_WRITE) || (flags2 & PAGE_WRITE)) {
                printf("ERROR page flags: PC=%08lx size=%04x f1=%x f2=%x\n",
                       (long)tb->pc, tb->size, flags1, flags2);
            }
        }
    }
}

void tb_jmp_check(TranslationBlock *tb)
{
    TranslationBlock *tb1;
    unsigned int n1;

    /* suppress any remaining jumps to this TB */
    tb1 = tb->jmp_first;
    for(;;) {
        n1 = (long)tb1 & 3;
        tb1 = (TranslationBlock *)((long)tb1 & ~3);
        if (n1 == 2)
            break;
        tb1 = tb1->jmp_next[n1];
    }
    /* check end of list */
    if (tb1 != tb) {
        printf("ERROR: jmp_list from 0x%08lx\n", (long)tb);
    }
}

#endif

/* invalidate one TB */
static inline void tb_remove(TranslationBlock **ptb, TranslationBlock *tb,
                             int next_offset)
{
    TranslationBlock *tb1;
    for(;;) {
        tb1 = *ptb;
        if (tb1 == tb) {
            *ptb = *(TranslationBlock **)((char *)tb1 + next_offset);
            break;
        }
        ptb = (TranslationBlock **)((char *)tb1 + next_offset);
    }
}

static inline void tb_page_remove(TranslationBlock **ptb, TranslationBlock *tb)
{
    TranslationBlock *tb1;
    unsigned int n1;

    for(;;) {
        tb1 = *ptb;
        n1 = (long)tb1 & 3;
        tb1 = (TranslationBlock *)((long)tb1 & ~3);
        if (tb1 == tb) {
            *ptb = tb1->page_next[n1];
            break;
        }
        ptb = &tb1->page_next[n1];
    }
}

static inline void tb_jmp_remove(TranslationBlock *tb, int n)
{
    TranslationBlock *tb1, **ptb;
    unsigned int n1;

    ptb = &tb->jmp_next[n];
    tb1 = *ptb;
    if (tb1) {
        /* find tb(n) in circular list */
        for(;;) {
            tb1 = *ptb;
            n1 = (long)tb1 & 3;
            tb1 = (TranslationBlock *)((long)tb1 & ~3);
            if (n1 == n && tb1 == tb)
                break;
            if (n1 == 2) {
                ptb = &tb1->jmp_first;
            } else {
                ptb = &tb1->jmp_next[n1];
            }
        }
        /* now we can suppress tb(n) from the list */
        *ptb = tb->jmp_next[n];

        tb->jmp_next[n] = NULL;
    }
}

/* reset the jump entry 'n' of a TB so that it is not chained to
   another TB */
static inline void tb_reset_jump(TranslationBlock *tb, int n)
{
    tb_set_jmp_target(tb, n, (unsigned long)(tb->tc_ptr + tb->tb_next_offset[n]));
}

void tb_phys_invalidate(TranslationBlock *tb, target_ulong page_addr)
{
    CPUState *env;
    PageDesc *p;
    unsigned int h, n1;
    target_phys_addr_t phys_pc;
    TranslationBlock *tb1, *tb2;

    /* remove the TB from the hash list */
    phys_pc = tb->page_addr[0] + (tb->pc & ~TARGET_PAGE_MASK);
    h = tb_phys_hash_func(phys_pc);
    tb_remove(&tb_phys_hash[h], tb,
              offsetof(TranslationBlock, phys_hash_next));

    /* remove the TB from the page list */
    if (tb->page_addr[0] != page_addr) {
        p = page_find(tb->page_addr[0] >> TARGET_PAGE_BITS);
        tb_page_remove(&p->first_tb, tb);
        invalidate_page_bitmap(p);
    }
    if (tb->page_addr[1] != -1 && tb->page_addr[1] != page_addr) {
        p = page_find(tb->page_addr[1] >> TARGET_PAGE_BITS);
        tb_page_remove(&p->first_tb, tb);
        invalidate_page_bitmap(p);
    }

    tb_invalidated_flag = 1;

    /* remove the TB from the hash list */
    h = tb_jmp_cache_hash_func(tb->pc);
    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        if (env->tb_jmp_cache[h] == tb)
            env->tb_jmp_cache[h] = NULL;
    }

    /* suppress this TB from the two jump lists */
    tb_jmp_remove(tb, 0);
    tb_jmp_remove(tb, 1);

    /* suppress any remaining jumps to this TB */
    tb1 = tb->jmp_first;
    for(;;) {
        n1 = (long)tb1 & 3;
        if (n1 == 2)
            break;
        tb1 = (TranslationBlock *)((long)tb1 & ~3);
        tb2 = tb1->jmp_next[n1];
        tb_reset_jump(tb1, n1);
        tb1->jmp_next[n1] = NULL;
        tb1 = tb2;
    }
    tb->jmp_first = (TranslationBlock *)((long)tb | 2); /* fail safe */

    tb_phys_invalidate_count++;
}

static inline void set_bits(uint8_t *tab, int start, int len)
{
    int end, mask, end1;

    end = start + len;
    tab += start >> 3;
    mask = 0xff << (start & 7);
    if ((start & ~7) == (end & ~7)) {
        if (start < end) {
            mask &= ~(0xff << (end & 7));
            *tab |= mask;
        }
    } else {
        *tab++ |= mask;
        start = (start + 8) & ~7;
        end1 = end & ~7;
        while (start < end1) {
            *tab++ = 0xff;
            start += 8;
        }
        if (start < end) {
            mask = ~(0xff << (end & 7));
            *tab |= mask;
        }
    }
}

static void build_page_bitmap(PageDesc *p)
{
    int n, tb_start, tb_end;
    TranslationBlock *tb;

    p->code_bitmap = qemu_mallocz(TARGET_PAGE_SIZE / 8);
    if (!p->code_bitmap)
        return;

    tb = p->first_tb;
    while (tb != NULL) {
        n = (long)tb & 3;
        tb = (TranslationBlock *)((long)tb & ~3);
        /* NOTE: this is subtle as a TB may span two physical pages */
        if (n == 0) {
            /* NOTE: tb_end may be after the end of the page, but
               it is not a problem */
            tb_start = tb->pc & ~TARGET_PAGE_MASK;
            tb_end = tb_start + tb->size;
            if (tb_end > TARGET_PAGE_SIZE)
                tb_end = TARGET_PAGE_SIZE;
        } else {
            tb_start = 0;
            tb_end = ((tb->pc + tb->size) & ~TARGET_PAGE_MASK);
        }
        set_bits(p->code_bitmap, tb_start, tb_end - tb_start);
        tb = tb->page_next[n];
    }
}

TranslationBlock *tb_gen_code(CPUState *env,
                              target_ulong pc, target_ulong cs_base,
                              int flags, int cflags)
{
    TranslationBlock *tb;
    uint8_t *tc_ptr;
    target_ulong phys_pc, phys_page2, virt_page2;
    int code_gen_size;

    phys_pc = get_phys_addr_code(env, pc);
    tb = tb_alloc(pc);
    if (!tb) {
        /* flush must be done */
        tb_flush(env);
        /* cannot fail at this point */
        tb = tb_alloc(pc);
        /* Don't forget to invalidate previous TB info.  */
        tb_invalidated_flag = 1;
    }
    tc_ptr = code_gen_ptr;
    tb->tc_ptr = tc_ptr;
    tb->cs_base = cs_base;
    tb->flags = flags;
    tb->cflags = cflags;
#ifdef CONFIG_TRACE
    tb->bb_rec = NULL;
    tb->prev_time = 0;
#endif
    cpu_gen_code(env, tb, &code_gen_size);
    code_gen_ptr = (void *)(((unsigned long)code_gen_ptr + code_gen_size + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));

    /* check next page if needed */
    virt_page2 = (pc + tb->size - 1) & TARGET_PAGE_MASK;
    phys_page2 = -1;
    if ((pc & TARGET_PAGE_MASK) != virt_page2) {
        phys_page2 = get_phys_addr_code(env, virt_page2);
    }
    tb_link_phys(tb, phys_pc, phys_page2);
    return tb;
}

/* invalidate all TBs which intersect with the target physical page
   starting in range [start;end[. NOTE: start and end must refer to
   the same physical page. 'is_cpu_write_access' should be true if called
   from a real cpu write access: the virtual CPU will exit the current
   TB if code is modified inside this TB. */
void tb_invalidate_phys_page_range(target_phys_addr_t start, target_phys_addr_t end,
                                   int is_cpu_write_access)
{
    int n, current_tb_modified, current_tb_not_found, current_flags;
    CPUState *env = cpu_single_env;
    PageDesc *p;
    TranslationBlock *tb, *tb_next, *current_tb, *saved_tb;
    target_ulong tb_start, tb_end;
    target_ulong current_pc, current_cs_base;

    p = page_find(start >> TARGET_PAGE_BITS);
    if (!p)
        return;
    if (!p->code_bitmap &&
        ++p->code_write_count >= SMC_BITMAP_USE_THRESHOLD &&
        is_cpu_write_access) {
        /* build code bitmap */
        build_page_bitmap(p);
    }

    /* we remove all the TBs in the range [start, end[ */
    /* XXX: see if in some cases it could be faster to invalidate all the code */
    current_tb_not_found = is_cpu_write_access;
    current_tb_modified = 0;
    current_tb = NULL; /* avoid warning */
    current_pc = 0; /* avoid warning */
    current_cs_base = 0; /* avoid warning */
    current_flags = 0; /* avoid warning */
    tb = p->first_tb;
    while (tb != NULL) {
        n = (long)tb & 3;
        tb = (TranslationBlock *)((long)tb & ~3);
        tb_next = tb->page_next[n];
        /* NOTE: this is subtle as a TB may span two physical pages */
        if (n == 0) {
            /* NOTE: tb_end may be after the end of the page, but
               it is not a problem */
            tb_start = tb->page_addr[0] + (tb->pc & ~TARGET_PAGE_MASK);
            tb_end = tb_start + tb->size;
        } else {
            tb_start = tb->page_addr[1];
            tb_end = tb_start + ((tb->pc + tb->size) & ~TARGET_PAGE_MASK);
        }
        if (!(tb_end <= start || tb_start >= end)) {
#ifdef TARGET_HAS_PRECISE_SMC
            if (current_tb_not_found) {
                current_tb_not_found = 0;
                current_tb = NULL;
                if (env->mem_io_pc) {
                    /* now we have a real cpu fault */
                    current_tb = tb_find_pc(env->mem_io_pc);
                }
            }
            if (current_tb == tb &&
                (current_tb->cflags & CF_COUNT_MASK) != 1) {
                /* If we are modifying the current TB, we must stop
                its execution. We could be more precise by checking
                that the modification is after the current PC, but it
                would require a specialized function to partially
                restore the CPU state */

                current_tb_modified = 1;
                cpu_restore_state(current_tb, env,
                                  env->mem_io_pc, NULL);
#if defined(TARGET_I386)
                current_flags = env->hflags;
                current_flags |= (env->eflags & (IOPL_MASK | TF_MASK | VM_MASK));
                current_cs_base = (target_ulong)env->segs[R_CS].base;
                current_pc = current_cs_base + env->eip;
#else
#error unsupported CPU
#endif
            }
#endif /* TARGET_HAS_PRECISE_SMC */
            /* we need to do that to handle the case where a signal
               occurs while doing tb_phys_invalidate() */
            saved_tb = NULL;
            if (env) {
                saved_tb = env->current_tb;
                env->current_tb = NULL;
            }
            tb_phys_invalidate(tb, -1);
            if (env) {
                env->current_tb = saved_tb;
                if (env->interrupt_request && env->current_tb)
                    cpu_interrupt(env, env->interrupt_request);
            }
        }
        tb = tb_next;
    }
#if !defined(CONFIG_USER_ONLY)
    /* if no code remaining, no need to continue to use slow writes */
    if (!p->first_tb) {
        invalidate_page_bitmap(p);
        if (is_cpu_write_access) {
            tlb_unprotect_code_phys(env, start, env->mem_io_vaddr);
        }
    }
#endif
#ifdef TARGET_HAS_PRECISE_SMC
    if (current_tb_modified) {
        /* we generate a block containing just the instruction
           modifying the memory. It will ensure that it cannot modify
           itself */
        env->current_tb = NULL;
        tb_gen_code(env, current_pc, current_cs_base, current_flags, 1);
        cpu_resume_from_signal(env, NULL);
    }
#endif
}

/* len must be <= 8 and start must be a multiple of len */
static inline void tb_invalidate_phys_page_fast(target_phys_addr_t start, int len)
{
    PageDesc *p;
    int offset, b;
#if 0
    if (1) {
        if (loglevel) {
            fprintf(logfile, "modifying code at 0x%x size=%d EIP=%x PC=%08x\n",
                   cpu_single_env->mem_io_vaddr, len,
                   cpu_single_env->eip,
                   cpu_single_env->eip + (long)cpu_single_env->segs[R_CS].base);
        }
    }
#endif
    p = page_find(start >> TARGET_PAGE_BITS);
    if (!p)
        return;
    if (p->code_bitmap) {
        offset = start & ~TARGET_PAGE_MASK;
        b = p->code_bitmap[offset >> 3] >> (offset & 7);
        if (b & ((1 << len) - 1))
            goto do_invalidate;
    } else {
    do_invalidate:
        tb_invalidate_phys_page_range(start, start + len, 1);
    }
}

#if !defined(CONFIG_SOFTMMU)
static void tb_invalidate_phys_page(target_phys_addr_t addr,
                                    unsigned long pc, void *puc)
{
    int n, current_flags, current_tb_modified;
    target_ulong current_pc, current_cs_base;
    PageDesc *p;
    TranslationBlock *tb, *current_tb;
#ifdef TARGET_HAS_PRECISE_SMC
    CPUState *env = cpu_single_env;
#endif

    addr &= TARGET_PAGE_MASK;
    p = page_find(addr >> TARGET_PAGE_BITS);
    if (!p)
        return;
    tb = p->first_tb;
    current_tb_modified = 0;
    current_tb = NULL;
    current_pc = 0; /* avoid warning */
    current_cs_base = 0; /* avoid warning */
    current_flags = 0; /* avoid warning */
#ifdef TARGET_HAS_PRECISE_SMC
    if (tb && pc != 0) {
        current_tb = tb_find_pc(pc);
    }
#endif
    while (tb != NULL) {
        n = (long)tb & 3;
        tb = (TranslationBlock *)((long)tb & ~3);
#ifdef TARGET_HAS_PRECISE_SMC
        if (current_tb == tb &&
            (current_tb->cflags & CF_COUNT_MASK) != 1) {
                /* If we are modifying the current TB, we must stop
                   its execution. We could be more precise by checking
                   that the modification is after the current PC, but it
                   would require a specialized function to partially
                   restore the CPU state */

            current_tb_modified = 1;
            cpu_restore_state(current_tb, env, pc, puc);
#if defined(TARGET_I386)
            current_flags = env->hflags;
            current_flags |= (env->eflags & (IOPL_MASK | TF_MASK | VM_MASK));
            current_cs_base = (target_ulong)env->segs[R_CS].base;
            current_pc = current_cs_base + env->eip;
#else
#error unsupported CPU
#endif
        }
#endif /* TARGET_HAS_PRECISE_SMC */
        tb_phys_invalidate(tb, addr);
        tb = tb->page_next[n];
    }
    p->first_tb = NULL;
#ifdef TARGET_HAS_PRECISE_SMC
    if (current_tb_modified) {
        /* we generate a block containing just the instruction
           modifying the memory. It will ensure that it cannot modify
           itself */
        env->current_tb = NULL;
        tb_gen_code(env, current_pc, current_cs_base, current_flags, 1);
        cpu_resume_from_signal(env, puc);
    }
#endif
}
#endif

/* add the tb in the target page and protect it if necessary */
static inline void tb_alloc_page(TranslationBlock *tb,
                                 unsigned int n, target_ulong page_addr)
{
    PageDesc *p;
    TranslationBlock *last_first_tb;

    tb->page_addr[n] = page_addr;
    p = page_find_alloc(page_addr >> TARGET_PAGE_BITS);
    tb->page_next[n] = p->first_tb;
    last_first_tb = p->first_tb;
    p->first_tb = (TranslationBlock *)((long)tb | n);
    invalidate_page_bitmap(p);

#if defined(TARGET_HAS_SMC) || 1

#if defined(CONFIG_USER_ONLY)
    if (p->flags & PAGE_WRITE) {
        target_ulong addr;
        PageDesc *p2;
        int prot;

        /* force the host page as non writable (writes will have a
           page fault + mprotect overhead) */
        page_addr &= qemu_host_page_mask;
        prot = 0;
        for(addr = page_addr; addr < page_addr + qemu_host_page_size;
            addr += TARGET_PAGE_SIZE) {

            p2 = page_find (addr >> TARGET_PAGE_BITS);
            if (!p2)
                continue;
            prot |= p2->flags;
            p2->flags &= ~PAGE_WRITE;
            page_get_flags(addr);
          }
        mprotect(g2h(page_addr), qemu_host_page_size,
                 (prot & PAGE_BITS) & ~PAGE_WRITE);
#ifdef DEBUG_TB_INVALIDATE
        printf("protecting code page: 0x" TARGET_FMT_lx "\n",
               page_addr);
#endif
    }
#else
    /* if some code is already present, then the pages are already
       protected. So we handle the case where only the first TB is
       allocated in a physical page */
    if (!last_first_tb) {
        tlb_protect_code(page_addr);
    }
#endif

#endif /* TARGET_HAS_SMC */
}

/* Allocate a new translation block. Flush the translation buffer if
   too many translation blocks or too much generated code. */
TranslationBlock *tb_alloc(target_ulong pc)
{
    TranslationBlock *tb;

    if (nb_tbs >= code_gen_max_blocks ||
        (code_gen_ptr - code_gen_buffer) >= code_gen_buffer_max_size)
        return NULL;
    tb = &tbs[nb_tbs++];
    tb->pc = pc;
    tb->cflags = 0;
    return tb;
}

void tb_free(TranslationBlock *tb)
{
    /* In practice this is mostly used for single use temporary TB
       Ignore the hard cases and just back up if this TB happens to
       be the last one generated.  */
    if (nb_tbs > 0 && tb == &tbs[nb_tbs - 1]) {
        code_gen_ptr = tb->tc_ptr;
        nb_tbs--;
    }
}

/* add a new TB and link it to the physical page tables. phys_page2 is
   (-1) to indicate that only one page contains the TB. */
void tb_link_phys(TranslationBlock *tb,
                  target_ulong phys_pc, target_ulong phys_page2)
{
    unsigned int h;
    TranslationBlock **ptb;

    /* Grab the mmap lock to stop another thread invalidating this TB
       before we are done.  */
    mmap_lock();
    /* add in the physical hash table */
    h = tb_phys_hash_func(phys_pc);
    ptb = &tb_phys_hash[h];
    tb->phys_hash_next = *ptb;
    *ptb = tb;

    /* add in the page list */
    tb_alloc_page(tb, 0, phys_pc & TARGET_PAGE_MASK);
    if (phys_page2 != -1)
        tb_alloc_page(tb, 1, phys_page2);
    else
        tb->page_addr[1] = -1;

    tb->jmp_first = (TranslationBlock *)((long)tb | 2);
    tb->jmp_next[0] = NULL;
    tb->jmp_next[1] = NULL;

    /* init original jump addresses */
    if (tb->tb_next_offset[0] != 0xffff)
        tb_reset_jump(tb, 0);
    if (tb->tb_next_offset[1] != 0xffff)
        tb_reset_jump(tb, 1);

#ifdef DEBUG_TB_CHECK
    tb_page_check();
#endif
    mmap_unlock();
}

/* find the TB 'tb' such that tb[0].tc_ptr <= tc_ptr <
   tb[1].tc_ptr. Return NULL if not found */
TranslationBlock *tb_find_pc(unsigned long tc_ptr)
{
    int m_min, m_max, m;
    unsigned long v;
    TranslationBlock *tb;

    if (nb_tbs <= 0)
        return NULL;
    if (tc_ptr < (unsigned long)code_gen_buffer ||
        tc_ptr >= (unsigned long)code_gen_ptr)
        return NULL;
    /* binary search (cf Knuth) */
    m_min = 0;
    m_max = nb_tbs - 1;
    while (m_min <= m_max) {
        m = (m_min + m_max) >> 1;
        tb = &tbs[m];
        v = (unsigned long)tb->tc_ptr;
        if (v == tc_ptr)
            return tb;
        else if (tc_ptr < v) {
            m_max = m - 1;
        } else {
            m_min = m + 1;
        }
    }
    return &tbs[m_max];
}

static void tb_reset_jump_recursive(TranslationBlock *tb);

static inline void tb_reset_jump_recursive2(TranslationBlock *tb, int n)
{
    TranslationBlock *tb1, *tb_next, **ptb;
    unsigned int n1;

    tb1 = tb->jmp_next[n];
    if (tb1 != NULL) {
        /* find head of list */
        for(;;) {
            n1 = (long)tb1 & 3;
            tb1 = (TranslationBlock *)((long)tb1 & ~3);
            if (n1 == 2)
                break;
            tb1 = tb1->jmp_next[n1];
        }
        /* we are now sure now that tb jumps to tb1 */
        tb_next = tb1;

        /* remove tb from the jmp_first list */
        ptb = &tb_next->jmp_first;
        for(;;) {
            tb1 = *ptb;
            n1 = (long)tb1 & 3;
            tb1 = (TranslationBlock *)((long)tb1 & ~3);
            if (n1 == n && tb1 == tb)
                break;
            ptb = &tb1->jmp_next[n1];
        }
        *ptb = tb->jmp_next[n];
        tb->jmp_next[n] = NULL;

        /* suppress the jump to next tb in generated code */
        tb_reset_jump(tb, n);

        /* suppress jumps in the tb on which we could have jumped */
        tb_reset_jump_recursive(tb_next);
    }
}

static void tb_reset_jump_recursive(TranslationBlock *tb)
{
    tb_reset_jump_recursive2(tb, 0);
    tb_reset_jump_recursive2(tb, 1);
}

#if defined(TARGET_HAS_ICE)
static void breakpoint_invalidate(CPUState *env, target_ulong pc)
{
    target_phys_addr_t addr;
    target_ulong pd;
    ram_addr_t ram_addr;
    PhysPageDesc *p;

    addr = cpu_get_phys_page_debug(env, pc);
    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }
    ram_addr = (pd & TARGET_PAGE_MASK) | (pc & ~TARGET_PAGE_MASK);
    tb_invalidate_phys_page_range(ram_addr, ram_addr + 1, 0);
}
#endif

/* Add a watchpoint.  */
int cpu_watchpoint_insert(CPUState *env, target_ulong addr, int type)
{
    int i;

    for (i = 0; i < env->nb_watchpoints; i++) {
        if (addr == env->watchpoint[i].vaddr)
            return 0;
    }
    if (env->nb_watchpoints >= MAX_WATCHPOINTS)
        return -1;

    i = env->nb_watchpoints++;
    env->watchpoint[i].vaddr = addr;
    env->watchpoint[i].type = type;
    tlb_flush_page(env, addr);
    /* FIXME: This flush is needed because of the hack to make memory ops
       terminate the TB.  It can be removed once the proper IO trap and
       re-execute bits are in.  */
    tb_flush(env);
    return i;
}

/* Remove a watchpoint.  */
int cpu_watchpoint_remove(CPUState *env, target_ulong addr)
{
    int i;

    for (i = 0; i < env->nb_watchpoints; i++) {
        if (addr == env->watchpoint[i].vaddr) {
            env->nb_watchpoints--;
            env->watchpoint[i] = env->watchpoint[env->nb_watchpoints];
            tlb_flush_page(env, addr);
            return 0;
        }
    }
    return -1;
}

/* Remove all watchpoints. */
void cpu_watchpoint_remove_all(CPUState *env) {
    int i;

    for (i = 0; i < env->nb_watchpoints; i++) {
        tlb_flush_page(env, env->watchpoint[i].vaddr);
    }
    env->nb_watchpoints = 0;
}

/* add a breakpoint. EXCP_DEBUG is returned by the CPU loop if a
   breakpoint is reached */
int cpu_breakpoint_insert(CPUState *env, target_ulong pc)
{
#if defined(TARGET_HAS_ICE)
    int i;

    for(i = 0; i < env->nb_breakpoints; i++) {
        if (env->breakpoints[i] == pc)
            return 0;
    }

    if (env->nb_breakpoints >= MAX_BREAKPOINTS)
        return -1;
    env->breakpoints[env->nb_breakpoints++] = pc;

    breakpoint_invalidate(env, pc);
    return 0;
#else
    return -1;
#endif
}

/* remove all breakpoints */
void cpu_breakpoint_remove_all(CPUState *env) {
#if defined(TARGET_HAS_ICE)
    int i;
    for(i = 0; i < env->nb_breakpoints; i++) {
        breakpoint_invalidate(env, env->breakpoints[i]);
    }
    env->nb_breakpoints = 0;
#endif
}

/* remove a breakpoint */
int cpu_breakpoint_remove(CPUState *env, target_ulong pc)
{
#if defined(TARGET_HAS_ICE)
    int i;
    for(i = 0; i < env->nb_breakpoints; i++) {
        if (env->breakpoints[i] == pc)
            goto found;
    }
    return -1;
 found:
    env->nb_breakpoints--;
    if (i < env->nb_breakpoints)
      env->breakpoints[i] = env->breakpoints[env->nb_breakpoints];

    breakpoint_invalidate(env, pc);
    return 0;
#else
    return -1;
#endif
}

/* enable or disable single step mode. EXCP_DEBUG is returned by the
   CPU loop after each instruction */
void cpu_single_step(CPUState *env, int enabled)
{
#if defined(TARGET_HAS_ICE)
    if (env->singlestep_enabled != enabled) {
        env->singlestep_enabled = enabled;
        /* must flush all the translated code to avoid inconsistancies */
        /* XXX: only flush what is necessary */
        tb_flush(env);
    }
#endif
}

/* enable or disable low levels log */
void cpu_set_log(int log_flags)
{
    loglevel = log_flags;
    if (loglevel && !logfile) {
        logfile = fopen(logfilename, log_append ? "a" : "w");
        if (!logfile) {
            perror(logfilename);
            _exit(1);
        }
#if !defined(CONFIG_SOFTMMU)
        /* must avoid mmap() usage of glibc by setting a buffer "by hand" */
        {
            static uint8_t logfile_buf[4096];
            setvbuf(logfile, logfile_buf, _IOLBF, sizeof(logfile_buf));
        }
#else
        setvbuf(logfile, NULL, _IOLBF, 0);
#endif
        log_append = 1;
    }
    if (!loglevel && logfile) {
        fclose(logfile);
        logfile = NULL;
    }
}

void cpu_set_log_filename(const char *filename)
{
    logfilename = strdup(filename);
    if (logfile) {
        fclose(logfile);
        logfile = NULL;
    }
    cpu_set_log(loglevel);
}

/* mask must never be zero, except for A20 change call */
void cpu_interrupt(CPUState *env, int mask)
{
#if !defined(USE_NPTL)
    TranslationBlock *tb;
    static spinlock_t interrupt_lock = SPIN_LOCK_UNLOCKED;
#endif
    int old_mask;

    old_mask = env->interrupt_request;
    /* FIXME: This is probably not threadsafe.  A different thread could
       be in the middle of a read-modify-write operation.  */
    env->interrupt_request |= mask;
#if defined(USE_NPTL)
    /* FIXME: TB unchaining isn't SMP safe.  For now just ignore the
       problem and hope the cpu will stop of its own accord.  For userspace
       emulation this often isn't actually as bad as it sounds.  Often
       signals are used primarily to interrupt blocking syscalls.  */
#else
    if (use_icount) {
        env->icount_decr.u16.high = 0xffff;
#ifndef CONFIG_USER_ONLY
        /* CPU_INTERRUPT_EXIT isn't a real interrupt.  It just means
           an async event happened and we need to process it.  */
        if (!can_do_io(env)
            && (mask & ~(old_mask | CPU_INTERRUPT_EXIT)) != 0) {
            cpu_abort(env, "Raised interrupt while not in I/O function");
        }
#endif
    } else {
        tb = env->current_tb;
        /* if the cpu is currently executing code, we must unlink it and
           all the potentially executing TB */
        if (tb && !testandset(&interrupt_lock)) {
            env->current_tb = NULL;
            tb_reset_jump_recursive(tb);
            resetlock(&interrupt_lock);
        }
    }
#endif
}

void cpu_reset_interrupt(CPUState *env, int mask)
{
    env->interrupt_request &= ~mask;
}

CPULogItem cpu_log_items[] = {
    { CPU_LOG_TB_OUT_ASM, "out_asm",
      "show generated host assembly code for each compiled TB" },
    { CPU_LOG_TB_IN_ASM, "in_asm",
      "show target assembly code for each compiled TB" },
    { CPU_LOG_TB_OP, "op",
      "show micro ops for each compiled TB" },
    { CPU_LOG_TB_OP_OPT, "op_opt",
      "show micro ops "
#ifdef TARGET_I386
      "before eflags optimization and "
#endif
      "after liveness analysis" },
    { CPU_LOG_INT, "int",
      "show interrupts/exceptions in short format" },
    { CPU_LOG_EXEC, "exec",
      "show trace before each executed TB (lots of logs)" },
    { CPU_LOG_TB_CPU, "cpu",
      "show CPU state before block translation" },
#ifdef TARGET_I386
    { CPU_LOG_PCALL, "pcall",
      "show protected mode far calls/returns/exceptions" },
#endif
#ifdef DEBUG_IOPORT
    { CPU_LOG_IOPORT, "ioport",
      "show all i/o ports accesses" },
#endif
    { 0, NULL, NULL },
};

static int cmp1(const char *s1, int n, const char *s2)
{
    if (strlen(s2) != n)
        return 0;
    return memcmp(s1, s2, n) == 0;
}

/* takes a comma separated list of log masks. Return 0 if error. */
int cpu_str_to_log_mask(const char *str)
{
    CPULogItem *item;
    int mask;
    const char *p, *p1;

    p = str;
    mask = 0;
    for(;;) {
        p1 = strchr(p, ',');
        if (!p1)
            p1 = p + strlen(p);
	if(cmp1(p,p1-p,"all")) {
		for(item = cpu_log_items; item->mask != 0; item++) {
			mask |= item->mask;
		}
	} else {
        for(item = cpu_log_items; item->mask != 0; item++) {
            if (cmp1(p, p1 - p, item->name))
                goto found;
        }
        return 0;
	}
    found:
        mask |= item->mask;
        if (*p1 != ',')
            break;
        p = p1 + 1;
    }
    return mask;
}

void cpu_abort(CPUState *env, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    fprintf(stderr, "qemu: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
#ifdef TARGET_I386
    cpu_dump_state(env, stderr, fprintf, X86_DUMP_FPU | X86_DUMP_CCOP);
#else
    cpu_dump_state(env, stderr, fprintf, 0);
#endif
    if (logfile) {
        fprintf(logfile, "qemu: fatal: ");
        vfprintf(logfile, fmt, ap2);
        fprintf(logfile, "\n");
#ifdef TARGET_I386
        cpu_dump_state(env, logfile, fprintf, X86_DUMP_FPU | X86_DUMP_CCOP);
#else
        cpu_dump_state(env, logfile, fprintf, 0);
#endif
        fflush(logfile);
        fclose(logfile);
    }
    va_end(ap2);
    va_end(ap);
    abort();
}

CPUState *cpu_copy(CPUState *env)
{
    CPUState *new_env = cpu_init(env->cpu_model_str);
    /* preserve chaining and index */
    CPUState *next_cpu = new_env->next_cpu;
    int cpu_index = new_env->cpu_index;
    memcpy(new_env, env, sizeof(CPUState));
    new_env->next_cpu = next_cpu;
    new_env->cpu_index = cpu_index;
    return new_env;
}

#if !defined(CONFIG_USER_ONLY)

static inline void tlb_flush_jmp_cache(CPUState *env, target_ulong addr)
{
    unsigned int i;

    /* Discard jump cache entries for any tb which might potentially
       overlap the flushed page.  */
    i = tb_jmp_cache_hash_page(addr - TARGET_PAGE_SIZE);
    memset (&env->tb_jmp_cache[i], 0, 
	    TB_JMP_PAGE_SIZE * sizeof(TranslationBlock *));

    i = tb_jmp_cache_hash_page(addr);
    memset (&env->tb_jmp_cache[i], 0, 
	    TB_JMP_PAGE_SIZE * sizeof(TranslationBlock *));
}

/* NOTE: if flush_global is true, also flush global entries (not
   implemented yet) */
void tlb_flush(CPUState *env, int flush_global)
{
    int i;

#if defined(DEBUG_TLB)
    printf("tlb_flush:\n");
#endif
    /* must reset current TB so that interrupts cannot modify the
       links while we are modifying them */
    env->current_tb = NULL;

    for(i = 0; i < CPU_TLB_SIZE; i++) {
        env->tlb_table[0][i].addr_read = -1;
        env->tlb_table[0][i].addr_write = -1;
        env->tlb_table[0][i].addr_code = -1;
        env->tlb_table[1][i].addr_read = -1;
        env->tlb_table[1][i].addr_write = -1;
        env->tlb_table[1][i].addr_code = -1;
#if (NB_MMU_MODES >= 3)
        env->tlb_table[2][i].addr_read = -1;
        env->tlb_table[2][i].addr_write = -1;
        env->tlb_table[2][i].addr_code = -1;
#if (NB_MMU_MODES == 4)
        env->tlb_table[3][i].addr_read = -1;
        env->tlb_table[3][i].addr_write = -1;
        env->tlb_table[3][i].addr_code = -1;
#endif
#endif
    }

    memset (env->tb_jmp_cache, 0, TB_JMP_CACHE_SIZE * sizeof (void *));

#ifdef USE_KQEMU
    if (env->kqemu_enabled) {
        kqemu_flush(env, flush_global);
    }
#endif
    tlb_flush_count++;
}

static inline void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr)
{
    if (addr == (tlb_entry->addr_read &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_write &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_code &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        tlb_entry->addr_read = -1;
        tlb_entry->addr_write = -1;
        tlb_entry->addr_code = -1;
    }
}

void tlb_flush_page(CPUState *env, target_ulong addr)
{
    int i;

#if defined(DEBUG_TLB)
    printf("tlb_flush_page: " TARGET_FMT_lx "\n", addr);
#endif
    /* must reset current TB so that interrupts cannot modify the
       links while we are modifying them */
    env->current_tb = NULL;

    addr &= TARGET_PAGE_MASK;
    i = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    tlb_flush_entry(&env->tlb_table[0][i], addr);
    tlb_flush_entry(&env->tlb_table[1][i], addr);
#if (NB_MMU_MODES >= 3)
    tlb_flush_entry(&env->tlb_table[2][i], addr);
#if (NB_MMU_MODES == 4)
    tlb_flush_entry(&env->tlb_table[3][i], addr);
#endif
#endif

    tlb_flush_jmp_cache(env, addr);

#ifdef USE_KQEMU
    if (env->kqemu_enabled) {
        kqemu_flush_page(env, addr);
    }
#endif
}

/* update the TLBs so that writes to code in the virtual page 'addr'
   can be detected */
static void tlb_protect_code(ram_addr_t ram_addr)
{
    cpu_physical_memory_reset_dirty(ram_addr,
                                    ram_addr + TARGET_PAGE_SIZE,
                                    CODE_DIRTY_FLAG);
}

/* update the TLB so that writes in physical page 'phys_addr' are no longer
   tested for self modifying code */
static void tlb_unprotect_code_phys(CPUState *env, ram_addr_t ram_addr,
                                    target_ulong vaddr)
{
    phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS] |= CODE_DIRTY_FLAG;
}

static inline void tlb_reset_dirty_range(CPUTLBEntry *tlb_entry,
                                         unsigned long start, unsigned long length)
{
    unsigned long addr;
    if ((tlb_entry->addr_write & ~TARGET_PAGE_MASK) == IO_MEM_RAM) {
        addr = (tlb_entry->addr_write & TARGET_PAGE_MASK) + tlb_entry->addend;
        if ((addr - start) < length) {
            tlb_entry->addr_write = (tlb_entry->addr_write & TARGET_PAGE_MASK) | TLB_NOTDIRTY;
        }
    }
}

void cpu_physical_memory_reset_dirty(ram_addr_t start, ram_addr_t end,
                                     int dirty_flags)
{
    CPUState *env;
    unsigned long length, start1;
    int i, mask, len;
    uint8_t *p;

    start &= TARGET_PAGE_MASK;
    end = TARGET_PAGE_ALIGN(end);

    length = end - start;
    if (length == 0)
        return;
    len = length >> TARGET_PAGE_BITS;
#ifdef USE_KQEMU
    /* XXX: should not depend on cpu context */
    env = first_cpu;
    if (env->kqemu_enabled) {
        ram_addr_t addr;
        addr = start;
        for(i = 0; i < len; i++) {
            kqemu_set_notdirty(env, addr);
            addr += TARGET_PAGE_SIZE;
        }
    }
#endif
    mask = ~dirty_flags;
    p = phys_ram_dirty + (start >> TARGET_PAGE_BITS);
    for(i = 0; i < len; i++)
        p[i] &= mask;

    /* we modify the TLB cache so that the dirty bit will be set again
       when accessing the range */
    start1 = start + (unsigned long)phys_ram_base;
    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        for(i = 0; i < CPU_TLB_SIZE; i++)
            tlb_reset_dirty_range(&env->tlb_table[0][i], start1, length);
        for(i = 0; i < CPU_TLB_SIZE; i++)
            tlb_reset_dirty_range(&env->tlb_table[1][i], start1, length);
#if (NB_MMU_MODES >= 3)
        for(i = 0; i < CPU_TLB_SIZE; i++)
            tlb_reset_dirty_range(&env->tlb_table[2][i], start1, length);
#if (NB_MMU_MODES == 4)
        for(i = 0; i < CPU_TLB_SIZE; i++)
            tlb_reset_dirty_range(&env->tlb_table[3][i], start1, length);
#endif
#endif
    }
}

static inline void tlb_update_dirty(CPUTLBEntry *tlb_entry)
{
    ram_addr_t ram_addr;

    if ((tlb_entry->addr_write & ~TARGET_PAGE_MASK) == IO_MEM_RAM) {
        ram_addr = (tlb_entry->addr_write & TARGET_PAGE_MASK) +
            tlb_entry->addend - (unsigned long)phys_ram_base;
        if (!cpu_physical_memory_is_dirty(ram_addr)) {
            tlb_entry->addr_write |= TLB_NOTDIRTY;
        }
    }
}

/* update the TLB according to the current state of the dirty bits */
void cpu_tlb_update_dirty(CPUState *env)
{
    int i;
    for(i = 0; i < CPU_TLB_SIZE; i++)
        tlb_update_dirty(&env->tlb_table[0][i]);
    for(i = 0; i < CPU_TLB_SIZE; i++)
        tlb_update_dirty(&env->tlb_table[1][i]);
#if (NB_MMU_MODES >= 3)
    for(i = 0; i < CPU_TLB_SIZE; i++)
        tlb_update_dirty(&env->tlb_table[2][i]);
#if (NB_MMU_MODES == 4)
    for(i = 0; i < CPU_TLB_SIZE; i++)
        tlb_update_dirty(&env->tlb_table[3][i]);
#endif
#endif
}

static inline void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr)
{
    if (tlb_entry->addr_write == (vaddr | TLB_NOTDIRTY))
        tlb_entry->addr_write = vaddr;
}

/* update the TLB corresponding to virtual page vaddr
   so that it is no longer dirty */
static inline void tlb_set_dirty(CPUState *env, target_ulong vaddr)
{
    int i;

    vaddr &= TARGET_PAGE_MASK;
    i = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    tlb_set_dirty1(&env->tlb_table[0][i], vaddr);
    tlb_set_dirty1(&env->tlb_table[1][i], vaddr);
#if (NB_MMU_MODES >= 3)
    tlb_set_dirty1(&env->tlb_table[2][i], vaddr);
#if (NB_MMU_MODES == 4)
    tlb_set_dirty1(&env->tlb_table[3][i], vaddr);
#endif
#endif
}

/* add a new TLB entry. At most one entry for a given virtual address
   is permitted. Return 0 if OK or 2 if the page could not be mapped
   (can only happen in non SOFTMMU mode for I/O pages or pages
   conflicting with the host address space). */
int tlb_set_page_exec(CPUState *env, target_ulong vaddr,
                      target_phys_addr_t paddr, int prot,
                      int mmu_idx, int is_softmmu)
{
    PhysPageDesc *p;
    unsigned long pd;
    unsigned int index;
    target_ulong address;
    target_ulong code_address;
    target_phys_addr_t addend;
    int ret;
    CPUTLBEntry *te;
    int i;
    target_phys_addr_t iotlb;

    p = phys_page_find(paddr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }
#if defined(DEBUG_TLB)
    printf("tlb_set_page: vaddr=" TARGET_FMT_lx " paddr=0x%08x prot=%x idx=%d smmu=%d pd=0x%08lx\n",
           vaddr, (int)paddr, prot, mmu_idx, is_softmmu, pd);
#endif

    ret = 0;
    address = vaddr;
    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM && !(pd & IO_MEM_ROMD)) {
        /* IO memory case (romd handled later) */
        address |= TLB_MMIO;
    }
    addend = (target_phys_addr_t)phys_ram_base + (pd & TARGET_PAGE_MASK);
    if ((pd & ~TARGET_PAGE_MASK) <= IO_MEM_ROM) {
        /* Normal RAM.  */
        iotlb = pd & TARGET_PAGE_MASK;
        if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_RAM)
            iotlb |= IO_MEM_NOTDIRTY;
        else
            iotlb |= IO_MEM_ROM;
    } else {
        /* IO handlers are currently passed a phsical address.
           It would be nice to pass an offset from the base address
           of that region.  This would avoid having to special case RAM,
           and avoid full address decoding in every device.
           We can't use the high bits of pd for this because
           IO_MEM_ROMD uses these as a ram address.  */
        iotlb = (pd & ~TARGET_PAGE_MASK) + paddr;
    }

    code_address = address;
    /* Make accesses to pages with watchpoints go via the
       watchpoint trap routines.  */
    for (i = 0; i < env->nb_watchpoints; i++) {
        if (vaddr == (env->watchpoint[i].vaddr & TARGET_PAGE_MASK)) {
            iotlb = io_mem_watch + paddr;
            /* TODO: The memory case can be optimized by not trapping
               reads of pages with a write breakpoint.  */
            address |= TLB_MMIO;
        }
    }

    index = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    env->iotlb[mmu_idx][index] = iotlb - vaddr;
    te = &env->tlb_table[mmu_idx][index];
    te->addend = addend - vaddr;
    if (prot & PAGE_READ) {
        te->addr_read = address;
    } else {
        te->addr_read = -1;
    }

    if (prot & PAGE_EXEC) {
        te->addr_code = code_address;
    } else {
        te->addr_code = -1;
    }
    if (prot & PAGE_WRITE) {
        if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_ROM ||
            (pd & IO_MEM_ROMD)) {
            /* Write access calls the I/O callback.  */
            te->addr_write = address | TLB_MMIO;
        } else if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_RAM &&
                   !cpu_physical_memory_is_dirty(pd)) {
            te->addr_write = address | TLB_NOTDIRTY;
        } else {
            te->addr_write = address;
        }
    } else {
        te->addr_write = -1;
    }
    return ret;
}

#else

void tlb_flush(CPUState *env, int flush_global)
{
}

void tlb_flush_page(CPUState *env, target_ulong addr)
{
}

int tlb_set_page_exec(CPUState *env, target_ulong vaddr,
                      target_phys_addr_t paddr, int prot,
                      int mmu_idx, int is_softmmu)
{
    return 0;
}

/* dump memory mappings */
void page_dump(FILE *f)
{
    unsigned long start, end;
    int i, j, prot, prot1;
    PageDesc *p;

    fprintf(f, "%-8s %-8s %-8s %s\n",
            "start", "end", "size", "prot");
    start = -1;
    end = -1;
    prot = 0;
    for(i = 0; i <= L1_SIZE; i++) {
        if (i < L1_SIZE)
            p = l1_map[i];
        else
            p = NULL;
        for(j = 0;j < L2_SIZE; j++) {
            if (!p)
                prot1 = 0;
            else
                prot1 = p[j].flags;
            if (prot1 != prot) {
                end = (i << (32 - L1_BITS)) | (j << TARGET_PAGE_BITS);
                if (start != -1) {
                    fprintf(f, "%08lx-%08lx %08lx %c%c%c\n",
                            start, end, end - start,
                            prot & PAGE_READ ? 'r' : '-',
                            prot & PAGE_WRITE ? 'w' : '-',
                            prot & PAGE_EXEC ? 'x' : '-');
                }
                if (prot1 != 0)
                    start = end;
                else
                    start = -1;
                prot = prot1;
            }
            if (!p)
                break;
        }
    }
}

int page_get_flags(target_ulong address)
{
    PageDesc *p;

    p = page_find(address >> TARGET_PAGE_BITS);
    if (!p)
        return 0;
    return p->flags;
}

/* modify the flags of a page and invalidate the code if
   necessary. The flag PAGE_WRITE_ORG is positionned automatically
   depending on PAGE_WRITE */
void page_set_flags(target_ulong start, target_ulong end, int flags)
{
    PageDesc *p;
    target_ulong addr;

    /* mmap_lock should already be held.  */
    start = start & TARGET_PAGE_MASK;
    end = TARGET_PAGE_ALIGN(end);
    if (flags & PAGE_WRITE)
        flags |= PAGE_WRITE_ORG;
    for(addr = start; addr < end; addr += TARGET_PAGE_SIZE) {
        p = page_find_alloc(addr >> TARGET_PAGE_BITS);
        /* We may be called for host regions that are outside guest
           address space.  */
        if (!p)
            return;
        /* if the write protection is set, then we invalidate the code
           inside */
        if (!(p->flags & PAGE_WRITE) &&
            (flags & PAGE_WRITE) &&
            p->first_tb) {
            tb_invalidate_phys_page(addr, 0, NULL);
        }
        p->flags = flags;
    }
}

int page_check_range(target_ulong start, target_ulong len, int flags)
{
    PageDesc *p;
    target_ulong end;
    target_ulong addr;

    end = TARGET_PAGE_ALIGN(start+len); /* must do before we loose bits in the next step */
    start = start & TARGET_PAGE_MASK;

    if( end < start )
        /* we've wrapped around */
        return -1;
    for(addr = start; addr < end; addr += TARGET_PAGE_SIZE) {
        p = page_find(addr >> TARGET_PAGE_BITS);
        if( !p )
            return -1;
        if( !(p->flags & PAGE_VALID) )
            return -1;

        if ((flags & PAGE_READ) && !(p->flags & PAGE_READ))
            return -1;
        if (flags & PAGE_WRITE) {
            if (!(p->flags & PAGE_WRITE_ORG))
                return -1;
            /* unprotect the page if it was put read-only because it
               contains translated code */
            if (!(p->flags & PAGE_WRITE)) {
                if (!page_unprotect(addr, 0, NULL))
                    return -1;
            }
            return 0;
        }
    }
    return 0;
}

/* called from signal handler: invalidate the code and unprotect the
   page. Return TRUE if the fault was succesfully handled. */
int page_unprotect(target_ulong address, unsigned long pc, void *puc)
{
    unsigned int page_index, prot, pindex;
    PageDesc *p, *p1;
    target_ulong host_start, host_end, addr;

    /* Technically this isn't safe inside a signal handler.  However we
       know this only ever happens in a synchronous SEGV handler, so in
       practice it seems to be ok.  */
    mmap_lock();

    host_start = address & qemu_host_page_mask;
    page_index = host_start >> TARGET_PAGE_BITS;
    p1 = page_find(page_index);
    if (!p1) {
        mmap_unlock();
        return 0;
    }
    host_end = host_start + qemu_host_page_size;
    p = p1;
    prot = 0;
    for(addr = host_start;addr < host_end; addr += TARGET_PAGE_SIZE) {
        prot |= p->flags;
        p++;
    }
    /* if the page was really writable, then we change its
       protection back to writable */
    if (prot & PAGE_WRITE_ORG) {
        pindex = (address - host_start) >> TARGET_PAGE_BITS;
        if (!(p1[pindex].flags & PAGE_WRITE)) {
            mprotect((void *)g2h(host_start), qemu_host_page_size,
                     (prot & PAGE_BITS) | PAGE_WRITE);
            p1[pindex].flags |= PAGE_WRITE;
            /* and since the content will be modified, we must invalidate
               the corresponding translated code. */
            tb_invalidate_phys_page(address, pc, puc);
#ifdef DEBUG_TB_CHECK
            tb_invalidate_check(address);
#endif
            mmap_unlock();
            return 1;
        }
    }
    mmap_unlock();
    return 0;
}

static inline void tlb_set_dirty(CPUState *env,
                                 unsigned long addr, target_ulong vaddr)
{
}
#endif /* defined(CONFIG_USER_ONLY) */

#if !defined(CONFIG_USER_ONLY)
static int subpage_register (subpage_t *mmio, uint32_t start, uint32_t end,
                             ram_addr_t memory);
static void *subpage_init (target_phys_addr_t base, ram_addr_t *phys,
                           ram_addr_t orig_memory);
#define CHECK_SUBPAGE(addr, start_addr, start_addr2, end_addr, end_addr2, \
                      need_subpage)                                     \
    do {                                                                \
        if (addr > start_addr)                                          \
            start_addr2 = 0;                                            \
        else {                                                          \
            start_addr2 = start_addr & ~TARGET_PAGE_MASK;               \
            if (start_addr2 > 0)                                        \
                need_subpage = 1;                                       \
        }                                                               \
                                                                        \
        if ((start_addr + orig_size) - addr >= TARGET_PAGE_SIZE)        \
            end_addr2 = TARGET_PAGE_SIZE - 1;                           \
        else {                                                          \
            end_addr2 = (start_addr + orig_size - 1) & ~TARGET_PAGE_MASK; \
            if (end_addr2 < TARGET_PAGE_SIZE - 1)                       \
                need_subpage = 1;                                       \
        }                                                               \
    } while (0)

/* register physical memory. 'size' must be a multiple of the target
   page size. If (phys_offset & ~TARGET_PAGE_MASK) != 0, then it is an
   io memory page */
void cpu_register_physical_memory(target_phys_addr_t start_addr,
                                  ram_addr_t size,
                                  ram_addr_t phys_offset)
{
    target_phys_addr_t addr, end_addr;
    PhysPageDesc *p;
    CPUState *env;
    ram_addr_t orig_size = size;
    void *subpage;

#ifdef USE_KQEMU
    /* XXX: should not depend on cpu context */
    env = first_cpu;
    if (env->kqemu_enabled) {
        kqemu_set_phys_mem(start_addr, size, phys_offset);
    }
#endif
    size = (size + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK;
    end_addr = start_addr + (target_phys_addr_t)size;
    for(addr = start_addr; addr != end_addr; addr += TARGET_PAGE_SIZE) {
        p = phys_page_find(addr >> TARGET_PAGE_BITS);
        if (p && p->phys_offset != IO_MEM_UNASSIGNED) {
            ram_addr_t orig_memory = p->phys_offset;
            target_phys_addr_t start_addr2, end_addr2;
            int need_subpage = 0;

            CHECK_SUBPAGE(addr, start_addr, start_addr2, end_addr, end_addr2,
                          need_subpage);
            if (need_subpage || phys_offset & IO_MEM_SUBWIDTH) {
                if (!(orig_memory & IO_MEM_SUBPAGE)) {
                    subpage = subpage_init((addr & TARGET_PAGE_MASK),
                                           &p->phys_offset, orig_memory);
                } else {
                    subpage = io_mem_opaque[(orig_memory & ~TARGET_PAGE_MASK)
                                            >> IO_MEM_SHIFT];
                }
                subpage_register(subpage, start_addr2, end_addr2, phys_offset);
            } else {
                p->phys_offset = phys_offset;
                if ((phys_offset & ~TARGET_PAGE_MASK) <= IO_MEM_ROM ||
                    (phys_offset & IO_MEM_ROMD))
                    phys_offset += TARGET_PAGE_SIZE;
            }
        } else {
            p = phys_page_find_alloc(addr >> TARGET_PAGE_BITS, 1);
            p->phys_offset = phys_offset;
            if ((phys_offset & ~TARGET_PAGE_MASK) <= IO_MEM_ROM ||
                (phys_offset & IO_MEM_ROMD))
                phys_offset += TARGET_PAGE_SIZE;
            else {
                target_phys_addr_t start_addr2, end_addr2;
                int need_subpage = 0;

                CHECK_SUBPAGE(addr, start_addr, start_addr2, end_addr,
                              end_addr2, need_subpage);

                if (need_subpage || phys_offset & IO_MEM_SUBWIDTH) {
                    subpage = subpage_init((addr & TARGET_PAGE_MASK),
                                           &p->phys_offset, IO_MEM_UNASSIGNED);
                    subpage_register(subpage, start_addr2, end_addr2,
                                     phys_offset);
                }
            }
        }
    }

    /* since each CPU stores ram addresses in its TLB cache, we must
       reset the modified entries */
    /* XXX: slow ! */
    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        tlb_flush(env, 1);
    }
}

/* XXX: temporary until new memory mapping API */
ram_addr_t cpu_get_physical_page_desc(target_phys_addr_t addr)
{
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p)
        return IO_MEM_UNASSIGNED;
    return p->phys_offset;
}

/* XXX: better than nothing */
ram_addr_t qemu_ram_alloc(ram_addr_t size)
{
    ram_addr_t addr;
    if ((phys_ram_alloc_offset + size) > phys_ram_size) {
        fprintf(stderr, "Not enough memory (requested_size = %" PRIu64 ", max memory = %" PRIu64 "\n",
                (uint64_t)size, (uint64_t)phys_ram_size);
        abort();
    }
    addr = phys_ram_alloc_offset;
    phys_ram_alloc_offset = TARGET_PAGE_ALIGN(phys_ram_alloc_offset + size);
    return addr;
}

void qemu_ram_free(ram_addr_t addr)
{
}

static uint32_t unassigned_mem_readb(void *opaque, target_phys_addr_t addr)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem read " TARGET_FMT_plx "\n", addr);
#endif
#ifdef TARGET_SPARC
    do_unassigned_access(addr, 0, 0, 0);
#elif defined(TARGET_CRIS)
    do_unassigned_access(addr, 0, 0, 0);
#endif
    return 0;
}

static void unassigned_mem_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem write " TARGET_FMT_plx " = 0x%x\n", addr, val);
#endif
#ifdef TARGET_SPARC
    do_unassigned_access(addr, 1, 0, 0);
#elif defined(TARGET_CRIS)
    do_unassigned_access(addr, 1, 0, 0);
#endif
}

static CPUReadMemoryFunc *unassigned_mem_read[3] = {
    unassigned_mem_readb,
    unassigned_mem_readb,
    unassigned_mem_readb,
};

static CPUWriteMemoryFunc *unassigned_mem_write[3] = {
    unassigned_mem_writeb,
    unassigned_mem_writeb,
    unassigned_mem_writeb,
};

static void notdirty_mem_writeb(void *opaque, target_phys_addr_t ram_addr,
                                uint32_t val)
{
    int dirty_flags;
    dirty_flags = phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS];
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 1);
        dirty_flags = phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS];
#endif
    }
    stb_p(phys_ram_base + ram_addr, val);
#ifdef USE_KQEMU
    if (cpu_single_env->kqemu_enabled &&
        (dirty_flags & KQEMU_MODIFY_PAGE_MASK) != KQEMU_MODIFY_PAGE_MASK)
        kqemu_modify_page(cpu_single_env, ram_addr);
#endif
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG);
    phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS] = dirty_flags;
    /* we remove the notdirty callback only if the code has been
       flushed */
    if (dirty_flags == 0xff)
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
}

static void notdirty_mem_writew(void *opaque, target_phys_addr_t ram_addr,
                                uint32_t val)
{
    int dirty_flags;
    dirty_flags = phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS];
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 2);
        dirty_flags = phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS];
#endif
    }
    stw_p(phys_ram_base + ram_addr, val);
#ifdef USE_KQEMU
    if (cpu_single_env->kqemu_enabled &&
        (dirty_flags & KQEMU_MODIFY_PAGE_MASK) != KQEMU_MODIFY_PAGE_MASK)
        kqemu_modify_page(cpu_single_env, ram_addr);
#endif
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG);
    phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS] = dirty_flags;
    /* we remove the notdirty callback only if the code has been
       flushed */
    if (dirty_flags == 0xff)
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
}

static void notdirty_mem_writel(void *opaque, target_phys_addr_t ram_addr,
                                uint32_t val)
{
    int dirty_flags;
    dirty_flags = phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS];
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 4);
        dirty_flags = phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS];
#endif
    }
    stl_p(phys_ram_base + ram_addr, val);
#ifdef USE_KQEMU
    if (cpu_single_env->kqemu_enabled &&
        (dirty_flags & KQEMU_MODIFY_PAGE_MASK) != KQEMU_MODIFY_PAGE_MASK)
        kqemu_modify_page(cpu_single_env, ram_addr);
#endif
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG);
    phys_ram_dirty[ram_addr >> TARGET_PAGE_BITS] = dirty_flags;
    /* we remove the notdirty callback only if the code has been
       flushed */
    if (dirty_flags == 0xff)
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
}

static CPUReadMemoryFunc *error_mem_read[3] = {
    NULL, /* never used */
    NULL, /* never used */
    NULL, /* never used */
};

static CPUWriteMemoryFunc *notdirty_mem_write[3] = {
    notdirty_mem_writeb,
    notdirty_mem_writew,
    notdirty_mem_writel,
};

/* Generate a debug exception if a watchpoint has been hit.  */
static void check_watchpoint(int offset, int flags)
{
    CPUState *env = cpu_single_env;
    target_ulong vaddr;
    int i;

    vaddr = (env->mem_io_vaddr & TARGET_PAGE_MASK) + offset;
    for (i = 0; i < env->nb_watchpoints; i++) {
        if (vaddr == env->watchpoint[i].vaddr
                && (env->watchpoint[i].type & flags)) {
            env->watchpoint_hit = i + 1;
            cpu_interrupt(env, CPU_INTERRUPT_DEBUG);
            break;
        }
    }
}

/* Watchpoint access routines.  Watchpoints are inserted using TLB tricks,
   so these check for a hit then pass through to the normal out-of-line
   phys routines.  */
static uint32_t watch_mem_readb(void *opaque, target_phys_addr_t addr)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, PAGE_READ);
    return ldub_phys(addr);
}

static uint32_t watch_mem_readw(void *opaque, target_phys_addr_t addr)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, PAGE_READ);
    return lduw_phys(addr);
}

static uint32_t watch_mem_readl(void *opaque, target_phys_addr_t addr)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, PAGE_READ);
    return ldl_phys(addr);
}

static void watch_mem_writeb(void *opaque, target_phys_addr_t addr,
                             uint32_t val)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, PAGE_WRITE);
    stb_phys(addr, val);
}

static void watch_mem_writew(void *opaque, target_phys_addr_t addr,
                             uint32_t val)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, PAGE_WRITE);
    stw_phys(addr, val);
}

static void watch_mem_writel(void *opaque, target_phys_addr_t addr,
                             uint32_t val)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, PAGE_WRITE);
    stl_phys(addr, val);
}

static CPUReadMemoryFunc *watch_mem_read[3] = {
    watch_mem_readb,
    watch_mem_readw,
    watch_mem_readl,
};

static CPUWriteMemoryFunc *watch_mem_write[3] = {
    watch_mem_writeb,
    watch_mem_writew,
    watch_mem_writel,
};

static inline uint32_t subpage_readlen (subpage_t *mmio, target_phys_addr_t addr,
                                 unsigned int len)
{
    uint32_t ret;
    unsigned int idx;

    idx = SUBPAGE_IDX(addr - mmio->base);
#if defined(DEBUG_SUBPAGE)
    printf("%s: subpage %p len %d addr " TARGET_FMT_plx " idx %d\n", __func__,
           mmio, len, addr, idx);
#endif
    ret = (**mmio->mem_read[idx][len])(mmio->opaque[idx][0][len], addr);

    return ret;
}

static inline void subpage_writelen (subpage_t *mmio, target_phys_addr_t addr,
                              uint32_t value, unsigned int len)
{
    unsigned int idx;

    idx = SUBPAGE_IDX(addr - mmio->base);
#if defined(DEBUG_SUBPAGE)
    printf("%s: subpage %p len %d addr " TARGET_FMT_plx " idx %d value %08x\n", __func__,
           mmio, len, addr, idx, value);
#endif
    (**mmio->mem_write[idx][len])(mmio->opaque[idx][1][len], addr, value);
}

static uint32_t subpage_readb (void *opaque, target_phys_addr_t addr)
{
#if defined(DEBUG_SUBPAGE)
    printf("%s: addr " TARGET_FMT_plx "\n", __func__, addr);
#endif

    return subpage_readlen(opaque, addr, 0);
}

static void subpage_writeb (void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
#if defined(DEBUG_SUBPAGE)
    printf("%s: addr " TARGET_FMT_plx " val %08x\n", __func__, addr, value);
#endif
    subpage_writelen(opaque, addr, value, 0);
}

static uint32_t subpage_readw (void *opaque, target_phys_addr_t addr)
{
#if defined(DEBUG_SUBPAGE)
    printf("%s: addr " TARGET_FMT_plx "\n", __func__, addr);
#endif

    return subpage_readlen(opaque, addr, 1);
}

static void subpage_writew (void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
#if defined(DEBUG_SUBPAGE)
    printf("%s: addr " TARGET_FMT_plx " val %08x\n", __func__, addr, value);
#endif
    subpage_writelen(opaque, addr, value, 1);
}

static uint32_t subpage_readl (void *opaque, target_phys_addr_t addr)
{
#if defined(DEBUG_SUBPAGE)
    printf("%s: addr " TARGET_FMT_plx "\n", __func__, addr);
#endif

    return subpage_readlen(opaque, addr, 2);
}

static void subpage_writel (void *opaque,
                         target_phys_addr_t addr, uint32_t value)
{
#if defined(DEBUG_SUBPAGE)
    printf("%s: addr " TARGET_FMT_plx " val %08x\n", __func__, addr, value);
#endif
    subpage_writelen(opaque, addr, value, 2);
}

static CPUReadMemoryFunc *subpage_read[] = {
    &subpage_readb,
    &subpage_readw,
    &subpage_readl,
};

static CPUWriteMemoryFunc *subpage_write[] = {
    &subpage_writeb,
    &subpage_writew,
    &subpage_writel,
};

static int subpage_register (subpage_t *mmio, uint32_t start, uint32_t end,
                             ram_addr_t memory)
{
    int idx, eidx;
    unsigned int i;

    if (start >= TARGET_PAGE_SIZE || end >= TARGET_PAGE_SIZE)
        return -1;
    idx = SUBPAGE_IDX(start);
    eidx = SUBPAGE_IDX(end);
#if defined(DEBUG_SUBPAGE)
    printf("%s: %p start %08x end %08x idx %08x eidx %08x mem %d\n", __func__,
           mmio, start, end, idx, eidx, memory);
#endif
    memory >>= IO_MEM_SHIFT;
    for (; idx <= eidx; idx++) {
        for (i = 0; i < 4; i++) {
            if (io_mem_read[memory][i]) {
                mmio->mem_read[idx][i] = &io_mem_read[memory][i];
                mmio->opaque[idx][0][i] = io_mem_opaque[memory];
            }
            if (io_mem_write[memory][i]) {
                mmio->mem_write[idx][i] = &io_mem_write[memory][i];
                mmio->opaque[idx][1][i] = io_mem_opaque[memory];
            }
        }
    }

    return 0;
}

static void *subpage_init (target_phys_addr_t base, ram_addr_t *phys,
                           ram_addr_t orig_memory)
{
    subpage_t *mmio;
    int subpage_memory;

    mmio = qemu_mallocz(sizeof(subpage_t));
    if (mmio != NULL) {
        mmio->base = base;
        subpage_memory = cpu_register_io_memory(0, subpage_read, subpage_write, mmio);
#if defined(DEBUG_SUBPAGE)
        printf("%s: %p base " TARGET_FMT_plx " len %08x %d\n", __func__,
               mmio, base, TARGET_PAGE_SIZE, subpage_memory);
#endif
        *phys = subpage_memory | IO_MEM_SUBPAGE;
        subpage_register(mmio, 0, TARGET_PAGE_SIZE - 1, orig_memory);
    }

    return mmio;
}

static void io_mem_init(void)
{
    cpu_register_io_memory(IO_MEM_ROM >> IO_MEM_SHIFT, error_mem_read, unassigned_mem_write, NULL);
    cpu_register_io_memory(IO_MEM_UNASSIGNED >> IO_MEM_SHIFT, unassigned_mem_read, unassigned_mem_write, NULL);
    cpu_register_io_memory(IO_MEM_NOTDIRTY >> IO_MEM_SHIFT, error_mem_read, notdirty_mem_write, NULL);
    io_mem_nb = 5;

    io_mem_watch = cpu_register_io_memory(0, watch_mem_read,
                                          watch_mem_write, NULL);
    /* alloc dirty bits array */
    phys_ram_dirty = qemu_vmalloc(phys_ram_size >> TARGET_PAGE_BITS);
    memset(phys_ram_dirty, 0xff, phys_ram_size >> TARGET_PAGE_BITS);
}

/* mem_read and mem_write are arrays of functions containing the
   function to access byte (index 0), word (index 1) and dword (index
   2). Functions can be omitted with a NULL function pointer. The
   registered functions may be modified dynamically later.
   If io_index is non zero, the corresponding io zone is
   modified. If it is zero, a new io zone is allocated. The return
   value can be used with cpu_register_physical_memory(). (-1) is
   returned if error. */
int cpu_register_io_memory(int io_index,
                           CPUReadMemoryFunc **mem_read,
                           CPUWriteMemoryFunc **mem_write,
                           void *opaque)
{
    int i, subwidth = 0;

    if (io_index <= 0) {
        if (io_mem_nb >= IO_MEM_NB_ENTRIES)
            return -1;
        io_index = io_mem_nb++;
    } else {
        if (io_index >= IO_MEM_NB_ENTRIES)
            return -1;
    }

    for(i = 0;i < 3; i++) {
        if (!mem_read[i] || !mem_write[i])
            subwidth = IO_MEM_SUBWIDTH;
        io_mem_read[io_index][i] = mem_read[i];
        io_mem_write[io_index][i] = mem_write[i];
    }
    io_mem_opaque[io_index] = opaque;
    return (io_index << IO_MEM_SHIFT) | subwidth;
}

CPUWriteMemoryFunc **cpu_get_io_memory_write(int io_index)
{
    return io_mem_write[io_index >> IO_MEM_SHIFT];
}

CPUReadMemoryFunc **cpu_get_io_memory_read(int io_index)
{
    return io_mem_read[io_index >> IO_MEM_SHIFT];
}

#endif /* !defined(CONFIG_USER_ONLY) */

/* physical memory access (slow version, mainly for debug) */
#if defined(CONFIG_USER_ONLY)
void cpu_physical_memory_rw(target_phys_addr_t addr, uint8_t *buf,
                            int len, int is_write)
{
    int l, flags;
    target_ulong page;
    void * p;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        flags = page_get_flags(page);
        if (!(flags & PAGE_VALID))
            return;
        if (is_write) {
            if (!(flags & PAGE_WRITE))
                return;
            /* XXX: this code should not depend on lock_user */
            if (!(p = lock_user(VERIFY_WRITE, addr, l, 0)))
                /* FIXME - should this return an error rather than just fail? */
                return;
            memcpy(p, buf, l);
            unlock_user(p, addr, l);
        } else {
            if (!(flags & PAGE_READ))
                return;
            /* XXX: this code should not depend on lock_user */
            if (!(p = lock_user(VERIFY_READ, addr, l, 1)))
                /* FIXME - should this return an error rather than just fail? */
                return;
            memcpy(buf, p, l);
            unlock_user(p, addr, 0);
        }
        len -= l;
        buf += l;
        addr += l;
    }
}

#else
void cpu_physical_memory_rw(target_phys_addr_t addr, uint8_t *buf,
                            int len, int is_write)
{
    int l, io_index;
    uint8_t *ptr;
    uint32_t val;
    target_phys_addr_t page;
    unsigned long pd;
    PhysPageDesc *p;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        p = phys_page_find(page >> TARGET_PAGE_BITS);
        if (!p) {
            pd = IO_MEM_UNASSIGNED;
        } else {
            pd = p->phys_offset;
        }

        if (is_write) {
            if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
                io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
                /* XXX: could force cpu_single_env to NULL to avoid
                   potential bugs */
                if (l >= 4 && ((addr & 3) == 0)) {
                    /* 32 bit write access */
                    val = ldl_p(buf);
                    io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
                    l = 4;
                } else if (l >= 2 && ((addr & 1) == 0)) {
                    /* 16 bit write access */
                    val = lduw_p(buf);
                    io_mem_write[io_index][1](io_mem_opaque[io_index], addr, val);
                    l = 2;
                } else {
                    /* 8 bit write access */
                    val = ldub_p(buf);
                    io_mem_write[io_index][0](io_mem_opaque[io_index], addr, val);
                    l = 1;
                }
            } else {
                unsigned long addr1;
                addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
                /* RAM case */
                ptr = phys_ram_base + addr1;
                memcpy(ptr, buf, l);
                if (!cpu_physical_memory_is_dirty(addr1)) {
                    /* invalidate code */
                    tb_invalidate_phys_page_range(addr1, addr1 + l, 0);
                    /* set dirty bit */
                    phys_ram_dirty[addr1 >> TARGET_PAGE_BITS] |=
                        (0xff & ~CODE_DIRTY_FLAG);
                }
            }
        } else {
            if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
                !(pd & IO_MEM_ROMD)) {
                /* I/O case */
                io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
                if (l >= 4 && ((addr & 3) == 0)) {
                    /* 32 bit read access */
                    val = io_mem_read[io_index][2](io_mem_opaque[io_index], addr);
                    stl_p(buf, val);
                    l = 4;
                } else if (l >= 2 && ((addr & 1) == 0)) {
                    /* 16 bit read access */
                    val = io_mem_read[io_index][1](io_mem_opaque[io_index], addr);
                    stw_p(buf, val);
                    l = 2;
                } else {
                    /* 8 bit read access */
                    val = io_mem_read[io_index][0](io_mem_opaque[io_index], addr);
                    stb_p(buf, val);
                    l = 1;
                }
            } else {
                /* RAM case */
                ptr = phys_ram_base + (pd & TARGET_PAGE_MASK) +
                    (addr & ~TARGET_PAGE_MASK);
                memcpy(buf, ptr, l);
            }
        }
        len -= l;
        buf += l;
        addr += l;
    }
}

/* used for ROM loading : can write in RAM and ROM */
void cpu_physical_memory_write_rom(target_phys_addr_t addr,
                                   const uint8_t *buf, int len)
{
    int l;
    uint8_t *ptr;
    target_phys_addr_t page;
    unsigned long pd;
    PhysPageDesc *p;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        p = phys_page_find(page >> TARGET_PAGE_BITS);
        if (!p) {
            pd = IO_MEM_UNASSIGNED;
        } else {
            pd = p->phys_offset;
        }

        if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM &&
            (pd & ~TARGET_PAGE_MASK) != IO_MEM_ROM &&
            !(pd & IO_MEM_ROMD)) {
            /* do nothing */
        } else {
            unsigned long addr1;
            addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
            /* ROM/RAM case */
            ptr = phys_ram_base + addr1;
            memcpy(ptr, buf, l);
        }
        len -= l;
        buf += l;
        addr += l;
    }
}


/* warning: addr must be aligned */
uint32_t ldl_phys(target_phys_addr_t addr)
{
    int io_index;
    uint8_t *ptr;
    uint32_t val;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
        !(pd & IO_MEM_ROMD)) {
        /* I/O case */
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        val = io_mem_read[io_index][2](io_mem_opaque[io_index], addr);
    } else {
        /* RAM case */
        ptr = phys_ram_base + (pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        val = ldl_p(ptr);
    }
    return val;
}

/* warning: addr must be aligned */
uint64_t ldq_phys(target_phys_addr_t addr)
{
    int io_index;
    uint8_t *ptr;
    uint64_t val;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
        !(pd & IO_MEM_ROMD)) {
        /* I/O case */
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
#ifdef TARGET_WORDS_BIGENDIAN
        val = (uint64_t)io_mem_read[io_index][2](io_mem_opaque[io_index], addr) << 32;
        val |= io_mem_read[io_index][2](io_mem_opaque[io_index], addr + 4);
#else
        val = io_mem_read[io_index][2](io_mem_opaque[io_index], addr);
        val |= (uint64_t)io_mem_read[io_index][2](io_mem_opaque[io_index], addr + 4) << 32;
#endif
    } else {
        /* RAM case */
        ptr = phys_ram_base + (pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        val = ldq_p(ptr);
    }
    return val;
}

/* XXX: optimize */
uint32_t ldub_phys(target_phys_addr_t addr)
{
    uint8_t val;
    cpu_physical_memory_read(addr, &val, 1);
    return val;
}

/* XXX: optimize */
uint32_t lduw_phys(target_phys_addr_t addr)
{
    uint16_t val;
    cpu_physical_memory_read(addr, (uint8_t *)&val, 2);
    return tswap16(val);
}

/* warning: addr must be aligned. The ram page is not masked as dirty
   and the code inside is not invalidated. It is useful if the dirty
   bits are used to track modified PTEs */
void stl_phys_notdirty(target_phys_addr_t addr, uint32_t val)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
    } else {
        ptr = phys_ram_base + (pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        stl_p(ptr, val);
    }
}

void stq_phys_notdirty(target_phys_addr_t addr, uint64_t val)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
#ifdef TARGET_WORDS_BIGENDIAN
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val >> 32);
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr + 4, val);
#else
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr + 4, val >> 32);
#endif
    } else {
        ptr = phys_ram_base + (pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        stq_p(ptr, val);
    }
}

/* warning: addr must be aligned */
void stl_phys(target_phys_addr_t addr, uint32_t val)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
    } else {
        unsigned long addr1;
        addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
        /* RAM case */
        ptr = phys_ram_base + addr1;
        stl_p(ptr, val);
        if (!cpu_physical_memory_is_dirty(addr1)) {
            /* invalidate code */
            tb_invalidate_phys_page_range(addr1, addr1 + 4, 0);
            /* set dirty bit */
            phys_ram_dirty[addr1 >> TARGET_PAGE_BITS] |=
                (0xff & ~CODE_DIRTY_FLAG);
        }
    }
}

/* XXX: optimize */
void stb_phys(target_phys_addr_t addr, uint32_t val)
{
    uint8_t v = val;
    cpu_physical_memory_write(addr, &v, 1);
}

/* XXX: optimize */
void stw_phys(target_phys_addr_t addr, uint32_t val)
{
    uint16_t v = tswap16(val);
    cpu_physical_memory_write(addr, (const uint8_t *)&v, 2);
}

/* XXX: optimize */
void stq_phys(target_phys_addr_t addr, uint64_t val)
{
    val = tswap64(val);
    cpu_physical_memory_write(addr, (const uint8_t *)&val, 8);
}

#endif

/* virtual memory access for debug */
int cpu_memory_rw_debug(CPUState *env, target_ulong addr,
                        uint8_t *buf, int len, int is_write)
{
    int l;
    target_phys_addr_t phys_addr;
    target_ulong page;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        phys_addr = cpu_get_phys_page_debug(env, page);
        /* if no physical page mapped, return an error */
        if (phys_addr == -1)
            return -1;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        cpu_physical_memory_rw(phys_addr + (addr & ~TARGET_PAGE_MASK),
                               buf, l, is_write);
        len -= l;
        buf += l;
        addr += l;
    }
    return 0;
}

/* in deterministic execution mode, instructions doing device I/Os
   must be at the end of the TB */
void cpu_io_recompile(CPUState *env, void *retaddr)
{
    TranslationBlock *tb;
    uint32_t n, cflags;
    target_ulong pc, cs_base;
    uint64_t flags;

    tb = tb_find_pc((unsigned long)retaddr);
    if (!tb) {
        cpu_abort(env, "cpu_io_recompile: could not find TB for pc=%p", 
                  retaddr);
    }
    n = env->icount_decr.u16.low + tb->icount;
    cpu_restore_state(tb, env, (unsigned long)retaddr, NULL);
    /* Calculate how many instructions had been executed before the fault
       occurred.  */
    n = n - env->icount_decr.u16.low;
    /* Generate a new TB ending on the I/O insn.  */
    n++;
    /* On MIPS and SH, delay slot instructions can only be restarted if
       they were already the first instruction in the TB.  If this is not
       the first instruction in a TB then re-execute the preceding
       branch.  */
#if defined(TARGET_MIPS)
    if ((env->hflags & MIPS_HFLAG_BMASK) != 0 && n > 1) {
        env->active_tc.PC -= 4;
        env->icount_decr.u16.low++;
        env->hflags &= ~MIPS_HFLAG_BMASK;
    }
#elif defined(TARGET_SH4)
    if ((env->flags & ((DELAY_SLOT | DELAY_SLOT_CONDITIONAL))) != 0
            && n > 1) {
        env->pc -= 2;
        env->icount_decr.u16.low++;
        env->flags &= ~(DELAY_SLOT | DELAY_SLOT_CONDITIONAL);
    }
#endif
    /* This should never happen.  */
    if (n > CF_COUNT_MASK)
        cpu_abort(env, "TB too big during recompile");

    cflags = n | CF_LAST_IO;
    pc = tb->pc;
    cs_base = tb->cs_base;
    flags = tb->flags;
    tb_phys_invalidate(tb, -1);
    /* FIXME: In theory this could raise an exception.  In practice
       we have already translated the block once so it's probably ok.  */
    tb_gen_code(env, pc, cs_base, flags, cflags);
    /* TODO: If env->pc != tb->pc (i.e. the faulting instruction was not
       the first in the TB) then we end up generating a whole new TB and
       repeating the fault, which is horribly inefficient.
       Better would be to execute just this insn uncached, or generate a
       second new TB.  */
    cpu_resume_from_signal(env, NULL);
}

void dump_exec_info(FILE *f,
                    int (*cpu_fprintf)(FILE *f, const char *fmt, ...))
{
    int i, target_code_size, max_target_code_size;
    int direct_jmp_count, direct_jmp2_count, cross_page;
    TranslationBlock *tb;

    target_code_size = 0;
    max_target_code_size = 0;
    cross_page = 0;
    direct_jmp_count = 0;
    direct_jmp2_count = 0;
    for(i = 0; i < nb_tbs; i++) {
        tb = &tbs[i];
        target_code_size += tb->size;
        if (tb->size > max_target_code_size)
            max_target_code_size = tb->size;
        if (tb->page_addr[1] != -1)
            cross_page++;
        if (tb->tb_next_offset[0] != 0xffff) {
            direct_jmp_count++;
            if (tb->tb_next_offset[1] != 0xffff) {
                direct_jmp2_count++;
            }
        }
    }
    /* XXX: avoid using doubles ? */
    cpu_fprintf(f, "Translation buffer state:\n");
    cpu_fprintf(f, "gen code size       %ld/%ld\n",
                code_gen_ptr - code_gen_buffer, code_gen_buffer_max_size);
    cpu_fprintf(f, "TB count            %d/%d\n", 
                nb_tbs, code_gen_max_blocks);
    cpu_fprintf(f, "TB avg target size  %d max=%d bytes\n",
                nb_tbs ? target_code_size / nb_tbs : 0,
                max_target_code_size);
    cpu_fprintf(f, "TB avg host size    %d bytes (expansion ratio: %0.1f)\n",
                nb_tbs ? (code_gen_ptr - code_gen_buffer) / nb_tbs : 0,
                target_code_size ? (double) (code_gen_ptr - code_gen_buffer) / target_code_size : 0);
    cpu_fprintf(f, "cross page TB count %d (%d%%)\n",
            cross_page,
            nb_tbs ? (cross_page * 100) / nb_tbs : 0);
    cpu_fprintf(f, "direct jump count   %d (%d%%) (2 jumps=%d %d%%)\n",
                direct_jmp_count,
                nb_tbs ? (direct_jmp_count * 100) / nb_tbs : 0,
                direct_jmp2_count,
                nb_tbs ? (direct_jmp2_count * 100) / nb_tbs : 0);
    cpu_fprintf(f, "\nStatistics:\n");
    cpu_fprintf(f, "TB flush count      %d\n", tb_flush_count);
    cpu_fprintf(f, "TB invalidate count %d\n", tb_phys_invalidate_count);
    cpu_fprintf(f, "TLB flush count     %d\n", tlb_flush_count);
    tcg_dump_info(f, cpu_fprintf);
}

#if !defined(CONFIG_USER_ONLY)

#define MMUSUFFIX _cmmu
#define GETPC() NULL
#define env cpu_single_env
#define SOFTMMU_CODE_ACCESS

#define SHIFT 0
#include "softmmu_template.h"

#define SHIFT 1
#include "softmmu_template.h"

#define SHIFT 2
#include "softmmu_template.h"

#define SHIFT 3
#include "softmmu_template.h"

#undef env

#endif