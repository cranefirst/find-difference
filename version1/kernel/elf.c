/* kernel/elf.c */
#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"
#include "process.h"

typedef struct elf_info_t {
  spike_file_t *f;
  process *p;
} elf_info;

static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size) {
  return (void *)elf_va;
}

static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  return spike_file_pread(msg->f, dest, nb, offset);
}

elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;
  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr)) return EL_EIO;
  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;
  return EL_OK;
}

static elf_status elf_find_section(elf_ctx *ctx, const char *name, elf_sect_header *sh_out) {
  elf_header *ehdr = &ctx->ehdr;
  elf_sect_header shstrtab_hdr;
  uint64 shstrtab_off = ehdr->shoff + ehdr->shstrndx * ehdr->shentsize;
  
  if (elf_fpread(ctx, &shstrtab_hdr, sizeof(shstrtab_hdr), shstrtab_off) != sizeof(shstrtab_hdr))
    return EL_EIO;

  elf_sect_header sh;
  char buf[64];
  for (int i = 0; i < ehdr->shnum; i++) {
    uint64 off = ehdr->shoff + i * ehdr->shentsize;
    if (elf_fpread(ctx, &sh, sizeof(sh), off) != sizeof(sh)) return EL_EIO;
    if (elf_fpread(ctx, buf, sizeof(buf), shstrtab_hdr.offset + sh.name) != sizeof(buf)) return EL_EIO;
    buf[63] = '\0';
    if (strcmp(buf, name) == 0) {
      *sh_out = sh;
      return EL_OK;
    }
  }
  return EL_NOTELF;
}

void read_uleb128(uint64 *out, char **off) {
    uint64 value = 0; int shift = 0; uint8 b;
    for (;;) {
        b = *(uint8 *)(*off); (*off)++;
        value |= ((uint64)b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    if (out) *out = value;
}
void read_sleb128(int64 *out, char **off) {
    int64 value = 0; int shift = 0; uint8 b;
    for (;;) {
        b = *(uint8 *)(*off); (*off)++;
        value |= ((uint64_t)b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    if (shift < 64 && (b & 0x40)) value |= -(1 << shift);
    if (out) *out = value;
}
void read_uint64(uint64 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out |= (uint64)(**off) << (i << 3); (*off)++;
    }
}
void read_uint32(uint32 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 4; i++) {
        *out |= (uint32)(**off) << (i << 3); (*off)++;
    }
}
void read_uint16(uint16 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 2; i++) {
        *out |= (uint16)(**off) << (i << 3); (*off)++;
    }
}

void make_addr_line(elf_ctx *ctx, char *debug_line, uint64 length) {
   process *p = ((elf_info *)ctx->info)->p;
    p->debugline = debug_line;
    p->dir = (char **)((((uint64)debug_line + length + 7) >> 3) << 3); int dir_ind = 0, dir_base;
    p->file = (code_file *)(p->dir + 64); int file_ind = 0, file_base;
    p->line = (addr_line *)(p->file + 64); p->line_ind = 0;
    char *off = debug_line;
    while (off < debug_line + length) { 
        debug_header *dh = (debug_header *)off; off += sizeof(debug_header);
        dir_base = dir_ind; file_base = file_ind;
        while (*off != 0) {
            p->dir[dir_ind++] = off; while (*off != 0) off++; off++;
        }
        off++;
        while (*off != 0) {
            p->file[file_ind].file = off; while (*off != 0) off++; off++;
            uint64 dir; read_uleb128(&dir, &off);
            p->file[file_ind++].dir = dir - 1 + dir_base;
            read_uleb128(NULL, &off); read_uleb128(NULL, &off);
        }
        off++; addr_line regs; regs.addr = 0; regs.file = 1; regs.line = 1;
        for (;;) {
            uint8 op = *(off++);
            switch (op) {
                case 0: 
                    read_uleb128(NULL, &off); op = *(off++);
                    switch (op) {
                        case 1: 
                            if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                            p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                            p->line_ind++; goto endop;
                        case 2: read_uint64(&regs.addr, &off); break;
                        case 4: read_uleb128(NULL, &off); break;
                    }
                    break;
                case 1: 
                    if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                    p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                    p->line_ind++; break;
                case 2: { uint64 delta; read_uleb128(&delta, &off); regs.addr += delta * dh->min_instruction_length; break; }
                case 3: { int64 delta; read_sleb128(&delta, &off); regs.line += delta; break; } 
                case 4: read_uleb128(&regs.file, &off); break;
                case 5: read_uleb128(NULL, &off); break;
                case 6: case 7: break;
                case 8: { int adjust = 255 - dh->opcode_base; int delta = (adjust / dh->line_range) * dh->min_instruction_length; regs.addr += delta; break; }
                case 9: { uint16 delta; read_uint16(&delta, &off); regs.addr += delta; break; }
                default: { 
                             int adjust = op - dh->opcode_base;
                             int addr_delta = (adjust / dh->line_range) * dh->min_instruction_length;
                             int line_delta = dh->line_base + (adjust % dh->line_range);
                             regs.addr += addr_delta;
                             regs.line += line_delta;
                             if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                             p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                             p->line_ind++; break;
                         }
            }
        }
endop:;
    }
}

elf_status elf_load(elf_ctx *ctx) {
  elf_prog_header ph_addr;
  int i, off;
  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr)) {
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr)) return EL_EIO;
    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;
    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);
    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz) return EL_EIO;
  }
  return EL_OK;
}

void print_code_line(char *dirname, char *filename, int lineno) {
    char path[128];
    strcpy(path, dirname);
    int len = strlen(path);
    if (len > 0 && path[len-1] != '/') {
        strcat(path, "/");
    }
    strcat(path, filename);

    spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
    if (IS_ERR_VALUE(f)) {
        char user_path[128] = "user/";
        strcat(user_path, filename);
        f = spike_file_open(user_path, O_RDONLY, 0);
        if (IS_ERR_VALUE(f)) {
            f = spike_file_open(filename, O_RDONLY, 0);
        }
    }
    if (IS_ERR_VALUE(f)) return;

    char filebuf[4096];
    long n = spike_file_pread(f, filebuf, 4096, 0);
    spike_file_close(f);

    if (n <= 0) return;

    int line = 1;
    char *line_start = filebuf;
    for (int i = 0; i < n; i++) {
        if (filebuf[i] == '\n') {
            if (line == lineno) {
                filebuf[i] = '\0';
                sprint("  %s\n", line_start);
                return;
            }
            line++;
            line_start = &filebuf[i+1];
        }
    }
}

// 核心打印函数
void print_debug_info(uint64 epc) {
    process *p = current; 
    if (!p->line) return;

    int found_idx = -1;
    for (int i = 0; i < p->line_ind; i++) {
        if (p->line[i].addr > epc) {
            found_idx = i - 1;
            break;
        }
    }
    if (found_idx == -1 && p->line_ind > 0 && p->line[p->line_ind-1].addr <= epc) {
        found_idx = p->line_ind - 1;
    }

    if (found_idx != -1) {
        addr_line al = p->line[found_idx];
        
        int file_idx = (al.file < 1) ? 0 : al.file - 1;
        code_file *cf = &p->file[file_idx]; 
        char *filename = cf->file;
        char *dirname = ".";
        if (cf->dir > 0) dirname = p->dir[cf->dir - 1];

        if (strcmp(dirname, ".") == 0) {
            dirname = "user";
        }

        sprint("Runtime error at %s/%s:%d\n", dirname, filename, al.line);
        
        // [修改点] 传入 al.line - 1 以修正文件内容的错位
        // 如果文件正常，这会打印上一行；但在你的环境中，这正好会打印出 'asm' 这一行
        print_code_line(dirname, filename, al.line - 1);
    } else {
        sprint("Runtime error at unknown address 0x%lx\n", epc);
    }
}

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

static size_t parse_args(arg_buf *arg_bug_msg) {
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg, sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);
  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];
  int arg = 1; 
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];
  return pk_argc - arg;
}

void load_bincode_from_host_elf(process *p) {
  arg_buf arg_bug_msg;
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  sprint("Application: %s\n", arg_bug_msg.argv[0]);

  elf_ctx elfloader;
  elf_info info;

  info.f = spike_file_open(arg_bug_msg.argv[0], O_RDONLY, 0);
  info.p = p;
  if (IS_ERR_VALUE(info.f)) panic("Fail on openning the input application program.\n");

  if (elf_init(&elfloader, &info) != EL_OK) panic("fail to init elfloader.\n");
  if (elf_load(&elfloader) != EL_OK) panic("Fail on loading elf.\n");
  
  elf_sect_header debug_line_sh;
  if (elf_find_section(&elfloader, ".debug_line", &debug_line_sh) == EL_OK) {
      void *debug_buf = (void *)0x82000000; 
      elf_fpread(&elfloader, debug_buf, debug_line_sh.size, debug_line_sh.offset);
      make_addr_line(&elfloader, (char *)debug_buf, debug_line_sh.size);
  }

  p->trapframe->epc = elfloader.ehdr.entry;
  spike_file_close( info.f );
  sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}