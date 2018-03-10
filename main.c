#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

static const size_t k_addr_space_size = 0x10000;
static const size_t k_guard_size = 4096;
static const size_t k_os_rom_offset = 0xc000;
static const size_t k_os_rom_len = 0x4000;
static const int k_jit_bytes_per_byte = 64;
static const size_t k_vector_reset = 0xfffc;

static void
jit_init(char* p_mem, size_t jit_stride, size_t num_bytes) {
  char* p_jit = p_mem + k_addr_space_size + k_guard_size;
  // nop
  memset(p_jit, '\x90', num_bytes * jit_stride);
  while (num_bytes--) {
    // ud2
    p_jit[0] = 0x0f;
    p_jit[1] = 0x0b;
    p_jit += jit_stride;
  }
}

static size_t jit_emit_int(char* p_jit, size_t index, ssize_t offset) {
  p_jit[index++] = offset & 0xff;
  offset >>= 8;
  p_jit[index++] = offset & 0xff;
  offset >>= 8;
  p_jit[index++] = offset & 0xff;
  offset >>= 8;
  p_jit[index++] = offset & 0xff;

  return index;
}

static size_t jit_emit_do_jmp_next(char* p_jit,
                                   size_t jit_stride,
                                   size_t index,
                                   size_t oplen) {
  assert(index + 2 <= jit_stride);
  size_t offset = (jit_stride * oplen) - (index + 2);
  if (offset <= 0x7f) {
    // jmp
    p_jit[index++] = 0xeb;
    p_jit[index++] = offset;
  } else {
    offset -= 3;
    p_jit[index++] = 0xe9;
    index = jit_emit_int(p_jit, index, offset);
  }

  return index;
}

static size_t jit_emit_do_relative_jump(char* p_jit,
                                        size_t jit_stride,
                                        size_t index,
                                        unsigned char intel_opcode,
                                        unsigned char unsigned_jump_size) {
  char jump_size = (char) unsigned_jump_size;
  ssize_t offset = (jit_stride * (jump_size + 2)) - (index + 2);
  if (offset <= 0x7f && offset >= -0x80) {
    // Fits in a 1-byte offset.
    assert(index + 2 <= jit_stride);
    p_jit[index++] = intel_opcode;
    p_jit[index++] = (unsigned char) offset;
  } else {
    unsigned int uint_offset = (unsigned int) offset;
    offset -= 4;
    assert(index + 6 <= jit_stride);
    p_jit[index++] = 0x0f;
    p_jit[index++] = intel_opcode + 0x10;
    index = jit_emit_int(p_jit, index, offset);
  }

  return index;
}

static size_t jit_emit_do_zn_flags(char* p_jit, size_t index, int reg) {
  assert(index + 8 <= k_jit_bytes_per_byte);
  if (reg == -1) {
    // Nothing -- flags already set.
  } else if (reg == 0) {
    // test al, al
    p_jit[index++] = 0x84;
    p_jit[index++] = 0xc0;
  } else if (reg == 1) {
    // test bl, bl
    p_jit[index++] = 0x84;
    p_jit[index++] = 0xdb;
  } else if (reg == 2) {
    // test bh, bh
    p_jit[index++] = 0x84;
    p_jit[index++] = 0xff;
  }
  // sete dl
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x94;
  p_jit[index++] = 0xc2;
  // sets dh
  p_jit[index++] = 0x0f;
  p_jit[index++] = 0x98;
  p_jit[index++] = 0xc6;

  return index;
}

static void
jit_jit(char* p_mem,
        size_t jit_stride,
        size_t jit_offset,
        size_t jit_len) {
  char* p_jit = p_mem + k_addr_space_size + k_guard_size;
  size_t jit_end = jit_offset + jit_len;
  p_mem += jit_offset;
  p_jit += (jit_offset * jit_stride);
  while (jit_offset < jit_end) {
    unsigned char opcode = p_mem[0];
    unsigned char operand1 = 0;
    unsigned char operand2 = 0;
    size_t index = 0;
    if (jit_offset + 1 < jit_end) {
      operand1 = p_mem[1];
    }
    if (jit_offset + 2 < jit_end) {
      operand2 = p_mem[2];
    }
    switch (opcode) {
    case 0x08:
      // PHP
      // mov rsi, r8
      p_jit[index++] = 0x4c;
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xc6;
      // mov r9, rax
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xc1;
      // shr r9, 8
      p_jit[index++] = 0x49;
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xe9;
      p_jit[index++] = 0x08;
      // or rsi, r9
      p_jit[index++] = 0x4c;
      p_jit[index++] = 0x09;
      p_jit[index++] = 0xce;
      // mov r9, rdx
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xd1;
      // and r9, 1
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x83;
      p_jit[index++] = 0xe1;
      p_jit[index++] = 0x01;
      // shl r9, 1
      p_jit[index++] = 0x49;
      p_jit[index++] = 0xd1;
      p_jit[index++] = 0xe1;
      // or rsi, r9
      p_jit[index++] = 0x4c;
      p_jit[index++] = 0x09;
      p_jit[index++] = 0xce;
      // mov r9, rdx
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xd1;
      // shr r9, 8
      p_jit[index++] = 0x49;
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xe9;
      p_jit[index++] = 0x08;
      // shl r9, 7
      p_jit[index++] = 0x49;
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xe1;
      p_jit[index++] = 0x07;
      // or rsi, r9
      p_jit[index++] = 0x4c;
      p_jit[index++] = 0x09;
      p_jit[index++] = 0xce;

      // mov [rdi + rcx], sil
      p_jit[index++] = 0x40;
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x34;
      p_jit[index++] = 0x0f;
      // dec cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc9;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x09:
      // ORA #imm
      // or al, op1
      p_jit[index++] = 0x0c;
      p_jit[index++] = operand1;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x0a:
      // ASL A
      // shl al, 1
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0xe0;
      // setb ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x10:
      // BPL
      // test dh, dh
      p_jit[index++] = 0x84;
      p_jit[index++] = 0xf6;
      // je
      index = jit_emit_do_relative_jump(p_jit,
                                        jit_stride,
                                        index,
                                        0x74,
                                        operand1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x24:
      // BIT zp
      // mov dl, [rdi + op1]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x97;
      index = jit_emit_int(p_jit, index, operand1);
      // bt edx, 7
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xe2;
      p_jit[index++] = 0x07;
      // setb dh
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc6;
      // mov esi, edx
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xd6;
      // and esi, 0x40
      p_jit[index++] = 0x83;
      p_jit[index++] = 0xe6;
      p_jit[index++] = 0x40;
      // and r8b, 0xbf
      p_jit[index++] = 0x41;
      p_jit[index++] = 0x80;
      p_jit[index++] = 0xe0;
      p_jit[index++] = 0xbf;
      // or r8, rsi
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x09;
      p_jit[index++] = 0xf0;
      // and dl, al
      p_jit[index++] = 0x20;
      p_jit[index++] = 0xc2;
      // sete dl
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x94;
      p_jit[index++] = 0xc2;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x26:
      // ROL zp
      // shr ah, 1 (load carry flag to eflags)
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0xec;
      // rcl [rdi + op1], 1
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0x97;
      index = jit_emit_int(p_jit, index, operand1);
      // setb ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x28:
      // PLP
      // inc cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc1;
      // mov r8b, [rdi + rcx]
      p_jit[index++] = 0x44;
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x0f;

      // bt r8, 0
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xe0;
      p_jit[index++] = 0x00;
      // setb ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc4;
      // bt r8, 1
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xe0;
      p_jit[index++] = 0x01;
      // setb dl
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc2;
      // bt r8, 7
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xe0;
      p_jit[index++] = 0x07;
      // setb dh
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc6;
      // and r8b, 0x7c
      p_jit[index++] = 0x41;
      p_jit[index++] = 0x80;
      p_jit[index++] = 0xe0;
      p_jit[index++] = 0x7c;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x29:
      // AND #imm
      // and al, op1
      p_jit[index++] = 0x24;
      p_jit[index++] = operand1;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x38:
      // SEC
      // mov ah, 1
      p_jit[index++] = 0xb4;
      p_jit[index++] = 0x01;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x66:
      // ROR zp
      // shr ah, 1 (load carry flag to eflags)
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0xec;
      // rcr [rdi + op1], 1
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0x9f;
      index = jit_emit_int(p_jit, index, operand1);
      // setb ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x20:
      // JSR
      // push rax
      p_jit[index++] = 0x50;
      // lea rax, [rip - (k_addr_space_size + k_guard_size)]
      p_jit[index++] = 0x48;
      p_jit[index++] = 0x8d;
      p_jit[index++] = 0x05;
      index = jit_emit_int(p_jit,
                           index,
                           -(ssize_t) (k_addr_space_size + k_guard_size));
      // sub rsi, rdi
      p_jit[index++] = 0x48;
      p_jit[index++] = 0x29;
      p_jit[index++] = 0xf8;
      // shr eax, 6 (must be 2^6 == jit_stride)
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xe8;
      p_jit[index++] = 0x06;
      // add eax, 2
      p_jit[index++] = 0x83;
      p_jit[index++] = 0xc0;
      p_jit[index++] = 0x02;
      // mov [rdi + rcx], ah
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x24;
      p_jit[index++] = 0x0f;
      // dec cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc9;
      // mov [rdi + rcx], al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x0f;
      // dec cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc9;
      // pop rax
      p_jit[index++] = 0x58;
      // lea rsi, [rdi + k_addr_space_size + k_guard_size +
      //               op1,op2 * jit_stride]
      p_jit[index++] = 0x48;
      p_jit[index++] = 0x8d;
      p_jit[index++] = 0xb7;
      index = jit_emit_int(p_jit,
                           index,
                           k_addr_space_size + k_guard_size +
                               ((operand1 + (operand2 << 8)) * jit_stride));
      // jmp rsi
      p_jit[index++] = 0xff;
      p_jit[index++] = 0xe6;
      break;
    case 0x48:
      // PHA
      // mov [rdi + rcx], al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x0f;
      // dec cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc9;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x4a:
      // LSR A
      // shr al, 1
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0xe8;
      // setb ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x4c:
      // JMP
      // lea rsi, [rdi + k_addr_space_size + k_guard_size +
      //               op1,op2 * jit_stride]
      p_jit[index++] = 0x48;
      p_jit[index++] = 0x8d;
      p_jit[index++] = 0xb7;
      index = jit_emit_int(p_jit,
                           index,
                           k_addr_space_size + k_guard_size +
                               ((operand1 + (operand2 << 8)) * jit_stride));
      // jmp rsi
      p_jit[index++] = 0xff;
      p_jit[index++] = 0xe6;
      break;
    case 0x50:
      // BVC
      // test r8b, 0x40
      p_jit[index++] = 0x41;
      p_jit[index++] = 0xf6;
      p_jit[index++] = 0xc0;
      p_jit[index++] = 0x40;
      // je
      index = jit_emit_do_relative_jump(p_jit,
                                        jit_stride,
                                        index,
                                        0x74,
                                        operand1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x58:
      // CLI
      // btr r8, 2
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xf0;
      p_jit[index++] = 0x02;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x60:
      // RTS
      // push rax
      p_jit[index++] = 0x50;
      // inc cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc1;
      // mov al, [rdi + rcx]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x0f;
      // inc cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc1;
      // mov ah, [rdi + rcx]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x24;
      p_jit[index++] = 0x0f;
      // inc ax
      p_jit[index++] = 0x66;
      p_jit[index++] = 0xff;
      p_jit[index++] = 0xc0;
      // shl eax, 6
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xe0;
      p_jit[index++] = 0x06;
      // lea rsi, [rax + rdi + k_addr_space_size + k_guard_size]
      p_jit[index++] = 0x48;
      p_jit[index++] = 0x8d;
      p_jit[index++] = 0xb4;
      p_jit[index++] = 0x38;
      index = jit_emit_int(p_jit, index, k_addr_space_size + k_guard_size);
      // pop rax
      p_jit[index++] = 0x58;
      // jmp rsi
      p_jit[index++] = 0xff;
      p_jit[index++] = 0xe6;
      break;
    case 0x68:
      // PLA
      // inc cl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc1;
      // mov al, [rdi + rcx]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x0f;
      index = jit_emit_do_zn_flags(p_jit, index, 0);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x6a:
      // ROR A
      // shr ah, 1 (load carry flag to eflags)
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0xec;
      // rcr al, 1
      p_jit[index++] = 0xd0;
      p_jit[index++] = 0xd8;
      // setb ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x92;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x78:
      // SEI
      // bts r8, 2
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xe8;
      p_jit[index++] = 0x02;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x85:
      // STA zp
      // mov [rdi + op1], al
      // TODO: can be optimized to 1-byte offset for 0-0x7f.
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x87;
      p_jit[index++] = operand1;
      p_jit[index++] = 0x00;
      p_jit[index++] = 0x00;
      p_jit[index++] = 0x00;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x88:
      // DEY
      // dec bh
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xcf;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x8a:
      // TXA
      // mov al, bl
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xd8;
      index = jit_emit_do_zn_flags(p_jit, index, 0);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x8d:
      // STA abs
      // mov [rdi + op1,op2], al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x87;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0x8c:
      // STY abs
      // mov [rdi + op1,op2], bh
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xbf;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0x8e:
      // STX abs
      // mov [rdi + op1,op2], bl
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x9f;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0x90:
      // BCC
      // test ah, ah
      p_jit[index++] = 0x84;
      p_jit[index++] = 0xe4;
      // je
      index = jit_emit_do_relative_jump(p_jit,
                                        jit_stride,
                                        index,
                                        0x74,
                                        operand1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x95:
      // STA zp, X
      // mov esi, ebx
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xde;
      // add si, op1
      p_jit[index++] = 0x66;
      p_jit[index++] = 0x81;
      p_jit[index++] = 0xc6;
      p_jit[index++] = operand1;
      p_jit[index++] = 0x00;
      // and si, 0xff
      p_jit[index++] = 0x66;
      p_jit[index++] = 0x81;
      p_jit[index++] = 0xe6;
      p_jit[index++] = 0xff;
      p_jit[index++] = 0x00;
      // mov [rdi + rsi], al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x37;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0x99:
      // STA abs, Y
      // mov esi, ebx
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xde;
      // shr esi, 8
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xee;
      p_jit[index++] = 0x08;
      // add si, op1,op2
      p_jit[index++] = 0x66;
      p_jit[index++] = 0x81;
      p_jit[index++] = 0xc6;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      // mov [rdi + rsi], al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x37;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0x9a:
      // TXS
      // mov cl, bl
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xd9;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0x9d:
      // STA abs, X
      // mov esi, ebx
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xde;
      // and si, 0xff
      p_jit[index++] = 0x66;
      p_jit[index++] = 0x81;
      p_jit[index++] = 0xe6;
      p_jit[index++] = 0xff;
      p_jit[index++] = 0x00;
      // add si, op1,op2
      p_jit[index++] = 0x66;
      p_jit[index++] = 0x81;
      p_jit[index++] = 0xc6;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      // mov [rdi + rsi], al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x37;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0xa0:
      // LDY #imm
      // mov bh, op1
      p_jit[index++] = 0xb7;
      p_jit[index++] = operand1;
      index = jit_emit_do_zn_flags(p_jit, index, 2);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xa2:
      // LDX #imm
      // mov bl, op1
      p_jit[index++] = 0xb3;
      p_jit[index++] = operand1;
      index = jit_emit_do_zn_flags(p_jit, index, 1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xa9:
      // LDA #imm
      // mov al, op1
      p_jit[index++] = 0xb0;
      p_jit[index++] = operand1;
      index = jit_emit_do_zn_flags(p_jit, index, 0);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xaa:
      // TAX
      // mov bl, al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xc3;
      index = jit_emit_do_zn_flags(p_jit, index, 1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0xac:
      // LDY abs
      // mov bh, [rdi + op1,op2]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0xbf;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      index = jit_emit_do_zn_flags(p_jit, index, 2);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0xad:
      // LDA abs
      // mov al, [rdi + op1,op2]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x87;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      index = jit_emit_do_zn_flags(p_jit, index, 0);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0xae:
      // LDX abs
      // mov bl, [rdi + op1,op2]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x9f;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      index = jit_emit_do_zn_flags(p_jit, index, 1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0xb0:
      // BCS
      // test ah, ah
      p_jit[index++] = 0x84;
      p_jit[index++] = 0xe4;
      // jne
      index = jit_emit_do_relative_jump(p_jit,
                                        jit_stride,
                                        index,
                                        0x75,
                                        operand1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xb9:
      // LDA abs, Y
      // mov esi, ebx
      p_jit[index++] = 0x89;
      p_jit[index++] = 0xde;
      // shr esi, 8
      p_jit[index++] = 0xc1;
      p_jit[index++] = 0xee;
      p_jit[index++] = 0x08;
      // add si, op1,op2
      p_jit[index++] = 0x66;
      p_jit[index++] = 0x81;
      p_jit[index++] = 0xc6;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      // mov al, [rdi + rsi]
      p_jit[index++] = 0x8a;
      p_jit[index++] = 0x04;
      p_jit[index++] = 0x37;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0xc9:
      // CMP #imm
      // mov ah, al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xc4;
      // sub ah, op1
      p_jit[index++] = 0x80;
      p_jit[index++] = 0xec;
      p_jit[index++] = operand1;
      // setae ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x93;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xca:
      // DEX
      // dec bl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xcb;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0xcd:
      // CMP abs
      // mov ah, al
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xc4;
      // sub ah, [rdi + op1,op2]
      p_jit[index++] = 0x2a;
      p_jit[index++] = 0xa7;
      p_jit[index++] = operand1;
      p_jit[index++] = operand2;
      p_jit[index++] = 0;
      p_jit[index++] = 0;
      // setae ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x93;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 3);
      break;
    case 0xd0:
      // BNE
      // test dl, dl
      p_jit[index++] = 0x84;
      p_jit[index++] = 0xd2;
      // je
      index = jit_emit_do_relative_jump(p_jit,
                                        jit_stride,
                                        index,
                                        0x74,
                                        operand1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xd8:
      // CLD
      // btr r8, 3
      p_jit[index++] = 0x49;
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0xba;
      p_jit[index++] = 0xf0;
      p_jit[index++] = 0x03;
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0xe0:
      // CPX #imm
      // mov ah, bl
      p_jit[index++] = 0x88;
      p_jit[index++] = 0xdc;
      // sub ah, op1
      p_jit[index++] = 0x80;
      p_jit[index++] = 0xec;
      p_jit[index++] = operand1;
      // setae ah
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x93;
      p_jit[index++] = 0xc4;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    case 0xe8:
      // INX
      // inc bl
      p_jit[index++] = 0xfe;
      p_jit[index++] = 0xc3;
      index = jit_emit_do_zn_flags(p_jit, index, -1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 1);
      break;
    case 0xf0:
      // BEQ
      // test dl, dl
      p_jit[index++] = 0x84;
      p_jit[index++] = 0xd2;
      // jne
      index = jit_emit_do_relative_jump(p_jit,
                                        jit_stride,
                                        index,
                                        0x75,
                                        operand1);
      jit_emit_do_jmp_next(p_jit, jit_stride, index, 2);
      break;
    default:
      // ud2
      p_jit[index++] = 0x0f;
      p_jit[index++] = 0x0b;
      // Copy of unimplemented 6502 opcode.
      p_jit[index++] = opcode;
      // Virtual address of opcode, big endian.
      p_jit[index++] = jit_offset >> 8;
      p_jit[index++] = jit_offset & 0xff;
      break;
    }

    assert(index <= k_jit_bytes_per_byte);

    p_mem++;
    p_jit += jit_stride;
    jit_offset++;
  }
}

static void
jit_enter(const char* p_mem,
          size_t jit_stride,
          size_t vector_addr) {
  unsigned char addr_lsb = p_mem[vector_addr];
  unsigned char addr_msb = p_mem[vector_addr + 1];
  unsigned int addr = (addr_msb << 8) | addr_lsb;
  const char* p_jit = p_mem + k_addr_space_size + k_guard_size;
  const char* p_entry = p_jit + (addr * jit_stride);

  asm volatile (
    // al is 6502 A.
    // ah is 6502 CF.
    "xor %%eax, %%eax;"
    // bl is 6502 X.
    // bh is 6502 Y.
    "xor %%ebx, %%ebx;"
    // cl is 6502 S.
    // ch is 0x01 so that cx is 0x1xx, an offset from virtual RAM base.
    "mov $0x00000100, %%ecx;"
    // dl is 6502 ZF.
    // dh is 6502 NF.
    "xor %%edx, %%edx;"
    // r8 is the rest of the 6502 flags or'ed together.
    // Bit 5 is always set.
    // Bit 4 is set for BRK and PHP.
    "xor %%r8, %%r8;"
    "bts $5, %%r8;"
    "bts $4, %%r8;"
    // rdi points to the virtual RAM, guard page, JIT space.
    "mov %1, %%rdi;"
    // Use rsi as a scratch register for jump location.
    "mov %0, %%rsi;"
    "call *%%rsi;"
    :
    : "r" (p_entry), "r" (p_mem)
    : "rax", "rbx", "rcx", "rdx", "rdi", "rsi", "r8"
  );
}

int
main(int argc, const char* argv[]) {
  char* p_map;
  char* p_mem;
  int fd;
  ssize_t read_ret;

  p_map = mmap(NULL,
               (k_addr_space_size * (k_jit_bytes_per_byte + 1)) +
                   (k_guard_size * 3),
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);
  p_mem = p_map + k_guard_size;

  mprotect(p_map,
           k_guard_size,
           PROT_NONE);
  mprotect(p_mem + k_addr_space_size,
           k_guard_size,
           PROT_NONE);
  mprotect(p_mem + (k_addr_space_size * (k_jit_bytes_per_byte + 1)) +
               k_guard_size,
           k_guard_size,
           PROT_NONE);

  mprotect(p_mem + k_addr_space_size + k_guard_size,
           k_addr_space_size * k_jit_bytes_per_byte,
           PROT_READ | PROT_WRITE | PROT_EXEC);

  p_mem = p_map + k_guard_size;

  fd = open("os12.rom", O_RDONLY);
  if (fd < 0) {
    errx(1, "can't load rom");
  }
  read_ret = read(fd, p_mem + k_os_rom_offset, k_os_rom_len);
  if (read_ret != k_os_rom_len) {
    errx(1, "can't read rom");
  }
  close(fd);

  jit_init(p_mem, k_jit_bytes_per_byte, k_addr_space_size);
  jit_jit(p_mem, k_jit_bytes_per_byte, k_os_rom_offset, k_os_rom_len);
  jit_enter(p_mem, k_jit_bytes_per_byte, k_vector_reset);

  return 0;
}
