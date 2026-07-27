#ifdef ENABLE_SPIKE
#include "spike_ref.h"
#endif
#include "CSR.h"
#include "RISCV.h"
#include "ref.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
extern "C" {
#ifdef USE_SIMULATOR_SOFTFLOAT
#include "softfloat.h"
#else
#include "softfloat/softfloat.h"
#endif
}

namespace {
constexpr uint32_t kSdramBase = SDRAM_BASE;
constexpr uint64_t kSdramUpperBound =
    static_cast<uint64_t>(SDRAM_BASE) + SDRAM_SIZE;
constexpr uint32_t kRamBase = DDR_BASE;
constexpr uint64_t kRamUpperBound = 0x100000000ull;
constexpr uint32_t kRamSizeBytes = kRamUpperBound - kRamBase;
constexpr uint32_t kBootIoBase = 0x00000000u;
constexpr uint32_t kBootIoSize = 0x00100000u;
constexpr uint32_t kSdSectorSize = 512u;
constexpr bool kLogVerboseTimer = false;
constexpr bool kLogVerboseReturns = false;
constexpr bool kLogVerboseCsrMip = false;
constexpr bool kLogVerboseXpsIntc = false;
constexpr bool kLogVerboseOcsdcIrqEnable = false;
constexpr bool kLogVerboseOcsdcData = false;
constexpr bool kLogProgressSimTime = false;
constexpr bool kLogUserTrapSummary = false;
constexpr bool kLogUserEntrySummary = false;
constexpr bool kLogUserSyscallSummary = false;
constexpr bool kMirrorUserWriteSyscalls = true;
static uint64_t g_ref_timer_offset = 0;
static uint64_t g_ref_timer_cmp = ~0ull;
static bool g_disable_uart_console_output = false;

constexpr uint32_t kUartIrqId = 2u;
constexpr uint32_t kOcsdcCmdIrqId = 3u;
constexpr uint32_t kOcsdcDatIrqId = 4u;

constexpr uint32_t kUartRegRxTxDll = 0x0u;
constexpr uint32_t kUartRegIerDlm = 0x1u;
constexpr uint32_t kUartRegIirFcr = 0x2u;
constexpr uint32_t kUartRegLcr = 0x3u;
constexpr uint32_t kUartRegMcr = 0x4u;
constexpr uint32_t kUartRegLsr = 0x5u;
constexpr uint32_t kUartRegMsr = 0x6u;
constexpr uint32_t kUartRegScr = 0x7u;

constexpr uint8_t kUartLcrDlab = 0x80u;
constexpr uint8_t kUartLsrThre = 0x20u;
constexpr uint8_t kUartLsrTemt = 0x40u;
constexpr uint8_t kUartMsrCts = 0x10u;
constexpr uint8_t kUartMsrDsr = 0x20u;
constexpr uint8_t kUartMsrDcd = 0x80u;
constexpr uint8_t kUartIerThri = 0x02u;
constexpr uint8_t kUartIirNoInt = 0x01u;
constexpr uint8_t kUartIirThre = 0x02u;
constexpr uint8_t kUartIirFifoEnabled = 0xc0u;

bool ref_uart_trace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_UART_TRACE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool ref_ocsdc_probe_trace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_OCSDC_PROBE_TRACE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool ref_stop_at_shell_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_STOP_AT_SHELL");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool ref_init_print_trace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("REF_INIT_PRINT_TRACE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool observe_uart_console_byte(uint64_t time, char ch) {
  static char window[4] = {};
  static bool init_banner_seen = false;
  static unsigned init_banner_pos = 0;
  static uint64_t init_banner_start_time = 0;
  constexpr char kInitBannerPrefix[] = "_     ___ _   _";

  if (ref_init_print_trace_enabled() && !init_banner_seen) {
    if (ch == kInitBannerPrefix[init_banner_pos]) {
      if (init_banner_pos == 0) {
        init_banner_start_time = time;
      }
      ++init_banner_pos;
      if (init_banner_pos == sizeof(kInitBannerPrefix) - 1) {
        init_banner_seen = true;
        std::cerr << "\n[RefCPU][INIT_PRINT_BEGIN] sim_time="
                  << init_banner_start_time
                  << " confirm_time=" << time << std::endl;
      }
    } else {
      init_banner_pos = (ch == kInitBannerPrefix[0]) ? 1u : 0u;
      if (init_banner_pos == 1u) {
        init_banner_start_time = time;
      }
    }
  }

  window[0] = window[1];
  window[1] = window[2];
  window[2] = window[3];
  window[3] = ch;
  if (!ref_stop_at_shell_enabled()) {
    return false;
  }
  if (window[0] == '/' && window[1] == ' ' && window[2] == '#' &&
      window[3] == ' ') {
    std::cerr << "\n[RefCPU][SHELL] sim_time=" << time << std::endl;
    return true;
  }
  return false;
}

const char *uart_reg_name(uint32_t reg) {
  switch (reg) {
  case kUartRegRxTxDll:
    return "RBR/THR/DLL";
  case kUartRegIerDlm:
    return "IER/DLM";
  case kUartRegIirFcr:
    return "IIR/FCR";
  case kUartRegLcr:
    return "LCR";
  case kUartRegMcr:
    return "MCR";
  case kUartRegLsr:
    return "LSR";
  case kUartRegMsr:
    return "MSR";
  case kUartRegScr:
    return "SCR";
  default:
    return "UART";
  }
}

void trace_uart_load(uint64_t time, uint32_t pc, uint32_t paddr, uint8_t func3,
                     uint32_t raw_word, uint32_t result) {
  if (!ref_uart_trace_enabled()) {
    return;
  }
  const uint32_t reg = paddr - UART_BASE;
  std::fprintf(stderr,
               "[REF][UART][LOAD] t=%llu pc=0x%08x addr=0x%08x reg=0x%02x(%s) "
               "func3=0x%x raw_word=0x%08x result=0x%08x\n",
               static_cast<unsigned long long>(time), pc, paddr, reg,
               uart_reg_name(reg), static_cast<unsigned>(func3), raw_word,
               result);
}

void trace_uart_store_effect(uint64_t time, uint32_t pc, uint32_t paddr,
                             uint8_t strb, uint32_t data, uint32_t before0,
                             uint32_t after0, uint32_t after4) {
  if (!ref_uart_trace_enabled()) {
    return;
  }
  const uint32_t reg = paddr - UART_BASE;
  std::fprintf(stderr,
               "[REF][UART][STORE] t=%llu pc=0x%08x addr=0x%08x reg=0x%02x(%s) "
               "strb=0x%x data=0x%08x word0:0x%08x->0x%08x word4=0x%08x\n",
               static_cast<unsigned long long>(time), pc, paddr, reg,
               uart_reg_name(reg), static_cast<unsigned>(strb), data, before0,
               after0, after4);
}

struct SdCardId {
  uint32_t ocr;
  uint32_t cid[4];
  uint32_t csd[4];
};

SdCardId make_sdcard_id(size_t image_size) {
  SdCardId id = {};
  constexpr uint32_t kSdCmdClass =
      (1u << 2) |  // CCC_BLOCK_READ
      (1u << 4) |  // CCC_BLOCK_WRITE
      (1u << 5) |  // CCC_ERASE
      (1u << 6) |  // CCC_WRITE_PROT
      (1u << 8) |  // CCC_APP_SPEC
      (1u << 10);  // CCC_SWITCH
  id.ocr = 0xc0ff8000u; // power up complete + SDHC/SDXC + 2.7V-3.6V
  id.cid[0] = 0x03534453u;
  id.cid[1] = 0x30303030u;
  id.cid[2] = 0x12345678u;
  id.cid[3] = 0x01020304u;

  const uint64_t units = (static_cast<uint64_t>(image_size) + 524287u) / 524288u;
  const uint32_t csize = units == 0 ? 0u : static_cast<uint32_t>(units - 1u);

  id.csd[0] = 0x400e0032u;
  id.csd[1] = kSdCmdClass << 20;
  id.csd[1] |= 9u << 16; // READ_BL_LEN = 512 bytes
  id.csd[1] |= (csize >> 16) & 0x3fu;
  id.csd[2] = (csize & 0xffffu) << 16;
  id.csd[3] = 0u;
  return id;
}

[[noreturn]] void mem_oob_fatal(const char *op, uint32_t addr, uint32_t size) {
  std::cerr << "[RefCPU] illegal " << op << " at paddr=0x" << std::hex << addr
            << " size=0x" << size << " (not in RAM or implemented MMIO)"
            << std::dec << std::endl;
  std::exit(1);
}

std::vector<uint8_t> load_binary_file(const std::string &path,
                                      const char *kind) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "[RefCPU] Failed to open " << kind << ": " << path
              << std::endl;
    std::exit(1);
  }

  file.seekg(0, std::ios::end);
  const std::streamsize sz = file.tellg();
  file.seekg(0, std::ios::beg);
  if (sz < 0) {
    std::cerr << "[RefCPU] Failed to stat " << kind << ": " << path
              << std::endl;
    std::exit(1);
  }

  std::vector<uint8_t> data(static_cast<size_t>(sz));
  if (sz > 0 && !file.read(reinterpret_cast<char *>(data.data()), sz)) {
    std::cerr << "[RefCPU] Failed to read " << kind << ": " << path
              << std::endl;
    std::exit(1);
  }
  return data;
}

inline bool range_within(uint32_t addr, uint32_t size, uint64_t base,
                         uint64_t upper_bound) {
  if (size == 0 || static_cast<uint64_t>(addr) < base) {
    return false;
  }
  const uint64_t end =
      static_cast<uint64_t>(addr) + static_cast<uint64_t>(size) - 1;
  return end < upper_bound;
}

inline bool is_sdram_range(uint32_t addr, uint32_t size) {
  return range_within(addr, size, kSdramBase, kSdramUpperBound);
}

inline bool is_ddr_range(uint32_t addr, uint32_t size) {
  return range_within(addr, size, kRamBase, kRamUpperBound);
}

inline bool is_ram_range(uint32_t addr, uint32_t size) {
  return is_sdram_range(addr, size) || is_ddr_range(addr, size);
}

inline bool is_mmio_range(uint32_t addr, uint32_t size) {
  if (size == 0) {
    return false;
  }

  const uint64_t end =
      static_cast<uint64_t>(addr) + static_cast<uint64_t>(size) - 1;
  const auto in_range = [addr, end](uint32_t base, uint32_t span) {
    const uint64_t range_end =
        static_cast<uint64_t>(base) + static_cast<uint64_t>(span) - 1;
    return addr >= base && end <= range_end;
  };

  return in_range(kBootIoBase, kBootIoSize) ||
         in_range(UART_BASE, UART_MMIO_SIZE) ||
         in_range(XPS_INTC_BASE, XPS_INTC_MMIO_SIZE) ||
         in_range(TIMER_BASE, TIMER_MMIO_SIZE) ||
         in_range(OCSDC_BASE, OCSDC_MMIO_SIZE);
}

inline bool is_legal_phys_range(uint32_t addr, uint32_t size) {
  return is_ram_range(addr, size) || is_mmio_range(addr, size);
}

inline void check_mem_range_or_log(const char *op, uint32_t addr,
                                   uint32_t size) {
  if (size == 0) {
    return;
  }
  if (!is_legal_phys_range(addr, size)) {
    mem_oob_fatal(op, addr, size);
  }
}

static inline float32_t to_f32(uint32_t v) {
  float32_t f;
  f.v = v;
  return f;
}

static inline uint32_t ref_effective_mip(uint32_t mip_reg, uint64_t now,
                                         bool synthesize_timer_interrupt) {
  if (synthesize_timer_interrupt &&
      (now + g_ref_timer_offset) >= g_ref_timer_cmp) {
    return mip_reg | MIP_MTIP;
  }
  return mip_reg;
}

static inline void sync_timer_csrs(CPU_state &state, uint64_t now,
                                   bool synthesize_timer_interrupt) {
  state.csr[csr_sip] =
      ref_effective_mip(state.csr[csr_mip], now, synthesize_timer_interrupt) &
      0x00000333u;
}

static inline uint64_t ref_timer_now(uint64_t now) {
  return now + g_ref_timer_offset;
}

static inline void ref_write_timer_value(uint64_t value, uint64_t now) {
  g_ref_timer_offset = value - now;
}

static inline uint32_t from_f32(float32_t f) { return f.v; }

static inline bool is_nan32(uint32_t v) {
  return ((v & 0x7F800000u) == 0x7F800000u) && ((v & 0x007FFFFFu) != 0);
}

static inline bool is_snan32(uint32_t v) {
  return is_nan32(v) && (((v >> 22) & 1u) == 0);
}

static inline uint32_t f32_min_riscv(uint32_t a, uint32_t b) {
  if (is_snan32(a) || is_snan32(b)) {
    softfloat_exceptionFlags |= softfloat_flag_invalid;
  }
  const bool a_nan = is_nan32(a);
  const bool b_nan = is_nan32(b);
  if (a_nan && b_nan)
    return 0x7fc00000u;
  if (a_nan)
    return b;
  if (b_nan)
    return a;

  const float32_t fa = to_f32(a);
  const float32_t fb = to_f32(b);
  if (f32_lt(fa, fb))
    return a;
  if (f32_lt(fb, fa))
    return b;
  return a | b; // -0.0 wins on ties
}

static inline uint32_t f32_max_riscv(uint32_t a, uint32_t b) {
  if (is_snan32(a) || is_snan32(b)) {
    softfloat_exceptionFlags |= softfloat_flag_invalid;
  }
  const bool a_nan = is_nan32(a);
  const bool b_nan = is_nan32(b);
  if (a_nan && b_nan)
    return 0x7fc00000u;
  if (a_nan)
    return b;
  if (b_nan)
    return a;

  const float32_t fa = to_f32(a);
  const float32_t fb = to_f32(b);
  if (f32_lt(fa, fb))
    return b;
  if (f32_lt(fb, fa))
    return a;
  return a & b; // +0.0 wins on ties
}

static inline uint32_t f32_classify_riscv(float32_t f) {
  const uint32_t bits = from_f32(f);
  const uint32_t sign = (bits >> 31) & 1u;
  const uint32_t exp = (bits >> 23) & 0xFFu;
  const uint32_t mant = bits & 0x7FFFFFu;

  const bool is_subnormal = (exp == 0) && (mant != 0);
  const bool is_zero = (exp == 0) && (mant == 0);
  const bool is_inf = (exp == 0xFFu) && (mant == 0);
  const bool is_nan = (exp == 0xFFu) && (mant != 0);
  const bool is_snan = is_nan && (((mant >> 22) & 1u) == 0);
  const bool is_qnan = is_nan && (((mant >> 22) & 1u) == 1);

  uint32_t res = 0;
  if (is_inf && sign)
    res |= (1u << 0);
  else if (!is_inf && !is_zero && !is_nan && !is_subnormal && sign)
    res |= (1u << 1);
  else if (is_subnormal && sign)
    res |= (1u << 2);
  else if (is_zero && sign)
    res |= (1u << 3);
  else if (is_zero && !sign)
    res |= (1u << 4);
  else if (is_subnormal && !sign)
    res |= (1u << 5);
  else if (!is_inf && !is_zero && !is_nan && !is_subnormal && !sign)
    res |= (1u << 6);
  else if (is_inf && !sign)
    res |= (1u << 7);

  if (is_snan)
    res |= (1u << 8);
  if (is_qnan)
    res |= (1u << 9);
  return res;
}
} // namespace

uint32_t Ref_cpu::visible_mip() const {
  return ref_effective_mip(state.csr[csr_mip], sim_time,
                           device_effects_enable && interrupt_delivery_enable);
}

uint32_t Ref_cpu::visible_sip() const {
  return visible_mip() & 0x00000333u;
}

std::map<uint32_t, uint32_t> load_simpoints(const std::string &filename);
std::map<uint64_t, std::string>
load_special_checkpoint_targets(const std::string &filename);

namespace {
std::string checkpoint_label(std::string label) {
  for (char &ch : label) {
    const bool safe = (ch >= 'a' && ch <= 'z') ||
                      (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!safe)
      ch = '_';
  }
  return label.empty() ? "event" : label;
}
} // namespace

Ref_cpu::~Ref_cpu() {
  if (memory) {
    free(memory);
  }
  if (sdram_memory) {
    free(sdram_memory);
  }
}

void Ref_cpu::init(uint32_t reset_pc, const char *image, uint32_t size) {
  state.pc = reset_pc;
  g_ref_timer_offset = 0;
  g_ref_timer_cmp = ~0ull;
  ram_size = size;
  if (ram_size != kRamSizeBytes) {
    std::cerr << "[RefCPU] Unsupported RAM size: 0x" << std::hex << ram_size
              << ", expected 0x" << kRamSizeBytes << std::dec << std::endl;
    exit(1);
  }

  const uint32_t ram_words = ram_size / sizeof(uint32_t);
  memory = (uint32_t *)calloc(ram_words, sizeof(uint32_t));
  if (!memory) {
    std::cerr << "Error: Could not allocate " << ram_size
              << " bytes of memory" << std::endl;
    exit(1);
  }
  const uint32_t sdram_words = SDRAM_SIZE / sizeof(uint32_t);
  sdram_memory =
      (uint32_t *)calloc(sdram_words, sizeof(uint32_t));
  if (!sdram_memory) {
    std::cerr << "Error: Could not allocate " << SDRAM_SIZE
              << " bytes of SDRAM" << std::endl;
    exit(1);
  }
  io_words.clear();
  io_words[UART_BASE + 0x00] =
      static_cast<uint32_t>(kUartIirNoInt | kUartIirFifoEnabled) << 16;
  io_words[UART_BASE + 0x04] =
      0x00000003u |
      (static_cast<uint32_t>(kUartLsrThre | kUartLsrTemt) << 8) |
      (static_cast<uint32_t>(kUartMsrCts | kUartMsrDsr | kUartMsrDcd) << 16);
  ocsdc_regs.clear();
  ocsdc_data_pending = false;

  if (image != nullptr) {
    std::ifstream inst_data(image, std::ios::in | std::ios::binary);
    if (!inst_data.is_open()) {
      std::cerr << "Error: Image " << image << " does not exist" << std::endl;
      exit(1);
    }

    inst_data.seekg(0, std::ios::end);
    std::streamsize img_size = inst_data.tellg();
    inst_data.seekg(0, std::ios::beg);

    if (img_size < 0 || static_cast<uint64_t>(img_size) > kRamSizeBytes) {
      std::cerr << "[RefCPU] Image too large for DDR window: " << img_size
                << " bytes" << std::endl;
      exit(1);
    }
    const uint32_t img_bytes = static_cast<uint32_t>(img_size);
    check_mem_range_or_log("image load", kRamBase, img_bytes);

    std::cout << "[RefCPU] Loading image at 0x80000000, size: " << img_size
              << " bytes" << std::endl;
    if (!inst_data.read(reinterpret_cast<char *>(memory), img_bytes)) {
      std::cerr << "读取文件失败！" << std::endl;
      exit(1);
    }

    inst_data.close();
  }

  store_word(0x10000004, 0x00006000); // 和进入 OpenSBI 相关
  if (image != nullptr) {
    store_word(0x0, 0xf1402573);
    store_word(0x4, 0x83e005b7);
    store_word(0x8, 0x800002b7);
    store_word(0xc, 0x00028067);
  }

  for (int i = 0; i < 32; i++) {
    state.gpr[i] = 0;
  }
  for (int i = 0; i < 21; i++) {
    state.csr[i] = 0;
  }
  state.csr[csr_misa] = 0x40141103;
  privilege = 0b11;

  state.store = false;
  asy = false;
  page_fault_inst = false;
  page_fault_load = false;
  page_fault_store = false;
  state.reserve_valid = false;
  state.reserve_addr = 0;
  fcsr_fflags = 0;
  fcsr_frm = 0;
  sim_time = 0;
  g_disable_uart_console_output = false;
  difftest_started = false;
  sim_end = false;
  uart_print = false;
  ref_only = false;
  ocsdc_reset();
  xps_intc_reset();
}

void Ref_cpu::load_flash_image(const std::string &path) {
  flash_image = load_binary_file(path, "flash image");
  if (flash_image.size() > kBootIoSize) {
    std::cerr << "[RefCPU] Flash image too large for boot window: "
              << flash_image.size() << " > " << kBootIoSize << std::endl;
    std::exit(1);
  }

  std::cout << "[RefCPU] Loading flash image at 0x0, size: "
            << flash_image.size() << " bytes" << std::endl;
  for (size_t i = 0; i < flash_image.size(); ++i) {
    const uint32_t addr = static_cast<uint32_t>(i);
    const uint32_t word_addr = addr & ~0x3u;
    const uint32_t shift = (addr & 0x3u) * 8;
    uint32_t old_word = load_word(word_addr);
    old_word &= ~(0xffu << shift);
    old_word |= static_cast<uint32_t>(flash_image[i]) << shift;
    store_word(word_addr, old_word);
  }
}

void Ref_cpu::load_sdcard_image(const std::string &path) {
  sdcard_image = load_binary_file(path, "sdcard image");
  const uint64_t rounded_capacity =
      ((static_cast<uint64_t>(sdcard_image.size()) + 524287u) / 524288u) *
      524288u;
  if (rounded_capacity > sdcard_image.size()) {
    sdcard_image.resize(static_cast<size_t>(rounded_capacity), 0);
  }
  std::cout << "[RefCPU] Loaded sdcard backend, size: " << sdcard_image.size()
            << " bytes" << std::endl;
}

void Ref_cpu::ocsdc_reset() {
  ocsdc_regs.clear();
  ocsdc_regs[OCSDC_BASE + 0x00] = 0;
  ocsdc_regs[OCSDC_BASE + 0x04] = 0;
  ocsdc_regs[OCSDC_BASE + 0x2c] = 3300; // POWER_CONTROL
  ocsdc_regs[OCSDC_BASE + 0x30] = 0;
  ocsdc_regs[OCSDC_BASE + 0x34] = 0;
  ocsdc_regs[OCSDC_BASE + 0x38] = 0;
  ocsdc_regs[OCSDC_BASE + 0x3c] = 0;
  ocsdc_regs[OCSDC_BASE + 0x40] = 0;
  ocsdc_data_pending = false;
}

void Ref_cpu::xps_intc_reset() {
  xps_intc_isr = 0;
  xps_intc_ier = 0;
  xps_intc_mer = 0;
  io_words[XPS_INTC_BASE + 0x00] = 0; // ISR
  io_words[XPS_INTC_BASE + 0x04] = 0; // IPR
  io_words[XPS_INTC_BASE + 0x08] = 0; // IER
  io_words[XPS_INTC_BASE + 0x18] = 0xffffffffu; // IVR
  io_words[XPS_INTC_BASE + 0x1c] = 0; // MER
  refresh_external_interrupt();
}

static uint32_t uart_reg_word_addr(uint32_t addr) {
  return UART_BASE + ((addr - UART_BASE) & ~0x3u);
}

static uint8_t uart_extract_byte(uint32_t word, uint32_t byte_index) {
  return static_cast<uint8_t>((word >> (byte_index * 8)) & 0xffu);
}

static uint32_t uart_update_byte(uint32_t word, uint32_t byte_index,
                                 uint8_t value) {
  const uint32_t shift = byte_index * 8;
  word &= ~(0xffu << shift);
  word |= static_cast<uint32_t>(value) << shift;
  return word;
}

static uint8_t uart_read_reg8(
    const std::unordered_map<uint32_t, uint32_t> &io_words, uint32_t addr) {
  const uint32_t word_addr = uart_reg_word_addr(addr);
  const uint32_t byte_index = addr & 0x3u;
  auto it = io_words.find(word_addr);
  const uint32_t word = (it == io_words.end()) ? 0u : it->second;
  return uart_extract_byte(word, byte_index);
}

static void uart_write_reg8(std::unordered_map<uint32_t, uint32_t> &io_words,
                            uint32_t addr, uint8_t value) {
  const uint32_t word_addr = uart_reg_word_addr(addr);
  const uint32_t byte_index = addr & 0x3u;
  auto it = io_words.find(word_addr);
  const uint32_t old_word = (it == io_words.end()) ? 0u : it->second;
  io_words[word_addr] = uart_update_byte(old_word, byte_index, value);
}

void Ref_cpu::uart_refresh_interrupt() {
  const uint8_t ier = uart_read_reg8(io_words, UART_BASE + kUartRegIerDlm);
  const uint8_t lsr = uart_read_reg8(io_words, UART_BASE + kUartRegLsr);
  const bool thre_pending = ((ier & kUartIerThri) != 0) &&
                            ((lsr & kUartLsrThre) != 0);
  uart_write_reg8(io_words, UART_BASE + kUartRegIirFcr,
                  static_cast<uint8_t>(kUartIirFifoEnabled |
                                       (thre_pending ? kUartIirThre
                                                     : kUartIirNoInt)));
  xps_intc_set_irq_level(kUartIrqId, thre_pending);
}

uint32_t Ref_cpu::xps_intc_read_reg(uint32_t word_addr) const {
  switch (word_addr - XPS_INTC_BASE) {
  case 0x00:
    return xps_intc_isr;
  case 0x04:
    return xps_intc_isr & xps_intc_ier;
  case 0x08:
    return xps_intc_ier;
  case 0x18: {
    const uint32_t pending = xps_intc_isr & xps_intc_ier;
    if (pending == 0) {
      return 0xffffffffu;
    }
    for (uint32_t i = 0; i < 32; ++i) {
      if (pending & (1u << i)) {
        return i;
      }
    }
    return 0xffffffffu;
  }
  case 0x1c:
    return xps_intc_mer;
  default: {
    auto it = io_words.find(word_addr);
    return it == io_words.end() ? 0u : it->second;
  }
  }
}

void Ref_cpu::refresh_external_interrupt() {
  const bool global_enable = (xps_intc_mer & 0x3u) == 0x3u;
  const bool pending = global_enable && ((xps_intc_isr & xps_intc_ier) != 0);
  if (pending) {
    state.csr[csr_mip] |= MIP_SEIP;
  } else {
    state.csr[csr_mip] &= ~MIP_SEIP;
  }
  state.csr[csr_sip] = visible_sip();
  if (kLogVerboseXpsIntc && (xps_intc_isr != 0 || xps_intc_ier != 0 ||
                             xps_intc_mer != 0 || pending)) {
    std::cerr << "[RefCPU][XPS_INTC] refresh"
              << " isr=0x" << std::hex << xps_intc_isr
              << " ier=0x" << xps_intc_ier
              << " mer=0x" << xps_intc_mer
              << " pending=" << std::dec << pending
              << " mip=0x" << std::hex << state.csr[csr_mip]
              << " sip=0x" << state.csr[csr_sip] << std::dec << std::endl;
  }
}

void Ref_cpu::xps_intc_set_irq_level(uint32_t irq_id, bool asserted) {
  if (irq_id >= 32) {
    return;
  }
  const uint32_t mask = 1u << irq_id;
  if (asserted) {
    xps_intc_isr |= mask;
  } else {
    xps_intc_isr &= ~mask;
  }
  io_words[XPS_INTC_BASE + 0x00] = xps_intc_isr;
  io_words[XPS_INTC_BASE + 0x04] = xps_intc_isr & xps_intc_ier;
  if (kLogVerboseXpsIntc &&
      (irq_id == kUartIrqId || irq_id == kOcsdcCmdIrqId ||
       irq_id == kOcsdcDatIrqId)) {
    std::cerr << "[RefCPU][XPS_INTC] irq" << irq_id
              << (asserted ? " assert" : " clear") << " isr=0x" << std::hex
              << xps_intc_isr << " ier=0x" << xps_intc_ier
              << " mer=0x" << xps_intc_mer
              << " ipr=0x" << (xps_intc_isr & xps_intc_ier) << std::dec
              << std::endl;
  }
  refresh_external_interrupt();
}

void Ref_cpu::xps_intc_write_reg(uint32_t word_addr, uint32_t data) {
  const uint32_t offset = word_addr - XPS_INTC_BASE;
  if (kLogVerboseXpsIntc &&
      (offset == 0x08 || offset == 0x10 || offset == 0x14 ||
       offset == 0x18 || offset == 0x1c)) {
    std::cerr << "[RefCPU][XPS_INTC] write off=0x" << std::hex << offset
              << " data=0x" << data << std::dec << std::endl;
  }
  switch (word_addr - XPS_INTC_BASE) {
  case 0x00: // ISR clear-on-write
    xps_intc_isr &= ~data;
    break;
  case 0x08: // IER
    xps_intc_ier = data;
    break;
  case 0x0c: // IAR
    xps_intc_isr &= ~data;
    break;
  case 0x10: // SIE
    xps_intc_ier |= data;
    break;
  case 0x14: // CIE
    xps_intc_ier &= ~data;
    break;
  case 0x1c: // MER
    xps_intc_mer = data;
    break;
  default:
    io_words[word_addr] = data;
    break;
  }
  io_words[XPS_INTC_BASE + 0x00] = xps_intc_isr;
  io_words[XPS_INTC_BASE + 0x04] = xps_intc_isr & xps_intc_ier;
  io_words[XPS_INTC_BASE + 0x08] = xps_intc_ier;
  io_words[XPS_INTC_BASE + 0x18] = xps_intc_read_reg(XPS_INTC_BASE + 0x18);
  io_words[XPS_INTC_BASE + 0x1c] = xps_intc_mer;
  refresh_external_interrupt();
}

uint32_t Ref_cpu::ocsdc_read_reg(uint32_t word_addr) const {
  auto it = ocsdc_regs.find(word_addr);
  return it == ocsdc_regs.end() ? 0u : it->second;
}

void Ref_cpu::ocsdc_execute_command(uint32_t command, uint32_t argument) {
  constexpr uint32_t kCmdResp48 = 0x1;
  constexpr uint32_t kCmdResp136 = 0x2;
  constexpr uint32_t kCmdDataRead = 0x20;
  constexpr uint32_t kCmdDataWrite = 0x40;
  constexpr uint32_t kCmdIdxShift = 8;
  constexpr uint32_t kCmdIntCc = 0x0001;
  constexpr uint32_t kDatIntCc = 0x01;
  constexpr uint32_t kDatIntTrs = 0x01;

  const uint32_t opcode = command >> kCmdIdxShift;
  const bool wants_resp = command & (kCmdResp48 | kCmdResp136);
  const bool data_read = command & kCmdDataRead;
  const bool data_write = command & kCmdDataWrite;
  const SdCardId card_id = make_sdcard_id(sdcard_image.size());

  ocsdc_regs[OCSDC_BASE + 0x34] = 0;
  ocsdc_regs[OCSDC_BASE + 0x3c] = 0;
  ocsdc_data_pending = false;

  auto set_r1 = [&](uint32_t resp) {
    ocsdc_regs[OCSDC_BASE + 0x08] = resp;
    ocsdc_regs[OCSDC_BASE + 0x0c] = 0;
    ocsdc_regs[OCSDC_BASE + 0x10] = 0;
    ocsdc_regs[OCSDC_BASE + 0x14] = 0;
  };

  auto write_data_buffer = [&](const uint8_t *src, uint32_t len) {
    const uint32_t dst = ocsdc_read_reg(OCSDC_BASE + 0x60);
    check_mem_range_or_log("ocsdc dma read", dst, len);
    for (uint32_t i = 0; i < len; ++i) {
      const uint32_t addr = dst + i;
      const uint32_t word_addr = addr & ~0x3u;
      const uint32_t shift = (addr & 0x3u) * 8;
      uint32_t old_word = load_word(word_addr);
      old_word &= ~(0xffu << shift);
      old_word |= static_cast<uint32_t>(src[i]) << shift;
      store_word(word_addr, old_word);
    }
#ifdef ENABLE_SPIKE
    if (difftest_started && spike_ref) {
      spike_ref->sync_memory_range_from_dut(dst, len, memory, sdram_memory);
    }
#endif
  };

  switch (opcode) {
  case 0:
    set_r1(0);
    break;
  case 8:
    set_r1(argument);
    break;
  case 55:
    set_r1(0x20);
    break;
  case 41:
    set_r1(card_id.ocr);
    break;
  case 2:
    ocsdc_regs[OCSDC_BASE + 0x08] = card_id.cid[0];
    ocsdc_regs[OCSDC_BASE + 0x0c] = card_id.cid[1];
    ocsdc_regs[OCSDC_BASE + 0x10] = card_id.cid[2];
    ocsdc_regs[OCSDC_BASE + 0x14] = card_id.cid[3];
    break;
  case 3:
    set_r1(1u << 16);
    break;
  case 7:
    set_r1(0x00000900u);
    break;
  case 9:
    ocsdc_regs[OCSDC_BASE + 0x08] = card_id.csd[0];
    ocsdc_regs[OCSDC_BASE + 0x0c] = card_id.csd[1];
    ocsdc_regs[OCSDC_BASE + 0x10] = card_id.csd[2];
    ocsdc_regs[OCSDC_BASE + 0x14] = card_id.csd[3];
    break;
  case 13:
    set_r1(0x00000900u | (4u << 9) | (1u << 8));
    break;
  case 16:
    set_r1(0);
    break;
  case 17:
    set_r1(0);
    break;
  case 18:
    set_r1(0x00000900u);
    break;
  case 6:
    set_r1(0);
    break;
  default:
    set_r1(0);
    break;
  }

  if (kLogVerboseOcsdcData &&
      (opcode == 41 || opcode == 9 || opcode == 17 || opcode == 18)) {
    std::cerr << "[RefCPU][OCSDC] opcode=" << opcode << " arg=0x" << std::hex
              << argument << " resp0=0x" << ocsdc_regs[OCSDC_BASE + 0x08]
              << " resp1=0x" << ocsdc_regs[OCSDC_BASE + 0x0c]
              << " resp2=0x" << ocsdc_regs[OCSDC_BASE + 0x10]
              << " resp3=0x" << ocsdc_regs[OCSDC_BASE + 0x14] << std::dec
              << std::endl;
  }

  if (data_read || data_write) {
    const uint32_t dst = ocsdc_read_reg(OCSDC_BASE + 0x60);
    const uint32_t blk_size = (ocsdc_read_reg(OCSDC_BASE + 0x44) & 0xffffu) + 1u;
    const uint32_t blk_count = (ocsdc_read_reg(OCSDC_BASE + 0x48) & 0xffffu) + 1u;
    const uint64_t total_bytes =
        static_cast<uint64_t>(blk_size) * static_cast<uint64_t>(blk_count);
    const uint64_t card_offset =
        static_cast<uint64_t>(argument) * static_cast<uint64_t>(kSdSectorSize);

    if (kLogVerboseOcsdcData &&
        (opcode == 17 || opcode == 18 || opcode == 41 || opcode == 9)) {
      std::cerr << "[RefCPU][OCSDC] cmd=" << opcode << " arg=0x" << std::hex
                << argument << " blk_size=0x" << blk_size
                << " blk_count=0x" << blk_count << " card_offset=0x"
                << card_offset << std::dec << std::endl;
    }

    if (data_read) {
      if (opcode == 51 && blk_size == 8 && blk_count == 1) {
        // Linux mmc_app_send_scr() DMA-loads 8 raw bytes, then converts each
        // 32-bit lane with be32_to_cpu(). Encode SCR as:
        //   raw_scr[0] = 0x02050000 -> spec v2, 1-bit + 4-bit support
        //   raw_scr[1] = 0x00000000
        uint8_t scr[8] = {0x02, 0x05, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00};
        write_data_buffer(scr, sizeof(scr));
      } else if (opcode == 6 && blk_size == 64 && blk_count == 1) {
        uint32_t switch_status[16] = {};
        const bool do_switch = (argument >> 31) & 1u;
        switch_status[3] = 0x00020000u;
        switch_status[4] = do_switch ? 0x01000000u : 0x00000000u;
        switch_status[7] = 0x00000000u;
        write_data_buffer(reinterpret_cast<const uint8_t *>(switch_status),
                          sizeof(switch_status));
      } else {
        if (sdcard_image.empty()) {
          std::cerr << "[RefCPU] OCSDC read requested but no sdcard image loaded"
                    << std::endl;
          std::exit(1);
        }
        if (card_offset + total_bytes > sdcard_image.size()) {
          std::cerr << "[RefCPU] OCSDC read beyond sdcard image: offset=0x"
                    << std::hex << card_offset << " bytes=0x" << total_bytes
                    << " image_size=0x" << sdcard_image.size() << std::dec
                    << std::endl;
          std::exit(1);
        }
        write_data_buffer(sdcard_image.data() + card_offset,
                          static_cast<uint32_t>(total_bytes));
      }
      if (ref_ocsdc_probe_trace_enabled()) {
        std::cerr << "[REF][t=" << sim_time << "][OCSDC] dma-read opcode="
                  << std::dec << opcode << " arg=0x" << std::hex << argument
                  << " blk_size=0x" << blk_size << " blk_count=0x"
                  << blk_count << " dst=0x" << dst << " first=0x"
                  << load_word(dst & ~0x3u) << " bytes=0x" << total_bytes
                  << std::dec << std::endl;
      }
    }
    ocsdc_regs[OCSDC_BASE + 0x3c] = kDatIntCc | kDatIntTrs;
    ocsdc_data_pending = true;
    if (ocsdc_read_reg(OCSDC_BASE + 0x40) != 0) {
      xps_intc_set_irq_level(kOcsdcDatIrqId, true);
    }
  }

  if (wants_resp || opcode == 0) {
    ocsdc_regs[OCSDC_BASE + 0x34] = kCmdIntCc;
    if (ocsdc_read_reg(OCSDC_BASE + 0x38) != 0) {
      xps_intc_set_irq_level(kOcsdcCmdIrqId, true);
    }
  }
}

void Ref_cpu::ocsdc_write_reg(uint32_t word_addr, uint32_t data) {
  switch (word_addr - OCSDC_BASE) {
  case 0x28:
    ocsdc_regs[word_addr] = data;
    if (data & 1u)
      ocsdc_reset();
    break;
  case 0x34:
    ocsdc_regs[word_addr] = 0;
    xps_intc_set_irq_level(kOcsdcCmdIrqId, false);
    break;
  case 0x3c:
    ocsdc_regs[word_addr] = 0;
    xps_intc_set_irq_level(kOcsdcDatIrqId, false);
    break;
  case 0x38:
  case 0x40:
    ocsdc_regs[word_addr] = data;
    if (kLogVerboseOcsdcIrqEnable) {
      std::cerr << "[RefCPU][OCSDC] int_enable off=0x" << std::hex
                << (word_addr - OCSDC_BASE) << " data=0x" << data
                << std::dec << std::endl;
    }
    if ((word_addr - OCSDC_BASE) == 0x38) {
      if ((data != 0) && (ocsdc_read_reg(OCSDC_BASE + 0x34) != 0)) {
        xps_intc_set_irq_level(kOcsdcCmdIrqId, true);
      } else if (data == 0) {
        xps_intc_set_irq_level(kOcsdcCmdIrqId, false);
      }
    } else {
      if ((data != 0) && (ocsdc_read_reg(OCSDC_BASE + 0x3c) != 0)) {
        xps_intc_set_irq_level(kOcsdcDatIrqId, true);
      } else if (data == 0) {
        xps_intc_set_irq_level(kOcsdcDatIrqId, false);
      }
    }
    break;
  case 0x00:
    ocsdc_regs[word_addr] = data;
    ocsdc_execute_command(ocsdc_read_reg(OCSDC_BASE + 0x04), data);
    break;
  case 0x04:
    ocsdc_regs[word_addr] = data;
    break;
  default:
    ocsdc_regs[word_addr] = data;
    break;
  }
}

uint32_t Ref_cpu::checkpoint_mmio_read_word(uint32_t word_addr) const {
  word_addr &= ~0x3u;
  if (word_addr >= TIMER_BASE &&
      word_addr < (TIMER_BASE + TIMER_MMIO_SIZE)) {
    switch (word_addr - TIMER_BASE) {
    case 0x00:
      return static_cast<uint32_t>(ref_timer_now(sim_time));
    case 0x04:
      return static_cast<uint32_t>(ref_timer_now(sim_time) >> 32);
    case 0x08:
      return static_cast<uint32_t>(g_ref_timer_cmp);
    case 0x0c:
      return static_cast<uint32_t>(g_ref_timer_cmp >> 32);
    default:
      return 0;
    }
  }
  if (word_addr >= XPS_INTC_BASE &&
      word_addr < (XPS_INTC_BASE + XPS_INTC_MMIO_SIZE)) {
    return xps_intc_read_reg(word_addr);
  }
  if (word_addr >= OCSDC_BASE &&
      word_addr < (OCSDC_BASE + OCSDC_MMIO_SIZE)) {
    return ocsdc_read_reg(word_addr);
  }
  auto it = io_words.find(word_addr);
  return it == io_words.end() ? 0u : it->second;
}

void Ref_cpu::checkpoint_mmio_write_backing(uint32_t word_addr,
                                             uint32_t data) {
  word_addr &= ~0x3u;
  if (data == 0) {
    io_words.erase(word_addr);
  } else {
    io_words[word_addr] = data;
  }
}

void Ref_cpu::checkpoint_mmio_sync_devices() {
  const auto backing_word = [&](uint32_t addr) {
    auto it = io_words.find(addr & ~0x3u);
    return it == io_words.end() ? 0u : it->second;
  };

  const uint64_t timer_value =
      static_cast<uint64_t>(backing_word(TIMER_BASE)) |
      (static_cast<uint64_t>(backing_word(TIMER_BASE + 4u)) << 32);
  g_ref_timer_offset = timer_value - sim_time;
  g_ref_timer_cmp =
      static_cast<uint64_t>(backing_word(TIMER_BASE + 8u)) |
      (static_cast<uint64_t>(backing_word(TIMER_BASE + 12u)) << 32);

  xps_intc_isr = backing_word(XPS_INTC_BASE + 0x00u);
  xps_intc_ier = backing_word(XPS_INTC_BASE + 0x08u);
  xps_intc_mer = backing_word(XPS_INTC_BASE + 0x1cu);

  ocsdc_regs.clear();
  for (uint32_t off = 0; off + 4u <= OCSDC_MMIO_SIZE; off += 4u) {
    ocsdc_regs[OCSDC_BASE + off] = backing_word(OCSDC_BASE + off);
  }

  refresh_external_interrupt();
}

void Ref_cpu::exec(const SimConfig &config) {
  // Initialize Spike Reference Simulator if enabled
#ifdef ENABLE_SPIKE
  if (config.difftest) {
    spike_ref = std::make_unique<SpikeRef>("rv32imab_zfinx", 0x80000000, 0x80000000,
                                           config.image_file.c_str());
  }
#else
  if (config.difftest) {
    std::cerr << "[RefCPU] Difftest requested, but this build is compiled "
                 "without Spike support."
              << std::endl;
    exit(1);
  }
#endif

  // 准备 GEN_CHECKPOINT 需要的 target_intervals
  std::map<uint32_t, uint32_t> target_intervals;
  std::map<uint64_t, std::string> special_targets;
  uint32_t finished_intervals = 0;

  auto save_special_checkpoint = [&](const std::string &ckpt_name) {
    // The legacy interval field is a SimPoint warmup length.  It is unrelated
    // to an absolute special-checkpoint boundary; serializing the live U-mode
    // BBV counter here would make simulator CKPT mode prewarm past the saved
    // CPU state before starting the DUT.
    const uint64_t live_interval_inst_count = interval_inst_count;
    interval_inst_count = 0;
    save_checkpoint(ckpt_name);
    interval_inst_count = live_interval_inst_count;
  };

  if (config.mode == SimMode::GEN_CHECKPOINT) {
    target_intervals = load_simpoints(config.points_file);

    // [特殊处理] Interval 0 无法向前预热，只能直接保存
    // 如果 SimPoint 选中了 0，我们只能保存刚启动的状态
    if (target_intervals.count(0)) {
      uint32_t sp_id = target_intervals[0];
      std::string ckpt_name = config.checkpoint_dir + "/ckpt_sp" +
                              std::to_string(sp_id) +
                              "_target0_nowarmup.gz"; // 建议加上.gz后缀
      save_checkpoint(ckpt_name);
    }
  } else if (config.mode == SimMode::GEN_SPECIAL_CHECKPOINT) {
    special_targets = load_special_checkpoint_targets(config.points_file);
    if (special_targets.empty()) {
      std::cerr << "Error: no special checkpoint targets were loaded."
                << std::endl;
      exit(1);
    }
    auto zero = special_targets.find(0);
    if (zero != special_targets.end()) {
      const std::string ckpt_name =
          config.checkpoint_dir + "/ckpt_special_" +
          checkpoint_label(zero->second) + "_inst0.gz";
      save_special_checkpoint(ckpt_name);
      special_targets.erase(zero);
      if (special_targets.empty())
        sim_end = true;
    }
  } else if (config.mode == SimMode::GEN_BBV) {
    bbv_init_file(config.bbv_output_file.c_str());
  } else if (config.mode == SimMode::RESTORE) {
    restore_checkpoint(config.restore_file);
  }

  uint64_t restored_inst_count = 0;

  // --- 主循环 ---
  while (!sim_end && sim_time < MAX_SIM_TIME) {

    if (kLogProgressSimTime && sim_time % 100000000 == 0) {
      std::cout << "SimTime: " << sim_time
                << " | Intervals: " << finished_intervals << std::endl;
    }

    // 1. 执行指令
    RISCV();

    // If the simulation ended (e.g., ebreak), skip Difftest for this last step
    // because DUT and Spike handle termination differently.
    if (sim_end)
      break;

    // Difftest Step
    if (config.difftest
#ifdef ENABLE_SPIKE
        && spike_ref
#endif
    ) {
#ifdef ENABLE_SPIKE
      if (!difftest_started) {
        // DUT starts at 0x0 with some boot code.
        // Spike is configured to start at 0x80000000.
        // We wait until DUT reaches 0x80000000 to sync state and start
        // difftest.
        if (state.pc == 0x80000000) {
          spike_ref->sync_all_memory_from_dut(memory, ram_size, sdram_memory,
                                              SDRAM_SIZE);
          spike_ref->sync_state(state, privilege);
          difftest_started = true;
        }
      } else {
        processor_t *ref_core = spike_ref->sim->get_core(0);
        if (is_io || force_sync) {
          // Device/MMIO instructions and interrupt/CSR transitions are owned
          // by the DUT model.  Do not let Spike execute them independently;
          // synchronize the post-commit architectural state instead.
          spike_ref->sync_state(state, privilege);
        } else {
          spike_ref->suppress_async_interrupts();
          spike_ref->step(1);
          if (Instruction == INST_WFI) {
            // The DUT and RefCPU implement WFI as a retiring NOP.  Spike may
            // enter its internal WFI wait state after retiring the instruction;
            // clear that non-architectural state without synchronizing CPU
            // state so the next instruction can be checked normally.
            ref_core->clear_waiting_for_interrupt();
          }
          // Strict check: No automatic recovery. Mismatch = Failure.
          spike_ref->reg_check(state, privilege);
        }
      }
#endif
    }

    sim_time++;

    // Special checkpoints use the absolute functional-model instruction count,
    // independent of U-mode SimPoint interval accounting.  At sim_time == N,
    // exactly N instructions have completed, so the next instruction is a
    // deterministic boundary shared by generation and restore.
    if (config.mode == SimMode::GEN_SPECIAL_CHECKPOINT) {
      auto target = special_targets.find(sim_time);
      if (target != special_targets.end()) {
        const std::string ckpt_name =
            config.checkpoint_dir + "/ckpt_special_" +
            checkpoint_label(target->second) + "_inst" +
            std::to_string(target->first) + ".gz";
        std::cout << "Creating special checkpoint at absolute instruction "
                  << target->first << " (" << target->second << ")"
                  << std::endl;
        save_special_checkpoint(ckpt_name);
        special_targets.erase(target);
        if (special_targets.empty()) {
          std::cout << "All special checkpoints generated. Simulation "
                       "finished."
                    << std::endl;
          sim_end = true;
        }
      }
    }

    // 2. 计数逻辑 (保持和你生成 BBV 时一致，这对 SimPoint 对齐至关重要)
    if (privilege == RISCV_MODE_U && is_br) {
      if (config.mode == SimMode::GEN_BBV) {
        bbv_commit(); // 更新内存中的 bbv_counts
      }
      // 这里的 interval_inst_count 决定了 Interval 的边界
      interval_inst_count += current_bb_len;
      current_bb_len = 0;
    }

    // 3. 指令数限制检查
    if (config.max_insts > 0) {
      if (config.mode == SimMode::RESTORE) {
        restored_inst_count++;
        if (restored_inst_count >= config.max_insts) {
          std::cout << "Restore run finished (max_insts reached)." << std::endl;
          break;
        }
      } else if (sim_time >= config.max_insts) {
        std::cout << "Run finished (max_insts reached)." << std::endl;
        break;
      }
    }

    // 4. Interval 边界处理
    if (interval_inst_count >= INTERVAL_SIZE) {
      // 当前 Interval (finished_intervals) 刚刚结束
      finished_intervals++;

      // 即将开始的 Interval 编号
      // 例如：finished_intervals 变成 1823，说明我们要开始跑 Interval 1823 了
      uint32_t upcoming_interval_id = finished_intervals;

      // --- 模式分支处理 ---

      if (config.mode == SimMode::GEN_BBV) {
        std::cout << "Dump BBV at interval boundary: " << upcoming_interval_id
                  << std::endl;
        dump_bbv();
      } else if (config.mode == SimMode::GEN_CHECKPOINT) {
        // [Warmup 逻辑核心修改]
        // 我们不检查 upcoming_interval_id 是否是目标，
        // 而是检查 (upcoming_interval_id + 1) 是否是目标。
        // 如果是，说明当前这个 Interval (upcoming_interval_id) 就是那个 Warmup
        // 区间。 我们需要在进入 Warmup 区间之前保存 Checkpoint。

        uint32_t target_id = upcoming_interval_id + 1; // Lookahead (向前看一个)

        auto it = target_intervals.find(target_id);
        if (it != target_intervals.end()) {
          uint32_t sp_id = it->second;

          // 文件名命名建议：明确标出这是 target 谁的 warmup
          std::string ckpt_name = config.checkpoint_dir + "/ckpt_sp" +
                                  std::to_string(sp_id) + "_target" +
                                  std::to_string(target_id) +
                                  "_warmup1.gz"; // warmup1 代表预热长度为1

          std::cout << "Creating Warmup Checkpoint for Target " << target_id
                    << std::endl;
          save_checkpoint(ckpt_name);

          // 优化：如果最大的目标都已经生成过预热快照了，就可以退出了
          // 注意：target_intervals 是有序 map，rbegin() 是最大的 key
          if (!target_intervals.empty() &&
              target_id >= target_intervals.rbegin()->first) {
            std::cout << "All checkpoints generated. Simulation finished."
                      << std::endl;
            sim_end = true;
          }
        }
      }

      // 重置区间指令计数
      interval_inst_count = 0;
    }
  }

  if (sim_time >= MAX_SIM_TIME) {
    std::cerr << "Error: Simulation Timeout! (MAX_SIM_TIME = " << MAX_SIM_TIME
              << " reached)" << std::endl;
    exit(1);
  }

  // --- 结束清理 ---
  if (config.mode == SimMode::GEN_BBV) {
    if (bbv_file.is_open())
      bbv_file.close();
  }
}

void Ref_cpu::exception(uint32_t trap_val) {
  is_exception = true;
  uint32_t next_pc = state.pc + 4;

  // 重新获取当前状态（因为exec可能没传进来最新的）
  bool ecall = (Instruction == INST_ECALL);
  bool mret = (Instruction == INST_MRET);
  bool sret = (Instruction == INST_SRET);

  uint32_t mstatus = state.csr[csr_mstatus];
  uint32_t sstatus = state.csr[csr_sstatus];
  uint32_t medeleg = state.csr[csr_medeleg];
  uint32_t mtvec = state.csr[csr_mtvec];
  uint32_t stvec = state.csr[csr_stvec];

  // 再次计算 Trap 原因 (与 RISCV() 中逻辑一致，但这里是为了确定是用 MTrap 还是
  // STrap 处理)
  // 注意：为了代码复用，这里其实可以简化，但为了保持你原有逻辑结构：

  bool medeleg_U_ecall = (medeleg >> 8) & 1;
  bool medeleg_S_ecall = (medeleg >> 9) & 1;
  bool medeleg_page_fault_inst = (medeleg >> 12) & 1;
  bool medeleg_page_fault_load = (medeleg >> 13) & 1;
  bool medeleg_page_fault_store = (medeleg >> 15) & 1;

  // 这里直接复用成员变量里的中断状态 (假设RISCV函数刚跑完，状态是新的)
  // 如果不是，需要重新计算 M_software_interrupt 等

  bool MTrap =
      (M_software_interrupt) || (M_timer_interrupt) || (M_external_interrupt) ||
      ((privilege == 0) && !medeleg_U_ecall && ecall) ||
      (ecall && (privilege == 1) && !medeleg_S_ecall) ||
      (ecall && (privilege == 3)) ||
      (page_fault_inst && !medeleg_page_fault_inst) ||
      (page_fault_load && !medeleg_page_fault_load) ||
      (page_fault_store && !medeleg_page_fault_store) || illegal_exception;

  bool STrap = S_software_interrupt || S_timer_interrupt ||
               S_external_interrupt ||
               (ecall && (privilege == 0) && medeleg_U_ecall) ||
               (ecall && (privilege == 1) && medeleg_S_ecall) ||
               (page_fault_inst && medeleg_page_fault_inst) ||
               (page_fault_load && medeleg_page_fault_load) ||
               (page_fault_store && medeleg_page_fault_store);

  if (M_software_interrupt || M_timer_interrupt || M_external_interrupt ||
      S_software_interrupt || S_timer_interrupt || S_external_interrupt) {
    force_sync = true;
  }

  if (kLogUserSyscallSummary && privilege == RISCV_MODE_U && ecall) {
    const uint32_t sysno = state.gpr[17];
    if (sysno == 29 || sysno == 56 || sysno == 57 || sysno == 63 ||
        sysno == 64 || sysno == 66 || sysno == 221 || sysno == 222) {
      std::cerr << "[RefCPU][U-SYSCALL] nr=" << sysno << " pc=0x" << std::hex
                << state.pc << " a0=0x" << state.gpr[10] << " a1=0x"
                << state.gpr[11] << " a2=0x" << state.gpr[12] << " a3=0x"
                << state.gpr[13] << std::dec << std::endl;
    }
  }
  if (MTrap) {
    state.csr[csr_mepc] = state.pc;
    uint32_t cause = 0;

    // 计算 MCause
    bool is_interrupt =
        M_software_interrupt || M_timer_interrupt || M_external_interrupt;
    if (is_interrupt)
      cause |= (1u << 31);

    uint32_t exception_code = 0;
    if (M_software_interrupt)
      exception_code = 3;
    else if (M_timer_interrupt)
      exception_code = 7;
    else if (M_external_interrupt ||
             (ecall && privilege == 3 && !medeleg_U_ecall))
      exception_code = 11;
    else if (ecall && privilege == 0 && !medeleg_U_ecall)
      exception_code = 8;
    else if (ecall && privilege == 1 && !medeleg_S_ecall)
      exception_code = 9;
    else if (page_fault_inst && !medeleg_page_fault_inst)
      exception_code = 12;
    else if (page_fault_load && !medeleg_page_fault_load)
      exception_code = 13;
    else if (page_fault_store && !medeleg_page_fault_store)
      exception_code = 15;
    else if (illegal_exception)
      exception_code = 2;

    cause |= exception_code;
    state.csr[csr_mcause] = cause;
    if (kLogVerboseTimer && exception_code == 7) {
      std::cerr << "[RefCPU][TRAP] M-timer pc=0x" << std::hex << state.pc
                << " mtvec=0x" << mtvec << " mip=0x" << visible_mip()
                << " mie=0x" << state.csr[csr_mie]
                << " cmp=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }

    // 向量中断跳转
    if ((mtvec & 1) && (cause & (1u << 31))) {
      next_pc = (mtvec & 0xfffffffc) + 4 * (cause & 0x7fffffff);
    } else {
      next_pc =
          mtvec & 0xfffffffc; // 这里的MASK可能需要根据Spec确认，通常是清除低2位
    }

    // 更新 mstatus
    // MPP = privilege
    mstatus = (mstatus & ~MSTATUS_MPP) | ((privilege & 0x3) << 11);
    // MPIE = MIE
    if (mstatus & MSTATUS_MIE)
      mstatus |= MSTATUS_MPIE;
    else
      mstatus &= ~MSTATUS_MPIE;
    // MIE = 0
    mstatus &= ~MSTATUS_MIE;

    // 同步 sstatus (sstatus 是 mstatus 的影子)
    state.csr[csr_mstatus] = mstatus;
    state.csr[csr_sstatus] =
        mstatus & 0x800DE133; // sstatus is the supervisor-visible view of mstatus

    privilege = 3; // Machine Mode
    state.csr[csr_mtval] = trap_val;

  } else if (STrap) {
    state.csr[csr_sepc] = state.pc;
    uint32_t cause = 0;

    bool is_interrupt =
        S_software_interrupt || S_timer_interrupt || S_external_interrupt;
    if (is_interrupt)
      cause |= (1u << 31);

    uint32_t exception_code = 0;
    if (S_external_interrupt || (ecall && privilege == 1 && medeleg_S_ecall))
      exception_code = 9;
    else if (S_timer_interrupt)
      exception_code = 5;
    else if (ecall && privilege == 0 && medeleg_U_ecall)
      exception_code = 8;
    else if (S_software_interrupt)
      exception_code = 1;
    else if (page_fault_inst && medeleg_page_fault_inst)
      exception_code = 12;
    else if (page_fault_load && medeleg_page_fault_load)
      exception_code = 13;
    else if (page_fault_store && medeleg_page_fault_store)
      exception_code = 15;

    cause |= exception_code;
    state.csr[csr_scause] = cause;
    if (kLogUserTrapSummary && privilege == RISCV_MODE_U &&
        !S_software_interrupt && !S_timer_interrupt && !S_external_interrupt) {
      std::cerr << "[RefCPU][U-TRAP->S] pc=0x" << std::hex << state.pc
                << " cause=0x" << cause << " tval=0x" << trap_val
                << " sepc=0x" << state.csr[csr_sepc]
                << " satp=0x" << state.csr[csr_satp] << std::dec << std::endl;
    }
    if (kLogVerboseTimer && exception_code == 5) {
      std::cerr << "[RefCPU][TRAP] S-timer pc=0x" << std::hex << state.pc
                << " stvec=0x" << stvec << " mip=0x" << visible_mip()
                << " mie=0x" << state.csr[csr_mie]
                << " cmp=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }

    if ((stvec & 1) && (cause & (1u << 31))) {
      next_pc = (stvec & 0xfffffffc) + 4 * (cause & 0x7fffffff);
    } else {
      next_pc = stvec & 0xfffffffc;
    }

    // 更新 sstatus
    // SPP = privilege
    if (privilege == 1)
      sstatus |= MSTATUS_SPP;
    else
      sstatus &= ~MSTATUS_SPP;

    // SPIE = SIE
    if (sstatus & MSTATUS_SIE)
      sstatus |= MSTATUS_SPIE;
    else
      sstatus &= ~MSTATUS_SPIE;

    // SIE = 0
    sstatus &= ~MSTATUS_SIE;

    // 写回
    state.csr[csr_sstatus] = sstatus;
    // 更新 mstatus 中对应的位 (SPIE, SIE, SPP 在 mstatus 中也有对应位置)
    // 简单做法：读出改完的 sstatus，把对应位刷回 mstatus
    uint32_t mask = 0x800DE133;
    state.csr[csr_mstatus] =
        (state.csr[csr_mstatus] & ~mask) | (sstatus & mask);

    privilege = 1; // Supervisor Mode
    state.csr[csr_stval] = trap_val;

  } else if (mret) {
    // MIE = MPIE
    if (mstatus & MSTATUS_MPIE)
      mstatus |= MSTATUS_MIE;
    else
      mstatus &= ~MSTATUS_MIE;

    // Privilege = MPP
    privilege = GET_MPP(mstatus);

    // MPIE = 1
    mstatus |= MSTATUS_MPIE;
    // MPP = U (0)
    mstatus &= ~MSTATUS_MPP;

    state.csr[csr_mstatus] = mstatus;
    // 同步 sstatus
    state.csr[csr_sstatus] = mstatus & 0x800DE133;

    next_pc = state.csr[csr_mepc];
    if (kLogVerboseReturns &&
        (next_pc == 0xc033fc6c || next_pc == 0xc033fc70 ||
         next_pc == 0xc033fc74 || next_pc == 0xc0004f5c)) {
      std::cerr << "[RefCPU][RET] mret -> pc=0x" << std::hex << next_pc
                << " priv=" << std::dec << privilege << " mip=0x" << std::hex
                << visible_mip() << " mie=0x" << state.csr[csr_mie]
                << " mstatus=0x" << state.csr[csr_mstatus] << std::dec
                << std::endl;
    }

  } else if (sret) {
    // SIE = SPIE
    if (sstatus & MSTATUS_SPIE)
      sstatus |= MSTATUS_SIE;
    else
      sstatus &= ~MSTATUS_SIE;

    // Privilege = SPP
    privilege = GET_SPP(sstatus);

    // SPIE = 1
    sstatus |= MSTATUS_SPIE;
    // SPP = U (0)
    sstatus &= ~MSTATUS_SPP;

    state.csr[csr_sstatus] = sstatus;
    // 同步回 mstatus
    uint32_t mask = 0x800DE133;
    state.csr[csr_mstatus] =
        (state.csr[csr_mstatus] & ~mask) | (sstatus & mask);

    next_pc = state.csr[csr_sepc];
    if (kLogVerboseReturns) {
      std::cerr << "[RefCPU][RET] sret -> pc=0x" << std::hex << next_pc
                << " priv=" << std::dec << privilege << " mip=0x" << std::hex
                << visible_mip() << " mie=0x" << state.csr[csr_mie]
                << " sstatus=0x" << state.csr[csr_sstatus] << std::dec
                << std::endl;
    }
  }

  state.pc = next_pc;
}

void Ref_cpu::evaluate_interrupts(uint32_t mip_reg) {
  const uint32_t mstatus = state.csr[csr_mstatus];
  const uint32_t mie_reg = state.csr[csr_mie];
  const uint32_t mideleg = state.csr[csr_mideleg];
  const bool mstatus_mie = (mstatus & MSTATUS_MIE) != 0;
  const bool mstatus_sie = (mstatus & MSTATUS_SIE) != 0;

  M_software_interrupt = (mip_reg & MIP_MSIP) && (mie_reg & MIP_MSIP) &&
                         !(mideleg & MIP_MSIP) &&
                         (privilege < RISCV_MODE_M || mstatus_mie);
  M_timer_interrupt = (mip_reg & MIP_MTIP) && (mie_reg & MIP_MTIP) &&
                      !(mideleg & MIP_MTIP) &&
                      (privilege < RISCV_MODE_M || mstatus_mie);
  M_external_interrupt = (mip_reg & MIP_MEIP) && (mie_reg & MIP_MEIP) &&
                         !(mideleg & MIP_MEIP) &&
                         (privilege < RISCV_MODE_M || mstatus_mie);

  const bool s_irq_enable =
      privilege < RISCV_MODE_S ||
      (privilege == RISCV_MODE_S && mstatus_sie);
  S_software_interrupt =
      (((mip_reg & MIP_MSIP) && (mie_reg & MIP_MSIP) &&
        (mideleg & MIP_MSIP)) ||
       ((mip_reg & MIP_SSIP) && (mie_reg & MIP_SSIP))) &&
      privilege < RISCV_MODE_M && s_irq_enable;
  S_timer_interrupt =
      (((mip_reg & MIP_MTIP) && (mie_reg & MIP_MTIP) &&
        (mideleg & MIP_MTIP)) ||
       ((mip_reg & MIP_STIP) && (mie_reg & MIP_STIP))) &&
      privilege < RISCV_MODE_M && s_irq_enable;
  S_external_interrupt =
      (((mip_reg & MIP_MEIP) && (mie_reg & MIP_MEIP) &&
        (mideleg & MIP_MEIP)) ||
       ((mip_reg & MIP_SEIP) && (mie_reg & MIP_SEIP))) &&
      privilege < RISCV_MODE_M && s_irq_enable;
}

bool Ref_cpu::take_forced_interrupt(uint32_t cause, uint8_t target_privilege,
                                    uint32_t pending_snapshot) {
  constexpr uint32_t kInterruptBit = 1u << 31;
  constexpr uint32_t kPendingMask = 0x00000bbbu;
  if ((cause & kInterruptBit) == 0) {
    return false;
  }

  state.csr[csr_mip] = (state.csr[csr_mip] & ~kPendingMask) |
                       (pending_snapshot & kPendingMask);
  state.csr[csr_sip] = state.csr[csr_mip] & 0x00000333u;
  evaluate_interrupts(state.csr[csr_mip]);

  struct Candidate {
    bool active;
    uint8_t privilege;
    uint32_t code;
  };
  const Candidate candidates[] = {
      {M_software_interrupt, RISCV_MODE_M, 3u},
      {M_timer_interrupt, RISCV_MODE_M, 7u},
      {M_external_interrupt, RISCV_MODE_M, 11u},
      {S_external_interrupt, RISCV_MODE_S, 9u},
      {S_timer_interrupt, RISCV_MODE_S, 5u},
      {S_software_interrupt, RISCV_MODE_S, 1u},
  };

  const uint32_t requested_code = cause & ~kInterruptBit;
  const Candidate *selected = nullptr;
  for (const auto &candidate : candidates) {
    if (candidate.active) {
      selected = &candidate;
      break;
    }
  }
  if (selected == nullptr || selected->privilege != target_privilege ||
      selected->code != requested_code) {
    return false;
  }

  Instruction = 0;
  state.store = false;
  state.store_addr = state.store_data = state.store_strb = 0;
  page_fault_inst = page_fault_load = page_fault_store = false;
  illegal_exception = false;
  is_io = false;
  force_sync = false;
  exception(0);
  evaluate_interrupts(0);
  return true;
}

void Ref_cpu::RISCV() {
  sync_timer_csrs(state, sim_time,
                  device_effects_enable && interrupt_delivery_enable);
  if (privilege == RISCV_MODE_U) {
    if (current_bb_len == 0) {
      current_bb_head_pc = state.pc;
    }
    current_bb_len++;
  }

  is_csr = is_exception = is_br = br_taken = false;
  illegal_exception = page_fault_load = page_fault_inst = page_fault_store =
      asy = false;
  state.store = false;
  is_io = false;
  force_sync = false;

  uint32_t p_addr = state.pc;

  if ((state.csr[csr_satp] & 0x80000000) && privilege != 3) {
    page_fault_inst = !va2pa(p_addr, state.pc, 0);

    if (page_fault_inst) {
      exception(state.pc);
      return;
    }
  }
  check_mem_range_or_log("instruction fetch", p_addr, 4);
  Instruction = load_word(p_addr);

  if (kLogUserEntrySummary && privilege == RISCV_MODE_U &&
      (state.pc == 0x10000u || state.pc == 0x10428u)) {
    std::cerr << "[RefCPU][U-ENTRY] pc=0x" << std::hex << state.pc
              << " sp=0x" << state.gpr[2] << " ra=0x" << state.gpr[1]
              << " satp=0x" << state.csr[csr_satp] << std::dec << std::endl;
  }

  if (Instruction == INST_EBREAK) {
    uint32_t exit_code = state.gpr[10]; // a0
    if (kLogUserTrapSummary && privilege == RISCV_MODE_U) {
      std::cerr << "[RefCPU][U-EBREAK] pc=0x" << std::hex << state.pc
                << " a0=0x" << exit_code << " sp=0x" << state.gpr[2]
                << " ra=0x" << state.gpr[1] << std::dec << std::endl;
    }
    std::cout << "ebreak signal received. code = 0x" << std::hex << exit_code
              << std::dec << std::endl;
    std::cout << "total_sim_time: " << sim_time << std::endl;
    if (exit_code == 0) {
      std::cout << "\033[1;32mSuccess!\033[0m" << std::endl;
    } else {
      std::cout << "\033[1;31mTest Failed with code: " << exit_code << "\033[0m"
                << std::endl;
    }
    state.pc += 4;
    sim_end = true;
    return;
  }

  // === 优化 1: 极速解码 ===
  // 使用 BITS 宏直接提取字段，完全替代 bool 数组操作
  uint32_t opcode = BITS(Instruction, 6, 0);

  bool ecall = (Instruction == INST_ECALL);
  bool mret = (Instruction == INST_MRET);
  bool sret = (Instruction == INST_SRET);

  // === 优化 2: 快速读取 CSR 状态 ===
  uint32_t mie_reg = state.csr[csr_mie];
  uint32_t mip_reg = ref_effective_mip(state.csr[csr_mip], sim_time,
                                       device_effects_enable &&
                                           interrupt_delivery_enable);
  if (!interrupt_delivery_enable) {
    mip_reg = 0;
  }
  uint32_t medeleg = state.csr[csr_medeleg];

  // 异常委托位 (Exceptions)
  bool medeleg_U_ecall = (medeleg >> 8) & 1;
  bool medeleg_S_ecall = (medeleg >> 9) & 1;
  // bool medeleg_M_ecall = (medeleg >> 11) & 1; // 通常M-ecall不委托

  bool medeleg_page_fault_inst = (medeleg >> 12) & 1;
  bool medeleg_page_fault_load = (medeleg >> 13) & 1;
  bool medeleg_page_fault_store = (medeleg >> 15) & 1;

  evaluate_interrupts(mip_reg);

  // Trap 判断
  bool MTrap =
      M_software_interrupt || M_timer_interrupt || M_external_interrupt ||
      ((privilege == 0) && !medeleg_U_ecall && ecall) || // ecall from U
      ((privilege == 1) && !medeleg_S_ecall && ecall) || // ecall from S
      ((privilege == 3) && ecall) ||                     // ecall from M
      (page_fault_inst && !medeleg_page_fault_inst) || illegal_exception;

  bool STrap = S_software_interrupt || S_timer_interrupt ||
               S_external_interrupt ||
               ((privilege == 0) && medeleg_U_ecall && ecall) ||
               ((privilege == 1) && medeleg_S_ecall && ecall) ||
               (page_fault_inst && medeleg_page_fault_inst);

  asy = MTrap || STrap || mret || sret;

  if (page_fault_inst) {
    exception(state.pc);
    return;
  } else if (illegal_exception) {
    exception(Instruction);
    return;
  } else if (asy || Instruction == INST_ECALL) {
    exception(0);
    return;
  } else if (opcode == number_10_opcode_ecall) {
    // SYSTEM 指令 (CSR, WFI, MRET等)
    if (Instruction == INST_WFI) {
      is_csr = false;
    } else {
      is_csr = true;
    }
    RV32CSR();
  } else if (opcode == number_11_opcode_lrw) {
    RV32A();
  } else if (opcode == number_12_opcode_float ||
             opcode == number_13_opcode_fmadd ||
             opcode == number_14_opcode_fmsub ||
             opcode == number_15_opcode_fnmsub ||
             opcode == number_16_opcode_fnmadd) {
    RV32Zfinx();
  } else {
    RV32IM();
  }
  state.gpr[0] = 0;
}

void Ref_cpu::RV32CSR() {
  // pc + 4
  uint32_t next_pc = state.pc + 4;

  // 使用宏直接提取，无需 copy_indice
  uint32_t rd = BITS(Instruction, 11, 7);
  uint32_t rs1 = BITS(Instruction, 19, 15);
  uint32_t uimm = rs1; // 对于立即数CSR指令，rs1字段就是立即数
  uint32_t csr_addr = BITS(Instruction, 31, 20);
  uint32_t funct3 = BITS(Instruction, 14, 12);

  uint32_t reg_rdata1 = state.gpr[rs1];

  bool we = funct3 == 1 || rs1 != 0;
  bool re = funct3 != 1 || rd != 0;
  uint32_t wcmd = funct3 & 0b11;
  uint32_t csr_wdata = 0, wdata;

  if (funct3 & 0b100) {
    wdata = rs1;
  } else {
    wdata = reg_rdata1;
  }

  auto apply_csr_op = [&](uint32_t old_val, uint32_t operand) -> uint32_t {
    if (wcmd == CSR_W) {
      return operand;
    }
    if (wcmd == CSR_S) {
      return old_val | operand;
    }
    return old_val & ~operand;
  };

  if (funct3 == 0) {
    const bool is_sfence_vma =
        BITS(Instruction, 31, 25) == 0b0001001 && rd == 0;
    if (Instruction == INST_WFI || is_sfence_vma) {
      state.pc = next_pc;
      return;
    }
    illegal_exception = true;
    exception(Instruction);
    return;
  }

  if (csr_addr == number_fflags || csr_addr == number_frm ||
      csr_addr == number_fcsr) {
    uint32_t old_val = 0;
    if (csr_addr == number_fflags) {
      old_val = fcsr_fflags & 0x1Fu;
    } else if (csr_addr == number_frm) {
      old_val = fcsr_frm & 0x7u;
    } else {
      old_val = ((fcsr_frm & 0x7u) << 5) | (fcsr_fflags & 0x1Fu);
    }

    if (re) {
      state.gpr[rd] = old_val;
    }

    if (we) {
      uint32_t new_val = apply_csr_op(old_val, wdata);
      if (csr_addr == number_fflags) {
        fcsr_fflags = static_cast<uint8_t>(new_val & 0x1Fu);
      } else if (csr_addr == number_frm) {
        fcsr_frm = static_cast<uint8_t>(new_val & 0x7u);
      } else {
        fcsr_fflags = static_cast<uint8_t>(new_val & 0x1Fu);
        fcsr_frm = static_cast<uint8_t>((new_val >> 5) & 0x7u);
      }
    }
    state.pc = next_pc;
    return;
  }

  if (csr_addr != number_mtvec && csr_addr != number_mepc &&
      csr_addr != number_mcause && csr_addr != number_mie &&
      csr_addr != number_mip && csr_addr != number_mtval &&
      csr_addr != number_mscratch && csr_addr != number_mstatus &&
      csr_addr != number_mideleg && csr_addr != number_medeleg &&
      csr_addr != number_sepc && csr_addr != number_stvec &&
      csr_addr != number_scause && csr_addr != number_sscratch &&
      csr_addr != number_stval && csr_addr != number_sstatus &&
      csr_addr != number_sie && csr_addr != number_sip &&
      csr_addr != number_satp && csr_addr != number_mhartid &&
      csr_addr != number_misa) {
    illegal_exception = true;
    exception(Instruction);
    return;
  } else {

    int csr_idx = cvt_number_to_csr(csr_addr);
    const bool use_external_csr_read =
        external_csr_read.valid && external_csr_read.address == csr_addr;
    uint32_t observed_csr_value = 0;
    if (use_external_csr_read) {
      observed_csr_value = external_csr_read.value;
    } else if (csr_addr == number_mip) {
      observed_csr_value =
          ref_effective_mip(state.csr[csr_mip], sim_time,
                            device_effects_enable && interrupt_delivery_enable);
    } else if (csr_addr == number_sip) {
      observed_csr_value =
          ref_effective_mip(state.csr[csr_mip], sim_time,
                            device_effects_enable && interrupt_delivery_enable) &
          0x00000333u;
    } else {
      observed_csr_value = state.csr[csr_idx];
    }

    if (re) {
      state.gpr[rd] = observed_csr_value;
    }

    if (we) {
      uint32_t old_val = observed_csr_value;
      if (wcmd == CSR_W) {
        csr_wdata = wdata;
      } else if (wcmd == CSR_S) {
        csr_wdata = old_val | wdata;
      } else if (wcmd == CSR_C) {
        csr_wdata = old_val & ~wdata;
      }

      if (csr_idx == csr_mie || csr_idx == csr_sie) {
        uint32_t mie_mask =
            0x00000aaa; // MEI(11), SEI(9), MTI(7), STI(5), SSI(1)
        uint32_t sie_mask =
            0x00000333; // SEI(9), UEI(8), STI(5), UTI(4), SSI(1), USI(0)

        if (csr_idx == csr_sie) {
          // sie: 0x333 (Include User-Level Interrupt bits)
          state.csr[csr_mie] =
              (state.csr[csr_mie] & ~sie_mask) | (csr_wdata & sie_mask);
        } else {
          // mie: 0xbbb
          state.csr[csr_mie] = csr_wdata & mie_mask;
        }
        // sie 始终是 mie 的影子 (masked by 0x333)
        state.csr[csr_sie] = state.csr[csr_mie] & sie_mask;

      } else if (csr_idx == csr_mip || csr_idx == csr_sip) {
        uint32_t mip_mask = MIP_SEIP | MIP_STIP | MIP_SSIP;
        uint32_t sip_mask =
            0x00000333; // SEIP(9), UEIP(8), STIP(5), UTIP(4), SSIP(1), USIP(0)

        if (csr_idx == csr_sip) {
          const uint32_t writable =
              (privilege == RISCV_MODE_M) ? mip_mask : static_cast<uint32_t>(MIP_SSIP);
          state.csr[csr_mip] =
              (state.csr[csr_mip] & ~writable) | (csr_wdata & writable);
        } else {
          state.csr[csr_mip] =
              (state.csr[csr_mip] & ~mip_mask) | (csr_wdata & mip_mask);
        }
        force_sync = true;
        state.csr[csr_sip] =
            ref_effective_mip(state.csr[csr_mip], sim_time,
                              device_effects_enable &&
                                  interrupt_delivery_enable) &
            sip_mask;
        if (kLogVerboseCsrMip &&
            ((csr_wdata & (MIP_STIP | MIP_SSIP)) != 0 ||
             ((old_val ^ state.csr[csr_mip]) & (MIP_STIP | MIP_SSIP)) != 0)) {
          std::cerr << "[RefCPU][CSR] " << (csr_idx == csr_sip ? "sip" : "mip")
                    << " <- 0x" << std::hex << csr_wdata
                    << " old=0x" << old_val << " new_mip=0x"
                    << state.csr[csr_mip] << " new_sip=0x"
                    << state.csr[csr_sip] << " pc=0x" << state.pc << std::dec
                    << std::endl;
        }

      } else if (csr_idx == csr_mstatus || csr_idx == csr_sstatus) {
        uint32_t mstatus_mask = 0x807FF9BB; // ~0x7f800644
        uint32_t sstatus_mask = 0x800DE133;

        if (csr_idx == csr_sstatus) {
          // sstatus 写入：仅修改 mstatus 中属于 sstatus 掩码范围内的位
          state.csr[csr_mstatus] = (state.csr[csr_mstatus] & ~sstatus_mask) |
                                   (csr_wdata & sstatus_mask);
        } else {
          // mstatus 写入：应用 mstatus 写掩码
          state.csr[csr_mstatus] = (state.csr[csr_mstatus] & ~mstatus_mask) |
                                   (csr_wdata & mstatus_mask);
        }
        // 同步更新 sstatus 影子值
        state.csr[csr_sstatus] = state.csr[csr_mstatus] & sstatus_mask;

      } else {
        state.csr[csr_idx] = csr_wdata;
      }
    }
    if (use_external_csr_read) {
      external_csr_read = {};
    }
  }

  state.pc = next_pc;
}

void Ref_cpu::RV32Zfinx() {
  uint32_t next_pc = state.pc + 4;

  uint32_t opcode = Instruction & 0x7Fu;
  uint32_t rd = (Instruction >> 7) & 0x1Fu;
  uint32_t funct3 = (Instruction >> 12) & 0x7u;
  uint32_t rs1 = (Instruction >> 15) & 0x1Fu;
  uint32_t rs2 = (Instruction >> 20) & 0x1Fu;
  uint32_t funct7 = (Instruction >> 25) & 0x7Fu;
  uint32_t rs3 = (Instruction >> 27) & 0x1Fu;

  uint32_t val_rs1 = state.gpr[rs1];
  uint32_t val_rs2 = state.gpr[rs2];
  uint32_t val_rs3 = state.gpr[rs3];

  uint8_t rm = static_cast<uint8_t>(funct3);
  if (rm == 7) {
    rm = static_cast<uint8_t>(fcsr_frm & 0x7u);
  }

  switch (rm) {
  case 0:
    softfloat_roundingMode = softfloat_round_near_even;
    break;
  case 1:
    softfloat_roundingMode = softfloat_round_minMag;
    break;
  case 2:
    softfloat_roundingMode = softfloat_round_min;
    break;
  case 3:
    softfloat_roundingMode = softfloat_round_max;
    break;
  case 4:
    softfloat_roundingMode = softfloat_round_near_maxMag;
    break;
  default:
    illegal_exception = true;
    return;
  }

  softfloat_exceptionFlags = 0;

  float32_t f_rs1 = to_f32(val_rs1);
  float32_t f_rs2 = to_f32(val_rs2);
  float32_t f_rs3 = to_f32(val_rs3);
  float32_t f_res;
  uint32_t i_res = 0;
  bool update_fflags = true;

  switch (opcode) {
  case number_12_opcode_float:
    switch (funct7) {
    case 0x00: // FADD.S
      f_res = f32_add(f_rs1, f_rs2);
      i_res = from_f32(f_res);
      break;
    case 0x04: // FSUB.S
      f_res = f32_sub(f_rs1, f_rs2);
      i_res = from_f32(f_res);
      break;
    case 0x08: // FMUL.S
      f_res = f32_mul(f_rs1, f_rs2);
      i_res = from_f32(f_res);
      break;
    case 0x0C: // FDIV.S
      f_res = f32_div(f_rs1, f_rs2);
      i_res = from_f32(f_res);
      break;
    case 0x2C: // FSQRT.S
      if (rs2 != 0) {
        illegal_exception = true;
        return;
      }
      f_res = f32_sqrt(f_rs1);
      i_res = from_f32(f_res);
      break;
    case 0x10: // FSGNJ.S/FSGNJN.S/FSGNJX.S
      if (funct3 == 0) {
        i_res = (val_rs1 & ~0x80000000u) | (val_rs2 & 0x80000000u);
      } else if (funct3 == 1) {
        i_res = (val_rs1 & ~0x80000000u) | (~val_rs2 & 0x80000000u);
      } else if (funct3 == 2) {
        i_res = val_rs1 ^ (val_rs2 & 0x80000000u);
      } else {
        illegal_exception = true;
        return;
      }
      update_fflags = false;
      break;
    case 0x14: // FMIN.S/FMAX.S
      if (funct3 == 0) {
        i_res = f32_min_riscv(val_rs1, val_rs2);
      } else if (funct3 == 1) {
        i_res = f32_max_riscv(val_rs1, val_rs2);
      } else {
        illegal_exception = true;
        return;
      }
      break;
    case 0x50: // FEQ.S/FLT.S/FLE.S
      if (funct3 == 2) {
        i_res = f32_eq(f_rs1, f_rs2);
      } else if (funct3 == 1) {
        i_res = f32_lt(f_rs1, f_rs2);
      } else if (funct3 == 0) {
        i_res = f32_le(f_rs1, f_rs2);
      } else {
        illegal_exception = true;
        return;
      }
      break;
    case 0x60: // FCVT.W.S/FCVT.WU.S
      if (rs2 == 0) {
        i_res = static_cast<uint32_t>(
            f32_to_i32(f_rs1, softfloat_roundingMode, true));
      } else if (rs2 == 1) {
        i_res = f32_to_ui32(f_rs1, softfloat_roundingMode, true);
      } else {
        illegal_exception = true;
        return;
      }
      break;
    case 0x68: // FCVT.S.W/FCVT.S.WU
      if (rs2 == 0) {
        f_res = i32_to_f32(static_cast<int32_t>(val_rs1));
      } else if (rs2 == 1) {
        f_res = ui32_to_f32(val_rs1);
      } else {
        illegal_exception = true;
        return;
      }
      i_res = from_f32(f_res);
      break;
    case 0x70: // FCLASS.S
      if (funct3 == 1) {
        i_res = f32_classify_riscv(f_rs1);
        update_fflags = false;
      } else {
        illegal_exception = true;
        return;
      }
      break;
    default:
      illegal_exception = true;
      return;
    }
    break;
  case number_13_opcode_fmadd: // FMADD.S
    f_res = f32_mulAdd(f_rs1, f_rs2, f_rs3);
    i_res = from_f32(f_res);
    break;
  case number_14_opcode_fmsub: { // FMSUB.S
    float32_t f_neg_rs3 = to_f32(val_rs3 ^ 0x80000000u);
    f_res = f32_mulAdd(f_rs1, f_rs2, f_neg_rs3);
    i_res = from_f32(f_res);
    break;
  }
  case number_15_opcode_fnmsub: { // FNMSUB.S
    float32_t f_neg_rs1 = to_f32(val_rs1 ^ 0x80000000u);
    f_res = f32_mulAdd(f_neg_rs1, f_rs2, f_rs3);
    i_res = from_f32(f_res);
    break;
  }
  case number_16_opcode_fnmadd: { // FNMADD.S
    float32_t f_neg_rs1 = to_f32(val_rs1 ^ 0x80000000u);
    float32_t f_neg_rs3 = to_f32(val_rs3 ^ 0x80000000u);
    f_res = f32_mulAdd(f_neg_rs1, f_rs2, f_neg_rs3);
    i_res = from_f32(f_res);
    break;
  }
  default:
    illegal_exception = true;
    return;
  }

  if (update_fflags) {
    fcsr_fflags = static_cast<uint8_t>((fcsr_fflags | softfloat_exceptionFlags) &
                                       0x1Fu);
  }
  if (rd != 0) {
    state.gpr[rd] = i_res;
  }
  state.pc = next_pc;
}

void Ref_cpu::RV32A() {
  // pc + 4
  uint32_t next_pc = state.pc + 4;
  uint32_t funct5 = BITS(Instruction, 31, 27);
  uint32_t reg_d_index = BITS(Instruction, 11, 7);
  uint32_t reg_a_index = BITS(Instruction, 19, 15);
  uint32_t reg_b_index = BITS(Instruction, 24, 20);

  uint32_t reg_rdata1 = state.gpr[reg_a_index];
  uint32_t reg_rdata2 = state.gpr[reg_b_index];

  uint32_t v_addr = reg_rdata1;
  uint32_t p_addr = v_addr;

  if (p_addr % 4 != 0) {
    std::cerr << "Misaligned AMO Access! addr: 0x" << std::hex << v_addr
              << std::endl;
    exit(-1);
  }

  if ((state.csr[csr_satp] & 0x80000000) && privilege != 3) {
    bool page_fault_1 = !va2pa(p_addr, v_addr, 1);
    bool page_fault_2 = !va2pa(p_addr, v_addr, 2);

    if (page_fault_1 || page_fault_2) {
      if (funct5 == 2) {
        if (page_fault_1) {
          page_fault_load = true;
        }
      } else if (funct5 == 3) {
        if (page_fault_2) {
          page_fault_store = true;
        }
      } else {
        page_fault_store = true;
      }
    }

    if (page_fault_load || page_fault_store) {
      exception(v_addr);
      return;
    }
  }
  if (!is_ram_range(p_addr, 4)) {
    is_io = true;
  }
  check_mem_range_or_log("amo", p_addr, 4);

  if (funct5 != 2) {
    state.store = true;
    state.store_addr = p_addr;
    state.store_strb = 0b1111;
  }
  uint32_t old_word = load_word(p_addr);

  switch (funct5) {
  case 0: { // amoadd.w
    state.gpr[reg_d_index] = old_word;
    state.store_data = old_word + reg_rdata2;
    break;
  }
  case 1: { // amoswap.w
    state.gpr[reg_d_index] = old_word;
    state.store_data = reg_rdata2;
    break;
  }
  case 2: { // lr.w
    state.gpr[reg_d_index] = old_word;
    state.reserve_valid = true;
    state.reserve_addr = p_addr;
    break;
  }
  case 3: { // sc.w
    if (state.reserve_valid && state.reserve_addr == p_addr) {
      state.store_data = reg_rdata2;
      state.gpr[reg_d_index] = 0; // Success
    } else {
      state.gpr[reg_d_index] = 1; // Fail
      state.store = false;        // Don't perform the write
    }
    state.reserve_valid =
        false; // Regardless of success, invalidate reservation
    break;
  }
  case 4: { // amoxor.w
    state.gpr[reg_d_index] = old_word;
    state.store_data = old_word ^ reg_rdata2;
    break;
  }
  case 8: { // amoor.w
    state.gpr[reg_d_index] = old_word;
    state.store_data = old_word | reg_rdata2;
    break;
  }
  case 12: { // amoand.w
    state.gpr[reg_d_index] = old_word;
    state.store_data = old_word & reg_rdata2;
    break;
  }
  case 16: { // amomin.w
    state.gpr[reg_d_index] = old_word;
    state.store_data =
        ((int32_t)old_word > (int32_t)reg_rdata2) ? reg_rdata2 : old_word;
    break;
  }
  case 20: { // amomax.w
    state.gpr[reg_d_index] = old_word;
    state.store_data =
        ((int32_t)old_word > (int32_t)reg_rdata2) ? old_word : reg_rdata2;
    break;
  }
  case 24: { // amominu.w
    state.gpr[reg_d_index] = old_word;
    state.store_data =
        ((uint32_t)old_word < (uint32_t)reg_rdata2) ? old_word : reg_rdata2;
    break;
  }
  case 28: { // amomaxu.w
    state.gpr[reg_d_index] = old_word;
    state.store_data =
        ((uint32_t)old_word > (uint32_t)reg_rdata2) ? old_word : reg_rdata2;
    break;
  }
  default: {
    break;
  }
  }

  if (state.store) {
    store_data();
  }
  state.pc = next_pc;
}

void Ref_cpu::RV32IM() {
  // pc + 4
  uint32_t next_pc = state.pc + 4;
  uint32_t opcode = BITS(Instruction, 6, 0);
  uint32_t funct3 = BITS(Instruction, 14, 12);
  uint32_t funct7 = BITS(Instruction, 31, 25);
  uint32_t reg_d_index = BITS(Instruction, 11, 7);
  uint32_t reg_a_index = BITS(Instruction, 19, 15);
  uint32_t reg_b_index = BITS(Instruction, 24, 20);

  uint32_t reg_rdata1 = state.gpr[reg_a_index];
  uint32_t reg_rdata2 = state.gpr[reg_b_index];

  switch (opcode) {
  case number_0_opcode_lui: { // lui
    state.gpr[reg_d_index] = immU(Instruction);
    break;
  }
  case number_1_opcode_auipc: { // auipc
    bool bit_temp[32];
    state.gpr[reg_d_index] = immU(Instruction) + state.pc;
    break;
  }
  case number_2_opcode_jal: { // jal
    is_br = true;
    br_taken = true;
    next_pc = state.pc + immJ(Instruction);
    state.gpr[reg_d_index] = state.pc + 4;
    break;
  }
  case number_3_opcode_jalr: { // jalr
    is_br = true;
    br_taken = true;
    bool bit_temp[32];
    next_pc = (reg_rdata1 + immI(Instruction)) & 0xFFFFFFFC;
    state.gpr[reg_d_index] = state.pc + 4;
    break;
  }
  case number_4_opcode_beq: { // beq, bne, blt, bge, bltu, bgeu
    is_br = true;
    switch (funct3) {
    case 0: { // beq
      if (reg_rdata1 == reg_rdata2) {
        br_taken = true;
        next_pc = (state.pc + immB(Instruction));
      }
      break;
    }
    case 1: { // bne
      if (reg_rdata1 != reg_rdata2) {
        br_taken = true;
        next_pc = (state.pc + immB(Instruction));
      }
      break;
    }
    case 4: { // blt
      if ((int32_t)reg_rdata1 < (int32_t)reg_rdata2) {
        br_taken = true;
        next_pc = (state.pc + immB(Instruction));
      }
      break;
    }
    case 5: { // bge
      if ((int32_t)reg_rdata1 >= (int32_t)reg_rdata2) {
        br_taken = true;
        next_pc = (state.pc + immB(Instruction));
      }
      break;
    }
    case 6: { // bltu
      if ((uint32_t)reg_rdata1 < (uint32_t)reg_rdata2) {
        br_taken = true;
        next_pc = (state.pc + immB(Instruction));
      }
      break;
    }
    case 7: { // bgeu
      if ((uint32_t)reg_rdata1 >= (uint32_t)reg_rdata2) {
        br_taken = true;
        next_pc = (state.pc + immB(Instruction));
      }
      break;
    }
    }
    break;
  }
  case number_5_opcode_lb: { // lb, lh, lw, lbu, lhu
    uint32_t v_addr = reg_rdata1 + immI(Instruction);
    uint32_t p_addr = v_addr;

    uint32_t size = funct3 & 0b11;
    uint32_t access_size = (size == 0b00) ? 1 : ((size == 0b01) ? 2 : 4);
    if ((size == 0b01 && (v_addr % 2 != 0)) ||
        (size == 0b10 && (v_addr % 4 != 0))) {
      std::cerr << "Misaligned Load! addr: 0x" << std::hex << v_addr
                << std::endl;
      exit(-1);
    }

    if ((state.csr[csr_satp] & 0x80000000) && privilege != 3) {
      page_fault_load = !va2pa(p_addr, v_addr, 1);
    }

    if (!is_ram_range(p_addr, access_size)) {
      is_io = true;
    }

    if (page_fault_load) {
      exception(v_addr);
      return;

    } else {
      check_mem_range_or_log("load", p_addr, access_size);
      uint32_t data = load_word(p_addr);
      const uint32_t raw_word = data;
      uint32_t offset = p_addr & 0b11;
      uint32_t size = funct3 & 0b11;
      uint32_t sign = 0, mask;
      data = data >> (offset * 8);
      if (size == 0) {
        mask = 0xFF;
        if (data & 0x80)
          sign = 0xFFFFFF00;
      } else if (size == 0b01) {
        mask = 0xFFFF;
        if (data & 0x8000)
          sign = 0xFFFF0000;
      } else {
        mask = 0xFFFFFFFF;
      }

      data = data & mask;

      // 有符号数
      if (!(funct3 & 0b100)) {
        data = data | sign;
      }

      if (p_addr >= UART_BASE && p_addr < (UART_BASE + UART_MMIO_SIZE)) {
        trace_uart_load(sim_time, state.pc, p_addr, static_cast<uint8_t>(funct3),
                        raw_word, data);
      }

      if (device_effects_enable && p_addr == TIMER_BASE) {
        data = static_cast<uint32_t>(ref_timer_now(sim_time));
      }
      if (device_effects_enable && p_addr == (TIMER_BASE + 4u)) {
        data = static_cast<uint32_t>(ref_timer_now(sim_time) >> 32);
      }
      if (device_effects_enable && p_addr == (TIMER_BASE + 8u)) {
        data = static_cast<uint32_t>(g_ref_timer_cmp);
      }
      if (device_effects_enable && p_addr == (TIMER_BASE + 12u)) {
        data = static_cast<uint32_t>(g_ref_timer_cmp >> 32);
      }

      state.gpr[reg_d_index] = data;
    }
    break;
  }
  case number_6_opcode_sb: { // sb, sh, sw

    uint32_t v_addr = reg_rdata1 + immS(Instruction);
    uint32_t p_addr = v_addr;
    uint32_t access_size = (funct3 == 0b00) ? 1 : ((funct3 == 0b01) ? 2 : 4);

    if ((funct3 == 0b01 && (v_addr % 2 != 0)) ||
        (funct3 == 0b10 && (v_addr % 4 != 0))) {
      std::cerr << "Misaligned Store! addr: 0x" << std::hex << v_addr
                << std::endl;
      exit(-1);
    }

    if ((state.csr[csr_satp] & 0x80000000) && privilege != 3) {
      page_fault_store = !va2pa(p_addr, v_addr, 2);
    }

    if (!is_ram_range(p_addr, access_size)) {
      is_io = true;
    }

    if (page_fault_store) {
      exception(v_addr);
      return;
    } else {
      check_mem_range_or_log("store", p_addr, access_size);

      state.store = true;
      state.store_addr = p_addr;
      state.store_data = reg_rdata2;
      if (funct3 == 0b00) {
        state.store_strb = 0b1;
        state.store_data &= 0xFF;
      } else if (funct3 == 0b01) {
        state.store_strb = 0b11;
        state.store_data &= 0xFFFF;
      } else {
        state.store_strb = 0b1111;
      }

      store_data();
    }

    break;
  }
  case number_7_opcode_addi: { // addi, slti, sltiu, xori, ori, andi, slli,
                               // srli, srai, and Zbb/Zbs Immediates
    uint32_t imm = immI(Instruction);
    uint32_t shamt = imm & 0x1F;
    switch (funct3) {
    case 0: { // addi
      state.gpr[reg_d_index] = reg_rdata1 + imm;
      break;
    }
    case 2: { // slti
      state.gpr[reg_d_index] = (int32_t)reg_rdata1 < (int32_t)imm ? 1 : 0;
      break;
    }
    case 3: { // sltiu
      state.gpr[reg_d_index] = (uint32_t)reg_rdata1 < (uint32_t)imm ? 1 : 0;
      break;
    }
    case 4: { // xori
      state.gpr[reg_d_index] = reg_rdata1 ^ imm;
      break;
    }
    case 6: { // ori
      state.gpr[reg_d_index] = reg_rdata1 | imm;
      break;
    }
    case 7: { // andi
      state.gpr[reg_d_index] = reg_rdata1 & imm;
      break;
    }
    case 1: {            // slli, bseti, bclri, binvi, clz, ctz, pcnt, sext
      if (funct7 == 0) { // slli
        state.gpr[reg_d_index] = reg_rdata1 << shamt;
      } else if (funct7 == 0x30) { // Zbb Unary (clz, ctz, pcnt, sext)
        // For OP-IMM-Unary, rs2 field (shamt) is the differentiator
        // reg_b_index is extracted from bits 24:20 which IS the shamt field
        // position So checking reg_b_index is correct.
        uint32_t sub_op = reg_b_index;
        if (sub_op == 0) { // clz
          state.gpr[reg_d_index] =
              (reg_rdata1 == 0) ? 32 : __builtin_clz(reg_rdata1);
        } else if (sub_op == 1) { // ctz
          state.gpr[reg_d_index] =
              (reg_rdata1 == 0) ? 32 : __builtin_ctz(reg_rdata1);
        } else if (sub_op == 2) { // pcnt
          state.gpr[reg_d_index] = __builtin_popcount(reg_rdata1);
        } else if (sub_op == 4) { // sext.b
          int32_t byte_val = (int32_t)((int8_t)(reg_rdata1 & 0xFF));
          state.gpr[reg_d_index] = (uint32_t)byte_val;
        } else if (sub_op == 5) { // sext.h
          int32_t half_val = (int32_t)((int16_t)(reg_rdata1 & 0xFFFF));
          state.gpr[reg_d_index] = (uint32_t)half_val;
        }
      } else if (funct7 == 0x14) { // bseti (Zbs)
        state.gpr[reg_d_index] = reg_rdata1 | (1u << shamt);
      } else if (funct7 == 0x24) { // bclri (Zbs)
        state.gpr[reg_d_index] = reg_rdata1 & ~(1u << shamt);
      } else if (funct7 == 0x34) { // binvi (Zbs)
        state.gpr[reg_d_index] = reg_rdata1 ^ (1u << shamt);
      }
      break;
    }
    case 5: {            // srli, srai, rori, bexti, rev8, orcb
      if (funct7 == 0) { // srli
        state.gpr[reg_d_index] = (uint32_t)reg_rdata1 >> shamt;
      } else if (funct7 == 0x20) { // srai
        state.gpr[reg_d_index] = (int32_t)reg_rdata1 >> shamt;
      } else if (funct7 == 0x30) { // rori (Zbb)
        state.gpr[reg_d_index] =
            (reg_rdata1 >> shamt) | (reg_rdata1 << (32 - shamt));
      } else if (funct7 == 0x24) { // bexti (Zbs)
        state.gpr[reg_d_index] = (reg_rdata1 >> shamt) & 1;
      } else if (funct7 == 0x34) { // rev8 (Zbb) - shamt must be 24?
        // Spec says rev8 encoding is fixed.
        // But checking sub_op (shamt/rs2) is valid.
        // rev8: rs2=24 (11000).
        if (reg_b_index == 24) {
          uint32_t x = reg_rdata1;
          state.gpr[reg_d_index] = ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
                                   ((x & 0xFF0000) >> 8) |
                                   ((x & 0xFF000000) >> 24);
        }
      } else if (funct7 == 0x14) { // orcb (Zbb) - shamt must be 7?
        if (reg_b_index == 7) {
          uint32_t x = reg_rdata1;
          uint32_t res = 0;
          if (x & 0xFF)
            res |= 0xFF;
          if (x & 0xFF00)
            res |= 0xFF00;
          if (x & 0xFF0000)
            res |= 0xFF0000;
          if (x & 0xFF000000)
            res |= 0xFF000000;
          state.gpr[reg_d_index] = res;
        }
      }
      break;
    }
    }
    break;
  }
  case number_8_opcode_add: { // add, sub, sll, slt, sltu, xor, srl, sra, or,
                              // and
    if (funct7 == 1) {        // mul div
      int64_t s1 = (int64_t)(int32_t)reg_rdata1;
      int64_t s2 = (int64_t)(int32_t)reg_rdata2;

      uint64_t u1 = (uint32_t)reg_rdata1;
      uint64_t u2 = (uint32_t)reg_rdata2;

      // 获取 32 位操作数
      int32_t dividend = (int32_t)reg_rdata1;
      int32_t divisor = (int32_t)reg_rdata2;
      uint32_t u_dividend = (uint32_t)reg_rdata1;
      uint32_t u_divisor = (uint32_t)reg_rdata2;

      switch (funct3) {
      case 0: { // mul
        state.gpr[reg_d_index] = (int32_t)(u1 * u2);
        break;
      }
      case 1: { // mulh
        state.gpr[reg_d_index] = (uint32_t)((s1 * s2) >> 32);
        break;
      }
      case 2: { // mulsu
        state.gpr[reg_d_index] = (uint32_t)((s1 * (int64_t)u2) >> 32);
        break;
      }
      case 3: { // mulhu
        state.gpr[reg_d_index] = (uint32_t)((u1 * u2) >> 32);
        break;
      }
      case 4: { // div (signed)
        if (divisor == 0) {
          state.gpr[reg_d_index] = -1; // RISC-V 规定：除以0结果为 -1
        } else if (dividend == INT32_MIN && divisor == -1) {
          state.gpr[reg_d_index] =
              INT32_MIN; // RISC-V 规定：溢出时结果为被除数本身(INT_MIN)
        } else {
          state.gpr[reg_d_index] = dividend / divisor;
        }
        break;
      }
      case 5: { // divu (unsigned)
        if (u_divisor == 0) {
          state.gpr[reg_d_index] = 0xFFFFFFFF; // RISC-V 规定：除以0结果为最大值
        } else {
          state.gpr[reg_d_index] = u_dividend / u_divisor;
        }
        break;
      }
      case 6: { // rem (signed)
        if (divisor == 0) {
          state.gpr[reg_d_index] = dividend; // RISC-V 规定：除以0，余数为被除数
        } else if (dividend == INT32_MIN && divisor == -1) {
          state.gpr[reg_d_index] = 0; // RISC-V 规定：溢出时，余数为 0
        } else {
          state.gpr[reg_d_index] = dividend % divisor;
        }
        break;
      }
      case 7: { // remu (unsigned)
        if (u_divisor == 0) {
          state.gpr[reg_d_index] =
              u_dividend; // RISC-V 规定：除以0，余数为被除数
        } else {
          state.gpr[reg_d_index] = u_dividend % u_divisor;
        }
        break;
      }
      }
    } else {
      switch (funct3) {
      case 0: {            // add, sub
        if (funct7 == 0) { // add
          state.gpr[reg_d_index] = reg_rdata1 + reg_rdata2;
        } else if (funct7 == 0x20) { // sub
          state.gpr[reg_d_index] = reg_rdata1 - reg_rdata2;
        }
        break;
      }
      case 1: { // sll, rol, bclr, bset, binv, clmul
        uint32_t shift = reg_rdata2 & 0x1F;
        if (funct7 == 0) { // sll
          state.gpr[reg_d_index] = reg_rdata1 << shift;
        } else if (funct7 == 0x30) { // rol (Zbb)
          state.gpr[reg_d_index] =
              (reg_rdata1 << shift) | (reg_rdata1 >> (32 - shift));
        } else if (funct7 == 0x24) { // bclr (Zbs)
          state.gpr[reg_d_index] = reg_rdata1 & ~(1u << shift);
        } else if (funct7 == 0x14) { // bset (Zbs)
          state.gpr[reg_d_index] = reg_rdata1 | (1u << shift);
        } else if (funct7 == 0x34) { // binv (Zbs)
          state.gpr[reg_d_index] = reg_rdata1 ^ (1u << shift);
        } else if (funct7 == 0x05) { // clmul (Zbc)
          uint32_t output = 0;
          for (int i = 0; i < 32; i++) {
            if ((reg_rdata2 >> i) & 1)
              output ^= (reg_rdata1 << i);
          }
          state.gpr[reg_d_index] = output;
        }
        break;
      }
      case 2: {            // slt, sh1add, clmulr
        if (funct7 == 0) { // slt
          state.gpr[reg_d_index] =
              (int32_t)reg_rdata1 < (int32_t)reg_rdata2 ? 1 : 0;
        } else if (funct7 == 0x10) { // sh1add (Zba)
          state.gpr[reg_d_index] = reg_rdata2 + (reg_rdata1 << 1);
        } else if (funct7 == 0x05) { // clmulr (Zbc)
          uint32_t output = 0;
          for (int i = 0; i < 32; i++) {
            if ((reg_rdata2 >> i) & 1)
              output ^= (reg_rdata1 >> (31 - i));
          }
          state.gpr[reg_d_index] = output;
        }
        break;
      }
      case 3: {            // sltu, clmulh
        if (funct7 == 0) { // sltu
          state.gpr[reg_d_index] =
              (uint32_t)reg_rdata1 < (uint32_t)reg_rdata2 ? 1 : 0;
        } else if (funct7 == 0x05) { // clmulh (Zbc)
          uint32_t output = 0;
          for (int i = 1; i < 32; i++) {
            if ((reg_rdata2 >> i) & 1)
              output ^= (reg_rdata1 >> (32 - i));
          }
          state.gpr[reg_d_index] = output;
        }
        break;
      }
      case 4: {            // xor, xnor, min, pack, sh2add
        if (funct7 == 0) { // xor
          state.gpr[reg_d_index] = reg_rdata1 ^ reg_rdata2;
        } else if (funct7 == 0x20) { // xnor (Zbb)
          state.gpr[reg_d_index] = ~(reg_rdata1 ^ reg_rdata2);
        } else if (funct7 == 0x10) { // sh2add (Zba)
          state.gpr[reg_d_index] = reg_rdata2 + (reg_rdata1 << 2);
        } else if (funct7 == 0x05) { // min (Zbb)
          state.gpr[reg_d_index] = ((int32_t)reg_rdata1 < (int32_t)reg_rdata2)
                                       ? reg_rdata1
                                       : reg_rdata2;
        } else if (funct7 == 0x04) { // pack (Zbb)
          state.gpr[reg_d_index] =
              (reg_rdata1 & 0x0000FFFF) | (reg_rdata2 << 16);
        }
        break;
      }
      case 5: { // srl, sra, ror, bext, minu
        uint32_t shift = reg_rdata2 & 0x1F;
        if (funct7 == 0) { // srl
          state.gpr[reg_d_index] = (uint32_t)reg_rdata1 >> shift;
        } else if (funct7 == 0x20) { // sra
          state.gpr[reg_d_index] = (int32_t)reg_rdata1 >> shift;
        } else if (funct7 == 0x30) { // ror (Zbb)
          state.gpr[reg_d_index] =
              (reg_rdata1 >> shift) | (reg_rdata1 << (32 - shift));
        } else if (funct7 == 0x24) { // bext (Zbs)
          state.gpr[reg_d_index] = (reg_rdata1 >> shift) & 1;
        } else if (funct7 == 0x05) { // minu (Zbb)
          state.gpr[reg_d_index] = ((uint32_t)reg_rdata1 < (uint32_t)reg_rdata2)
                                       ? reg_rdata1
                                       : reg_rdata2;
        }
        break;
      }
      case 6: {            // or, orn, max, sh3add
        if (funct7 == 0) { // or
          state.gpr[reg_d_index] = reg_rdata1 | reg_rdata2;
        } else if (funct7 == 0x20) { // orn (Zbb)
          state.gpr[reg_d_index] = reg_rdata1 | (~reg_rdata2);
        } else if (funct7 == 0x05) { // max (Zbb)
          state.gpr[reg_d_index] = ((int32_t)reg_rdata1 > (int32_t)reg_rdata2)
                                       ? reg_rdata1
                                       : reg_rdata2;
        } else if (funct7 == 0x10) { // sh3add (Zba)
          state.gpr[reg_d_index] = reg_rdata2 + (reg_rdata1 << 3);
        }
        break;
      }
      case 7: {            // and, andn, maxu, packh
        if (funct7 == 0) { // and
          state.gpr[reg_d_index] = reg_rdata1 & reg_rdata2;
        } else if (funct7 == 0x20) { // andn (Zbb)
          state.gpr[reg_d_index] = reg_rdata1 & (~reg_rdata2);
        } else if (funct7 == 0x05) { // maxu (Zbb)
          state.gpr[reg_d_index] = ((uint32_t)reg_rdata1 > (uint32_t)reg_rdata2)
                                       ? reg_rdata1
                                       : reg_rdata2;
        } else if (funct7 == 0x04) { // packh (Zbb)
          state.gpr[reg_d_index] =
              (reg_rdata1 & 0x000000FF) | ((reg_rdata2 & 0x000000FF) << 8);
        }
        break;
      }
      }
    }
    break;
  }
  case number_9_opcode_fence: { // fence, fence.i
    break;
  }
  default: {
    break;
  }
  }

  state.pc = next_pc;
}

uint32_t Ref_cpu::load_word(uint32_t addr) const {
  const uint32_t word_addr = addr & ~0x3u;
  if (is_sdram_range(word_addr, 4)) {
    return sdram_memory[(word_addr - kSdramBase) >> 2];
  }
  if (is_ddr_range(word_addr, 4)) {
    return memory[(word_addr - kRamBase) >> 2];
  }

  check_mem_range_or_log("word load", word_addr, 4);
  if (word_addr >= UART_BASE && word_addr < (UART_BASE + UART_MMIO_SIZE)) {
    auto it = io_words.find(word_addr);
    if (it == io_words.end()) {
      return 0;
    }
    return it->second;
  }
  if (word_addr >= XPS_INTC_BASE &&
      word_addr < (XPS_INTC_BASE + XPS_INTC_MMIO_SIZE)) {
    return xps_intc_read_reg(word_addr);
  }
  if (word_addr >= OCSDC_BASE && word_addr < (OCSDC_BASE + OCSDC_MMIO_SIZE)) {
    return ocsdc_read_reg(word_addr);
  }
  auto it = io_words.find(word_addr);
  return (it == io_words.end()) ? 0 : it->second;
}

void Ref_cpu::store_word(uint32_t addr, uint32_t data) {
  const uint32_t word_addr = addr & ~0x3u;
  if (device_effects_enable && word_addr == TIMER_BASE) {
    const uint64_t now = sim_time;
    const uint64_t current = ref_timer_now(now);
    ref_write_timer_value((current & 0xFFFFFFFF00000000ull) |
                              static_cast<uint64_t>(data),
                          now);
    return;
  }
  if (device_effects_enable && word_addr == (TIMER_BASE + 4u)) {
    const uint64_t now = sim_time;
    const uint64_t current = ref_timer_now(now);
    ref_write_timer_value((static_cast<uint64_t>(data) << 32) |
                              (current & 0x00000000FFFFFFFFull),
                          now);
    return;
  }
  if (device_effects_enable && word_addr == (TIMER_BASE + 8u)) {
    g_ref_timer_cmp = (g_ref_timer_cmp & 0xFFFFFFFF00000000ull) |
                      static_cast<uint64_t>(data);
    if (kLogVerboseTimer) {
      std::cerr << "[RefCPU][TIMER] mtimecmp_lo <- 0x" << std::hex << data
                << " full=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }
    return;
  }
  if (device_effects_enable && word_addr == (TIMER_BASE + 12u)) {
    g_ref_timer_cmp = (static_cast<uint64_t>(data) << 32) |
                      (g_ref_timer_cmp & 0x00000000FFFFFFFFull);
    if (kLogVerboseTimer) {
      std::cerr << "[RefCPU][TIMER] mtimecmp_hi <- 0x" << std::hex << data
                << " full=0x" << g_ref_timer_cmp << " now=0x"
                << ref_timer_now(sim_time) << std::dec
                << std::endl;
    }
    return;
  }
  if (is_sdram_range(word_addr, 4)) {
    sdram_memory[(word_addr - kSdramBase) >> 2] = data;
    return;
  }
  if (is_ddr_range(word_addr, 4)) {
    memory[(word_addr - kRamBase) >> 2] = data;
    return;
  }

  check_mem_range_or_log("word store", word_addr, 4);
  if (word_addr >= UART_BASE && word_addr < (UART_BASE + UART_MMIO_SIZE)) {
    io_words[word_addr] = data;
    return;
  }
  if (word_addr >= XPS_INTC_BASE &&
      word_addr < (XPS_INTC_BASE + XPS_INTC_MMIO_SIZE)) {
    xps_intc_write_reg(word_addr, data);
    return;
  }
  if (word_addr >= OCSDC_BASE && word_addr < (OCSDC_BASE + OCSDC_MMIO_SIZE)) {
    ocsdc_write_reg(word_addr, data);
    return;
  }
  io_words[word_addr] = data;
}

void Ref_cpu::store_data() {
  uint32_t p_addr = state.store_addr;
  uint32_t write_size =
      (state.store_strb == 0b1) ? 1 : ((state.store_strb == 0b11) ? 2 : 4);
  check_mem_range_or_log("store_data", p_addr, write_size);
  if (state.store && state.reserve_valid && state.reserve_addr == p_addr) {
    // Keep exported state semantics aligned with the old in-tree refcpu.
    state.reserve_valid = false;
  }
  int offset = p_addr & 0x3;
  uint32_t wstrb = state.store_strb << offset;
  uint32_t wdata = state.store_data << (offset * 8);
  const bool trace_uart =
      p_addr >= UART_BASE && p_addr < (UART_BASE + UART_MMIO_SIZE);
  const uint32_t trace_uart_pc = state.pc;
  const uint32_t trace_uart_before0 =
      trace_uart ? load_word(UART_BASE) : 0u;
  uint32_t old_data = load_word(p_addr);
  uint32_t mask = 0;

  if (wstrb & 0b1)
    mask |= 0xFF;
  if (wstrb & 0b10)
    mask |= 0xFF00;
  if (wstrb & 0b100)
    mask |= 0xFF0000;
  if (wstrb & 0b1000)
    mask |= 0xFF000000;

  const bool suppress_external_device_store =
      !device_effects_enable && !is_ram_range(p_addr & ~0x3u, 4);
  if (state.store && !suppress_external_device_store) {
    store_word(p_addr, (mask & wdata) | (~mask & old_data));
  }

  if (device_effects_enable && p_addr >= UART_BASE &&
      p_addr < (UART_BASE + UART_MMIO_SIZE)) {
    const uint32_t reg = p_addr - UART_BASE;
    const uint8_t value =
        static_cast<uint8_t>((wdata >> (offset * 8)) & 0xffu);
    const uint8_t lcr = uart_read_reg8(io_words, UART_BASE + kUartRegLcr);
    const bool dlab = (lcr & kUartLcrDlab) != 0;

    if (reg == kUartRegRxTxDll && !dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegRxTxDll, 0);
      uart_write_reg8(io_words, UART_BASE + kUartRegLsr,
                      kUartLsrThre | kUartLsrTemt);
      if (uart_print && privilege != RISCV_MODE_U &&
          !g_disable_uart_console_output) {
        const char ch = static_cast<char>(value);
        std::cout << ch << std::flush;
        if (observe_uart_console_byte(sim_time, ch)) {
          sim_end = true;
        }
      }
      uart_refresh_interrupt();
    } else if (reg == kUartRegRxTxDll && dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegRxTxDll, value);
    } else if (reg == kUartRegIerDlm && dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegIerDlm, value);
    } else if (reg == kUartRegIerDlm && !dlab) {
      uart_write_reg8(io_words, UART_BASE + kUartRegIerDlm, value);
      uart_refresh_interrupt();
    } else if (reg == kUartRegIirFcr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegIirFcr,
                      static_cast<uint8_t>(kUartIirNoInt |
                                           kUartIirFifoEnabled));
      uart_refresh_interrupt();
    } else if (reg == kUartRegLcr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegLcr, value);
    } else if (reg == kUartRegMcr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegMcr, value);
    } else if (reg == kUartRegScr) {
      uart_write_reg8(io_words, UART_BASE + kUartRegScr, value);
    }
  }

  if (trace_uart) {
    trace_uart_store_effect(sim_time, trace_uart_pc, p_addr,
                            static_cast<uint8_t>(wstrb), wdata,
                            trace_uart_before0, load_word(UART_BASE),
                            load_word(UART_BASE + 4u));
  }

  state.store_data = state.store_data << offset * 8;
  state.store_strb = state.store_strb << offset * 8;
}

bool Ref_cpu::va2pa(uint32_t &p_addr, uint32_t v_addr, uint32_t type) {
  uint32_t mstatus = state.csr[csr_mstatus];
  uint32_t sstatus = state.csr[csr_sstatus];
  uint32_t satp = state.csr[csr_satp];

  // 1. 提取状态位 (直接位运算，极快)
  bool mxr = (mstatus & MSTATUS_MXR) != 0;
  bool sum = (mstatus & MSTATUS_SUM) != 0;
  bool mprv = (mstatus & MSTATUS_MPRV) != 0;

  // 确定有效特权级 (Effective Privilege Mode)
  // 如果 MPRV=1 且不是取指(type!=0)，则使用 MPP 作为特权级进行检查
  int eff_priv = privilege;
  if (type != 0 && mprv) {
    eff_priv = (mstatus >> MSTATUS_MPP_SHIFT) & 0x3;
  }

  // 2. Level 1 Page Table Walk
  // satp 的 PPN 字段在 SV32 中是低 22 位 (0-21)
  // VPN[1] 是 v_addr 的 [31:22] 位
  // pte1_addr = (satp.ppn << 12) + (vpn1 * 4)
  // 你的原代码逻辑：(satp << 12) | ((v_addr >> 20) & 0xFFC)
  // 等价于下面的位操作：
  uint32_t ppn_root = satp & 0x3FFFFF; // 提取 SATP 中的 PPN
  uint32_t vpn1 = (v_addr >> 22) & 0x3FF;
  uint32_t pte1_addr = (ppn_root << 12) | (vpn1 << 2);

  // 直接读取，注意这里需要确保 memory 是按字寻址还是字节寻址
  check_mem_range_or_log("ptw-l1", pte1_addr, 4);
  uint32_t pte1 = load_word(pte1_addr);

  // 3. 检查 PTE 有效性
  // !V 或者 (!R && W) 都是无效的
  if (!(pte1 & PTE_V) || (!(pte1 & PTE_R) && (pte1 & PTE_W))) {
    return false;
  }

  // 4. 判断是否是叶子节点 (R=1 或 X=1)
  if ((pte1 & PTE_R) || (pte1 & PTE_X)) {
    // --- Superpage (4MB) ---

    // 权限检查 (Permission Check)
    // Fetch (0): 需要 X
    if (type == 0 && !(pte1 & PTE_X))
      return false;
    // Load (1): 需要 R，或者 (MXR=1 且 X=1)
    if (type == 1 && !(pte1 & PTE_R) && !(mxr && (pte1 & PTE_X)))
      return false;
    // Store (2): 需要 W
    if (type == 2 && !(pte1 & PTE_W))
      return false;

    // 用户权限检查 (User/Supervisor Check)
    bool is_user_page = (pte1 & PTE_U) != 0;
    if (eff_priv == 0 && !is_user_page)
      return false; // U-mode 访问 S-page -> Fault
    if (eff_priv == 1 && is_user_page && !sum)
      return false; // S-mode 访问 U-page 且 SUM=0 -> Fault

    // 对齐检查 (Superpage 要求 PPN[0] 为 0)
    // PPN[0] 对应 PTE 的 [19:10] 位
    if ((pte1 >> 10) & 0x3FF)
      return false;

    // A/D 位检查
    if (!(pte1 & PTE_A))
      return false; // Accessed 必须为 1 (硬件不自动设置时需报错)
    if (type == 2 && !(pte1 & PTE_D))
      return false; // 写操作 Dirty 必须为 1

    // 计算物理地址 (Superpage)
    // PA = PPN[1] | VPN[0] | Offset
    // PPN[1] 是 PTE[31:20]，对应 PA[31:22]
    // v_addr & 0x3FFFFF 保留低 22 位 (VPN[0] + Offset)
    p_addr = ((pte1 << 2) & 0xFFC00000) | (v_addr & 0x3FFFFF);
    return true;
  }

  // 5. Level 2 Page Table Walk (非叶子节点，指向下一级页表)
  // PPN 是 PTE 的 [31:10] 位
  uint32_t ppn1 = (pte1 >> 10) & 0x3FFFFF;
  uint32_t vpn0 = (v_addr >> 12) & 0x3FF;
  uint32_t pte2_addr = (ppn1 << 12) | (vpn0 << 2);

  check_mem_range_or_log("ptw-l2", pte2_addr, 4);
  uint32_t pte2 = load_word(pte2_addr);

  // 重复有效性检查
  if (!(pte2 & PTE_V) || (!(pte2 & PTE_R) && (pte2 & PTE_W))) {
    return false;
  }

  // Level 2 必须是叶子节点 (SV32 只有两级)
  if ((pte2 & PTE_R) || (pte2 & PTE_X)) {
    // --- 4KB Page ---

    // 权限检查 (逻辑同上)
    if (type == 0 && !(pte2 & PTE_X))
      return false;
    if (type == 1 && !(pte2 & PTE_R) && !(mxr && (pte2 & PTE_X)))
      return false;
    if (type == 2 && !(pte2 & PTE_W))
      return false;

    // 用户权限检查
    bool is_user_page = (pte2 & PTE_U) != 0;
    if (eff_priv == 0 && !is_user_page)
      return false;
    if (eff_priv == 1 && is_user_page && !sum)
      return false;

    // A/D 位检查
    if (!(pte2 & PTE_A))
      return false;
    if (type == 2 && !(pte2 & PTE_D))
      return false;

    // 计算物理地址 (4KB Page)
    // PA = PPN | Offset
    // PPN 是 PTE[31:10]，对应 PA[31:12]
    // Offset 是 v_addr[11:0]
    p_addr = ((pte2 >> 10) << 12) | (v_addr & 0xFFF);
    return true;
  }

  return false; // 如果 Level 2 还不是叶子节点，则是非法页表
}
