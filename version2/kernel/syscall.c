/* kernel/syscall.c */
#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "spike_interface/spike_utils.h"
#include "kernel/config.h"
#include "kernel/sync_utils.h"

volatile int exit_count = 0;
spinlock_t exit_lock = {0};
extern spinlock_t htif_lock; // 引用 kernel.c 的锁

void atomic_inc_exit_count() {
    spinlock_lock(&exit_lock);
    exit_count++;
    spinlock_unlock(&exit_lock);
}

ssize_t sys_user_print(const char* buf, size_t n) {
  assert(current);
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  
  // [关键] 加锁打印
  spinlock_lock(&htif_lock);
  sprint(pa);
  spinlock_unlock(&htif_lock);
  return 0;
}

ssize_t sys_user_exit(uint64 code) {
  uint64 id = read_tp();
  
  spinlock_lock(&htif_lock);
  sprint("hartid = %ld: User exit with code: %ld.\n", id, code);
  spinlock_unlock(&htif_lock);

  atomic_inc_exit_count();

  if (id == 0) {
      while (exit_count < NCPU) ; 
      
      spinlock_lock(&htif_lock);
      sprint("hartid = 0: shutdown with code: %ld.\n", code);
      spinlock_unlock(&htif_lock);
      
      shutdown(code);
  } else {
      while(1);
  }
  return 0;
}

uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va = current->user_heap_vaddr;
  current->user_heap_vaddr += PGSIZE;
  
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));
  
  spinlock_lock(&htif_lock);
  sprint("hartid = %ld: vaddr 0x%08x is mapped to paddr 0x%x\n", read_tp(), va, (uint64)pa);
  spinlock_unlock(&htif_lock);
  
  return va;
}

uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  return 0;
}

long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
  return 0;
}