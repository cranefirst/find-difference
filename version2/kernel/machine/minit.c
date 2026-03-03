/* kernel/machine/minit.c */
#include "util/types.h"
#include "kernel/riscv.h"
#include "kernel/config.h"
#include "spike_interface/spike_utils.h"

volatile int hart_init_lock = 0;
__attribute__((aligned(16))) char stack0[4096 * NCPU];

extern void s_start();
extern void mtrapvec();
extern uint64 htif;
extern uint64 g_mem_size;
riscv_regs g_itrframe;

void init_dtb(uint64 dtb) {
  query_htif(dtb);
  if (htif) sprint("HTIF is available!\r\n");
  query_mem(dtb);
  sprint("(Emulated) memory size: %ld MB\n", g_mem_size >> 20);
}

static void delegate_traps() {
  if (!supports_extension('S')) {
    sprint("S mode is not supported.\n");
    return;
  }
  uintptr_t interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
  uintptr_t exceptions = (1U << CAUSE_MISALIGNED_FETCH) | (1U << CAUSE_FETCH_PAGE_FAULT) |
                         (1U << CAUSE_BREAKPOINT) | (1U << CAUSE_LOAD_PAGE_FAULT) |
                         (1U << CAUSE_STORE_PAGE_FAULT) | (1U << CAUSE_USER_ECALL);
  write_csr(mideleg, interrupts);
  write_csr(medeleg, exceptions);
}

void timerinit(uintptr_t hartid) {
  *(uint64*)CLINT_MTIMECMP(hartid) = *(uint64*)CLINT_MTIME + TIMER_INTERVAL;
  write_csr(mie, read_csr(mie) | MIE_MTIE);
}

void m_start(uintptr_t hartid, uintptr_t dtb) {
  if (hartid == 0) {
      spike_file_init();
      init_dtb(dtb);
      hart_init_lock = 1;
  } else {
      while (hart_init_lock == 0) ;
  }

  sprint("In m_start, hartid:%ld\n", hartid);

  write_csr(mscratch, &g_itrframe);
  write_csr(mstatus, ((read_csr(mstatus) & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_S));
  write_csr(mepc, (uint64)s_start);
  write_csr(mtvec, (uint64)mtrapvec);
  write_csr(mstatus, read_csr(mstatus) | MSTATUS_MIE);
  delegate_traps();
  write_csr(sie, read_csr(sie) | SIE_SEIE | SIE_STIE | SIE_SSIE);
  timerinit(hartid);

  asm volatile("mret");
}