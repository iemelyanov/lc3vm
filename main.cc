#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Short description of LC3
// ------------------------
//
// - Memory corresponds to 2^16 location, eaech containig one word (16 bits).
// - Address numered from 0x0000 to 0xFFFF.
// - Total 10 (16 bit) registers. 8 general with program counter and flag cond.
//
// Memory map
// ----------
//
// +-----------------------+ 0x0000
// |   Trap Vector Table   |
// +-----------------------+ 0x00FF
// +-----------------------+ 0x0100
// | Interupt Vector Table |
// +-----------------------+ 0x01FF
// +-----------------------+ 0x0200
// |  Operatin system and  |
// /                       /
// |    Supervisor stack   |
// +-----------------------+ 0x2FFF
// +-----------------------+ 0x3000
// |                       |
// |                       |
// /  Available for user   /
// |      programm         |
// |                       |
// +-----------------------+ 0xFDFF
// +-----------------------+ 0xFE00
// | Device register addr  |
// +-----------------------+ 0xFFFF
//
//
// Assembly hello world
// --------------------
//
// .ORIG x3000                        ; this is the address in memory where the program will be loaded
// LEA R0, HELLO_STR                  ; load the address of the HELLO_STR string into
// R0 PUTs                            ; output the string pointed to by R0 to the console
// HALT                               ; halt the program
// HELLO_STR .STRINGZ "Hello World!"  ; store this string here in the program
// .END                               ; mark the end of the file
//
// Loop assembly
// -------------
//
// AND R0, R0, 0                      ; clear R0
// LOOP                               ; label at the top of our loop
// ADD R0, R0, 1                      ; add 1 to R0 and store back in R0
// ADD R1, R0, -10                    ; subtract 10 from R0 and store back in R1
// BRn LOOP                           ; go back to LOOP if the result was
// negative
// ... ; R0 is now 10!
//
//
// Transalation to binary code
// ---------------------------
//
// ADD R0, R0, 3 -> 0001 0000 0000 0011
//
//
// Directives
// ----------
//
// .ORIG    - address in memory where the program will be load
// .STRINGZ - inserts a string of characters into the program binary at the
// location it is written

typedef uint16_t u16;
typedef int64_t i64;
typedef size_t usize;

namespace lc3 {

enum : u16 {
  kR0 = 0,
  kR1,
  kR2,
  kR3,
  kR4,
  kR5,
  kR6,
  kR7,
  kRPC,
  kRCond,
};

constexpr int kRegCnt = kRCond + 1;

// Each instruction is 16 bit long, with the left 4 bits sotring the opcode.
enum : u16 {
  kOpAdd = 0x1,   // add
  kOpAnd = 0x5,   // bitwise and
  kOpBr = 0x0,    // branch
  kOpJmp = 0xC,   // jump
  kOpJsr = 0x4,   // jump register
  kOpLd = 0x2,    // load
  kOpLdi = 0xA,   // load indirect
  kOpLdr = 0x6,   // load register
  kOpLea = 0xE,   // load effective address
  kOpNot = 0x9,   // bitwise not
  kOpRti = 0x8,   // unused
  kOpRes = 0xD,   // reserved (unused)
  kOpSt = 0x3,    // store
  kOpSti = 0xB,   // store indirect
  kOpStr = 0x7,   // store register
  kOpTrap = 0xF,  // execute trap
};

enum : u16 {
  kFlPos = 1 << 0,
  kFlZro = 1 << 1,
  kFlNeg = 1 << 2,
};

enum : u16 {
  kTrapGetc = 0x20,   // get character from keyboard, not echoed onto the terminal
  kTrapOut = 0x21,    // output a character
  kTrapPuts = 0x22,   // output a word string
  kTrapIn = 0x23,     // get character from keyboard, echoed onto the terminal
  kTrapPutsp = 0x24,  // output a byte string
  kTrapHalt = 0x25,   // halt the program
};

enum {
  kKeyboardStatus = 0xFE00,
  kKeyboardData = 0xFE02,
};

constexpr int kMaxMemory = UINT16_MAX;

u16 SignExtend(u16 x, int bits_cnt) {
  if ((x >> (bits_cnt - 1)) & 1) return (0xFFFF << bits_cnt) | x;
  return x;
}

u16 u16Swap(u16 x) { return (x >> 8) | (x << 8); }

termios original_tio;

void RestoreInputBuffering() { tcsetattr(STDIN_FILENO, TCSANOW, &original_tio); }

void HandleInterrupt(int signal) {
  RestoreInputBuffering();
  printf("\n");
  exit(-2);
}

void DisableInputBuffering() {
  tcgetattr(STDIN_FILENO, &original_tio);
  struct termios new_tio = original_tio;
  new_tio.c_lflag &= ~ICANON & ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

u16 CheckKey() {
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  timeval timeout = {
      .tv_sec = 0,
      .tv_usec = 0,
  };
  return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

u16 R0(u16 instr) { return (instr >> 9) & 0x7; }
u16 R1(u16 instr) { return (instr >> 6) & 0x7; }
u16 R2(u16 instr) { return instr & 0x7; }

class VM {
 public:
  bool LoadImage(const char *image, int image_size) {
    if (!image || image_size == 0 || image_size > kMaxMemory) return false;

    u16 origin;
    memcpy(&origin, image, sizeof(origin));
    origin = u16Swap(origin);
    usize n = image_size - sizeof(origin);
    memcpy(memory_ + origin, image + sizeof(origin), n);

    u16 *p = memory_ + origin;
    while (n-- > 0) {
      *p = u16Swap(*p);
      ++p;
    }

    return true;
  }

  void Run();

 private:
  void UpdateFlag(u16 r) {
    if (reg_[r] == 0) {
      reg_[kRCond] = kFlZro;
    } else if (reg_[r] >> 15) {
      reg_[kRCond] = kFlNeg;
    } else {
      reg_[kRCond] = kFlPos;
    }
  }

  u16 MemRead(size_t addr) {
    if (addr == kKeyboardStatus) {
      if (CheckKey()) {
        memory_[kKeyboardStatus] = 1 << 15;
        memory_[kKeyboardData] = getchar();
      } else {
        memory_[kKeyboardStatus] = 0;
      }
    }
    return memory_[addr];
  }

  u16 memory_[kMaxMemory] = {0};
  u16 reg_[kRegCnt] = {0};
};

void VM::Run() {
  signal(SIGINT, HandleInterrupt);
  DisableInputBuffering();

  // Initial state
  reg_[kRCond] = kFlZro;
  reg_[kRPC] = 0x3000;

  bool running = true;
  while (running) {
    u16 addr = reg_[kRPC]++;
    u16 instr = MemRead(addr);
    u16 op = instr >> 12;

    switch (op) {
      case kOpAdd: {
        u16 r0 = R0(instr);
        u16 r1 = R1(instr);
        if ((instr >> 5) & 0x1) {
          reg_[r0] = reg_[r1] + SignExtend(instr & 0x1F, 5);
        } else {
          reg_[r0] = reg_[r1] + reg_[R2(instr)];
        }
        UpdateFlag(r0);
        break;
      }
      case kOpAnd: {
        u16 r0 = R0(instr);
        u16 r1 = R1(instr);
        if ((instr >> 5) & 0x1) {
          reg_[r0] = reg_[r1] & SignExtend(instr & 0x1F, 5);
        } else {
          reg_[r0] = reg_[r1] & reg_[R2(instr)];
        }
        UpdateFlag(r0);
        break;
      }
      case kOpBr: {
        if (reg_[kRCond] & ((instr >> 9) & 0x7)) {
          reg_[kRPC] += SignExtend(instr & 0x1FF, 9);
        }
        break;
      }
      case kOpJmp: /* RET */ {
        reg_[kRPC] = reg_[R1(instr)];
        break;
      }
      case kOpJsr: {
        reg_[kR7] = reg_[kRPC];
        if ((instr >> 11) & 0x1) {
          // JSR
          reg_[kRPC] += SignExtend(instr & 0x7FF, 11);
        } else {
          // JSRR
          reg_[kRPC] = reg_[R1(instr)];
        }
        break;
      }
      case kOpLd: {
        u16 r0 = R0(instr);
        u16 addr = reg_[kRPC] + SignExtend(instr & 0x1FF, 9);
        reg_[r0] = MemRead(addr);
        UpdateFlag(r0);
        break;
      }
      case kOpLdi: {
        u16 r0 = R0(instr);
        u16 addr = reg_[kRPC] + SignExtend(instr & 0x1FF, 9);
        reg_[r0] = MemRead(MemRead(addr));
        UpdateFlag(r0);
        break;
      }
      case kOpLdr: {
        u16 r0 = R0(instr);
        u16 addr = reg_[R1(instr)] + SignExtend(instr & 0x3F, 6);
        reg_[r0] = MemRead(addr);
        UpdateFlag(r0);
        break;
      }
      case kOpLea: {
        u16 r0 = R0(instr);
        reg_[r0] = reg_[kRPC] + SignExtend(instr & 0x1FF, 9);
        UpdateFlag(r0);
        break;
      }
      case kOpNot: {
        u16 r0 = R0(instr);
        reg_[r0] = ~reg_[R1(instr)];
        UpdateFlag(r0);
        break;
      }
      case kOpRti:
      case kOpRes:
        printf("bad opcode");
        running = false;
        break;
      case kOpSt: {
        u16 addr = reg_[kRPC] + SignExtend(instr & 0x1FF, 9);
        memory_[addr] = reg_[R0(instr)];
        break;
      }
      case kOpSti: {
        u16 addr = reg_[kRPC] + SignExtend(instr & 0x1FF, 9);
        memory_[MemRead(addr)] = reg_[R0(instr)];
        break;
      }
      case kOpStr: {
        u16 addr = reg_[R1(instr)] + SignExtend(instr & 0x3F, 6);
        memory_[addr] = reg_[R0(instr)];
        break;
      }
      case kOpTrap: {
        switch (instr & 0xFF) {
          case kTrapGetc: {
            reg_[kR0] = getchar();
            UpdateFlag(kR0);
            break;
          }
          case kTrapOut: {
            putc(reg_[kR0], stdout);
            fflush(stdout);
            break;
          }
          case kTrapPuts: {
            u16 *c = memory_ + reg_[kR0];
            while (*c) {
              putc((char)*c, stdout);
              ++c;
            }
            fflush(stdout);
            break;
          }
          case kTrapIn: {
            printf("Enter a character: ");
            char c = getc(stdin);
            putc(c, stdout);
            fflush(stdout);
            reg_[kR0] = c;
            UpdateFlag(kR0);
            break;
          }
          case kTrapPutsp: {
            u16 *c = memory_ + reg_[kR0];
            while (*c) {
              char c1 = *c & 0xFF;
              putc(c1, stdout);
              char c2 = *c >> 8;
              if (c2) putc(c2, stdout);
              ++c;
            }
            fflush(stdout);
            break;
          }
          case kTrapHalt: {
            running = false;
            puts("halt");
            fflush(stdout);
            break;
          }
        }
        break;
      }
      default:
        printf("bad opcode");
        running = false;
        break;
    }
  }

  RestoreInputBuffering();
}

}  // namespace lc3

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("Usage: lc3 <image-file>\n");
    return 0;
  }

  FILE *file = fopen(argv[1], "rb");
  if (!file) {
    printf("can't open file: %s\n", argv[1]);
    return 0;
  }

  fseek(file, 0, SEEK_END);
  i64 n = ftell(file);
  rewind(file);

  char *image = new char[n + 1];
  fread(image, 1, n, file);
  fclose(file);

  lc3::VM vm;
  vm.LoadImage(image, n);
  vm.Run();

  delete[] image;

  return 0;
}
