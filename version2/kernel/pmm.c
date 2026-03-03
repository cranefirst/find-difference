/* kernel/pmm.c */
#include "pmm.h"
#include "util/functions.h"
#include "riscv.h"
#include "config.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"
#include "kernel/sync_utils.h"

// 引用 kernel.c 中的全局打印锁
extern spinlock_t htif_lock;

extern char _end[];
extern uint64 g_mem_size;

static uint64 free_mem_start_addr;
static uint64 free_mem_end_addr;

int vm_alloc_stage[NCPU] = { 0 };

typedef struct node {
  struct node *next;
} list_node;

static list_node g_free_mem_list;
static spinlock_t pmm_lock = {0};

static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE)
    free_page( (void *)p );
}

void free_page(void *pa) {
  // [关键修复] 只有 pmm_init 初始化了边界变量，这里才不会 Panic
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr || (uint64)pa >= free_mem_end_addr)
    panic("free_page 0x%lx \n", pa);

  spinlock_lock(&pmm_lock);
  list_node *n = (list_node *)pa;
  n->next = g_free_mem_list.next;
  g_free_mem_list.next = n;
  spinlock_unlock(&pmm_lock);
}

void *alloc_page(void) {
  spinlock_lock(&pmm_lock);
  list_node *n = g_free_mem_list.next;
  if (n) g_free_mem_list.next = n->next;
  spinlock_unlock(&pmm_lock);

  uint64 hartid = read_tp();
  if (vm_alloc_stage[hartid]) {
    // [关键修复] 打印必须加锁
    spinlock_lock(&htif_lock);
    sprint("hartid = %ld: alloc page 0x%x\n", hartid, (uint64)n);
    spinlock_unlock(&htif_lock);
  }
  return (void *)n;
}

void pmm_init() {
  // 计算空闲内存起点（跳过内核代码和多核启动栈）
  uint64 free_mem_start = (uint64)_end + KERNEL_STACK_SIZE * NCPU;
  uint64 pke_kernel_size = (uint64)_end - KERN_BASE;
  
  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if( g_mem_size < pke_kernel_size )
    panic( "Error when recomputing physical memory size (g_mem_size).\n" );

  uint64 free_mem_end = DRAM_BASE + g_mem_size;

  // [核心修复] 必须给全局变量赋值！！！否则 free_page 会崩溃
  free_mem_start_addr = free_mem_start;
  free_mem_end_addr = free_mem_end;

  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: 0x%lx .\n",
    KERN_BASE, (uint64)_end, pke_kernel_size);
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start,
    free_mem_end - 1);

  sprint("kernel memory manager is initializing ...\n");
  create_freepage_list(free_mem_start, free_mem_end);
}