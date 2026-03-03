/* kernel/kernel.c */
#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"
#include "kernel/sync_utils.h"

// [新增] 全局 HTIF 锁
spinlock_t htif_lock = {0};
volatile int s_init_lock = 0;

extern char trap_sec_start[];

// [修复] 补充定义
void enable_paging() {
  write_csr(satp, MAKE_SATP(g_kernel_pagetable));
  flush_tlb();
}

extern process* alloc_process();
void load_bincode_from_host_elf(process *p);

void load_user_program(process *proc) {
  // [关键] 加锁保护文件读取和打印
  spinlock_lock(&htif_lock);
  
  uint64 id = read_tp();
  sprint("hartid = %ld: User application is loading.\n", id);

  // 分配物理页
  proc->trapframe = (trapframe *)alloc_page();
  memset(proc->trapframe, 0, sizeof(trapframe));

  proc->pagetable = (pagetable_t)alloc_page();
  memset((void *)proc->pagetable, 0, PGSIZE);

  proc->kstack = (uint64)alloc_page() + PGSIZE;
  uint64 user_stack = (uint64)alloc_page();

  // [关键] 设置虚拟地址 SP
  proc->trapframe->regs.sp = USER_STACK_TOP;
  proc->trapframe->regs.tp = id; 
  
  // 初始化堆起点 (虚拟地址)
  proc->user_heap_vaddr = USER_FREE_ADDRESS_START;

  sprint("hartid = %ld: user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n", id, proc->trapframe,
         proc->trapframe->regs.sp, proc->kstack);

  load_bincode_from_host_elf(proc);

  // 建立映射
  user_vm_map((pagetable_t)proc->pagetable, USER_STACK_TOP - PGSIZE, PGSIZE, user_stack,
         prot_to_type(PROT_WRITE | PROT_READ, 1));
  user_vm_map((pagetable_t)proc->pagetable, (uint64)proc->trapframe, PGSIZE, (uint64)proc->trapframe,
         prot_to_type(PROT_WRITE | PROT_READ, 0));
  user_vm_map((pagetable_t)proc->pagetable, (uint64)trap_sec_start, PGSIZE, (uint64)trap_sec_start,
         prot_to_type(PROT_READ | PROT_EXEC, 0));

  spinlock_unlock(&htif_lock);
  
  asm volatile("fence.i");
}

int s_start(void) {
  uint64 id = read_tp();
  
  // 保护打印
  spinlock_lock(&htif_lock);
  sprint("hartid = %ld: Enter supervisor mode...\n", id);
  spinlock_unlock(&htif_lock);
  
  write_csr(satp, 0);

  if (id == 0) {
      pmm_init();
      kern_vm_init();
      
      spinlock_lock(&htif_lock);
      sprint("kernel page table is on \n");
      spinlock_unlock(&htif_lock);
      
      s_init_lock = 1;
  } else {
      while (s_init_lock == 0);
  }

  enable_paging();

  current = alloc_process();
  load_user_program(current);

  spinlock_lock(&htif_lock);
  sprint("hartid = %ld: Switch to user mode...\n", id);
  spinlock_unlock(&htif_lock);
  
  switch_to(current);

  return 0;
}