#ifdef ENABLE_SPIKE
#include "spike_ref.h"
#endif
#include "CSR.h"
#include "RISCV.h"
#include "ref.h"
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
constexpr uint32_t kRamBase = 0x80000000u;
constexpr uint32_t kRamUpperBound = 0xC0000000u;
constexpr uint32_t kRamSizeBytes = kRamUpperBound - kRamBase;
constexpr uint32_t kBootIoBase = 0x00000000u;
constexpr uint32_t kBootIoSize = 0x00002000u;

[[noreturn]] void mem_oob_fatal(const char *op, uint32_t addr, uint32_t size) {
  std::cerr << "[RefCPU] illegal " << op << " at paddr=0x" << std::hex << addr
            << " size=0x" << size << " (not in RAM or implemented MMIO)"
            << std::dec << std::endl;
  std::exit(1);
}

inline bool is_ram_range(uint32_t addr, uint32_t size) {
  if (size == 0 || addr < kRamBase) {
    return false;
  }
  const uint64_t end =
      static_cast<uint64_t>(addr) + static_cast<uint64_t>(size) - 1;
  return end < kRamUpperBound;
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
         in_range(PLIC_BASE, PLIC_MMIO_SIZE) ||
         in_range(XPS_INTC_BASE, XPS_INTC_MMIO_SIZE) ||
         in_range(TIMER_BASE, TIMER_MMIO_SIZE);
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

std::map<uint32_t, uint32_t> load_simpoints(const std::string &filename);

Ref_cpu::~Ref_cpu() {
  if (memory) {
    free(memory);
  }
}

void Ref_cpu::init(uint32_t reset_pc, const char *image, uint32_t size) {
  state.pc = reset_pc;
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
  io_words.clear();

  std::ifstream inst_data(image, std::ios::in | std::ios::binary);
  if (!inst_data.is_open()) {
    std::cerr << "Error: Image " << (image ? image : "NULL")
              << " does not exist" << std::endl;
    exit(1);
  }

  inst_data.seekg(0, std::ios::end);
  std::streamsize img_size = inst_data.tellg();
  inst_data.seekg(0, std::ios::beg);

  if (img_size < 0 || static_cast<uint64_t>(img_size) > kRamSizeBytes) {
    std::cerr << "[RefCPU] Image too large for 1GB RAM window: " << img_size
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

  store_word(0x10000004, 0x00006000); // 和进入 OpenSBI 相关
  store_word(0x0, 0xf1402573);
  store_word(0x4, 0x83e005b7);
  store_word(0x8, 0x800002b7);
  store_word(0xc, 0x00028067);

  inst_data.close();

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
  difftest_started = false;
  sim_end = false;
  uart_print = false;
  ref_only = false;
  dut_pf_check_enable = true;
  dut_expect_pf_inst = false;
  dut_expect_pf_load = false;
  dut_expect_pf_store = false;
}

void Ref_cpu::exec(const SimConfig &config) {
  // Initialize Spike Reference Simulator if enabled
#ifdef ENABLE_SPIKE
  if (config.difftest) {
    spike_ref = std::make_unique<SpikeRef>("rv32imab_zfinx", 0x80000000, 0x40000000,
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
  uint32_t finished_intervals = 0;

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
  } else if (config.mode == SimMode::GEN_BBV) {
    bbv_init_file(config.bbv_output_file.c_str());
  } else if (config.mode == SimMode::RESTORE) {
    restore_checkpoint(config.restore_file);
  }

  uint64_t restored_inst_count = 0;

  // --- 主循环 ---
  while (!sim_end && sim_time < MAX_SIM_TIME) {

    if (sim_time % 100000000 == 0) {
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
          spike_ref->sync_state(state, privilege);
          difftest_started = true;
        }
      } else {
        processor_t *ref_core = spike_ref->sim->get_core(0);
        state_t *ref_state_before = ref_core->get_state();
        uint32_t ref_pc_before = static_cast<uint32_t>(ref_state_before->pc);
        uint32_t ref_ra_before = static_cast<uint32_t>(ref_state_before->XPR[1]);
        uint32_t ref_sp_before = static_cast<uint32_t>(ref_state_before->XPR[2]);
        uint32_t dut_pc_after = state.pc;
        uint32_t dut_insn_last = Instruction;

        auto fetch_ref_insn = [&](uint32_t pc) -> uint32_t {
          uint32_t val = 0;
          try {
            val = ref_core->get_mmu()->load_insn(pc).insn.bits();
            return val;
          } catch (...) {
          }
          uint32_t phys_pc = pc;
          if (pc >= 0xc0000000) {
            phys_pc = pc - 0xc0000000 + 0x80000000;
          }
          if (phys_pc >= 0x80000000 &&
              (phys_pc - 0x80000000 + 4) <= spike_ref->main_mem_ptr->size()) {
            uint8_t buf[4];
            if (spike_ref->main_mem_ptr->load(phys_pc - 0x80000000, 4, buf)) {
              val = static_cast<uint32_t>(buf[0]) |
                    (static_cast<uint32_t>(buf[1]) << 8) |
                    (static_cast<uint32_t>(buf[2]) << 16) |
                    (static_cast<uint32_t>(buf[3]) << 24);
            }
          }
          return val;
        };
        uint32_t ref_insn_before = fetch_ref_insn(ref_pc_before);

        spike_ref->step(1);

        state_t *ref_state_after = ref_core->get_state();
        uint32_t ref_pc_after = static_cast<uint32_t>(ref_state_after->pc);
        uint32_t ref_mcause = static_cast<uint32_t>(ref_core->get_csr(0x342));
        uint32_t ref_mepc = static_cast<uint32_t>(ref_core->get_csr(0x341));
        uint32_t ref_mtvec = static_cast<uint32_t>(ref_core->get_csr(0x305));

        static bool printed_first_ref_fault = false;
        if (!printed_first_ref_fault && (ref_pc_after == 0 || ref_mcause != 0)) {
          printed_first_ref_fault = true;
          uint32_t ref_insn_after = fetch_ref_insn(ref_pc_after);
          std::cout << "\n[DiffDebug] Spike abnormal step detected\n"
                    << "  DUT after-step: pc=0x" << std::hex << dut_pc_after
                    << " last_insn=0x" << dut_insn_last << "\n"
                    << "  Spike before step: pc=0x" << ref_pc_before
                    << " insn=0x" << ref_insn_before << " ra=0x" << ref_ra_before
                    << " sp=0x" << ref_sp_before << "\n"
                    << "  Spike after  step: pc=0x" << ref_pc_after
                    << " insn=0x" << ref_insn_after << "\n"
                    << "  Spike csrs: mepc=0x" << ref_mepc
                    << " mcause=0x" << ref_mcause << " mtvec=0x" << ref_mtvec
                    << std::dec << std::endl;

          const uint32_t op = ref_insn_before & 0x7Fu;
          if (op == 0x03) { // load
            const uint32_t rs1 = (ref_insn_before >> 15) & 0x1Fu;
            int32_t imm = static_cast<int32_t>(ref_insn_before) >> 20;
            uint32_t base = static_cast<uint32_t>(ref_state_before->XPR[rs1]);
            uint32_t vaddr = static_cast<uint32_t>(base + imm);
            std::cout << "  Decoded load: rs1=x" << rs1 << " base=0x" << std::hex
                      << base << " imm=" << std::dec << imm << " vaddr=0x"
                      << std::hex << vaddr << std::dec << std::endl;
          } else if (op == 0x23) { // store
            const uint32_t rs1 = (ref_insn_before >> 15) & 0x1Fu;
            int32_t imm = static_cast<int32_t>(((ref_insn_before >> 25) << 5) |
                                               ((ref_insn_before >> 7) & 0x1F));
            if (imm & 0x800) {
              imm |= ~0xFFF;
            }
            uint32_t base = static_cast<uint32_t>(ref_state_before->XPR[rs1]);
            uint32_t vaddr = static_cast<uint32_t>(base + imm);
            std::cout << "  Decoded store: rs1=x" << rs1 << " base=0x" << std::hex
                      << base << " imm=" << std::dec << imm << " vaddr=0x"
                      << std::hex << vaddr << std::dec << std::endl;
          }
        }

        if (is_io) {
          // Surgical sync of the destination register after I/O read
          spike_ref->sync_reg_from_dut(io_reg_idx, state.gpr[io_reg_idx]);
        } else if (force_sync) {
          // Sync full architectural state on interrupts or CSR writes
          spike_ref->sync_state(state, privilege);
        } else {
          // Strict check: No automatic recovery. Mismatch = Failure.
          spike_ref->reg_check(state, privilege);
        }
      }
#endif
    }

    sim_time++;

    // 2. 计数逻辑 (保持和你生成 BBV 时一致，这对 SimPoint 对齐至关重要)
    if (privilege == RISCV_MODE_U && is_br) {
      if (config.mode == SimMode::GEN_BBV) {
        bbv_commit(); // 更新内存中的 bbv_counts
      }
      // 这里的 interval_inst_count 决定了 Interval 的边界
      interval_inst_count += current_bb_len;
      current_bb_len = 0;
    }

    // 3. RESTORE 模式的指令数限制检查 (用于跑 Warmup + Sampling)
    if (config.mode == SimMode::RESTORE && config.max_insts > 0) {
      restored_inst_count++;
      if (restored_inst_count >= config.max_insts) {
        std::cout << "Restore run finished (max_insts reached)." << std::endl;
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
        mstatus & 0x800DE762; // 这是一个Mask，简单起见可以直接赋值

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
  }

  state.pc = next_pc;
}

void Ref_cpu::RISCV() {
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

  if (Instruction == INST_EBREAK) {
    uint32_t exit_code = state.gpr[10]; // a0
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
  uint32_t mstatus = state.csr[csr_mstatus];
  uint32_t mie_reg = state.csr[csr_mie];
  uint32_t mip_reg = state.csr[csr_mip];
  uint32_t mideleg = state.csr[csr_mideleg];
  uint32_t medeleg = state.csr[csr_medeleg];

  // 提取关键位
  bool mstatus_mie = (mstatus & MSTATUS_MIE) != 0;
  bool mstatus_sie = (mstatus & MSTATUS_SIE) != 0;

  // 异常委托位 (Exceptions)
  bool medeleg_U_ecall = (medeleg >> 8) & 1;
  bool medeleg_S_ecall = (medeleg >> 9) & 1;
  // bool medeleg_M_ecall = (medeleg >> 11) & 1; // 通常M-ecall不委托

  bool medeleg_page_fault_inst = (medeleg >> 12) & 1;
  bool medeleg_page_fault_load = (medeleg >> 13) & 1;
  bool medeleg_page_fault_store = (medeleg >> 15) & 1;

  // === 优化 3: 中断判断逻辑 (位运算) ===
  // M-mode 中断条件:Pending & Enabled & NotDelegated & (CurrentPriv < M ||
  // MIE=1)

  // Software Interrupts
  M_software_interrupt = (mip_reg & MIP_MSIP) && (mie_reg & MIP_MSIP) &&
                         !(mideleg & MIP_MSIP) &&
                         (privilege < 3 || mstatus_mie);

  // Timer Interrupts
  M_timer_interrupt = (mip_reg & MIP_MTIP) && (mie_reg & MIP_MTIP) &&
                      !(mideleg & MIP_MTIP) && (privilege < 3 || mstatus_mie);

  // External Interrupts
  M_external_interrupt = (mip_reg & MIP_MEIP) && (mie_reg & MIP_MEIP) &&
                         !(mideleg & MIP_MEIP) &&
                         (privilege < 3 || mstatus_mie);

  // S-mode 中断条件: Pending & Enabled & Delegated & (CurrentPriv < S || SIE=1)
  // 注意：privilege < 2 (S-mode=1, U-mode=0) 意味着当前是 U 或 S
  bool s_irq_enable = (privilege < 1 || (privilege == 1 && mstatus_sie));

  S_software_interrupt =
      (((mip_reg & MIP_MSIP) && (mie_reg & MIP_MSIP) && (mideleg & MIP_MSIP)) ||
       ((mip_reg & MIP_SSIP) && (mie_reg & MIP_SSIP))) &&
      (privilege < 2 && s_irq_enable);

  S_timer_interrupt =
      (((mip_reg & MIP_MTIP) && (mie_reg & MIP_MTIP) && (mideleg & MIP_MTIP)) ||
       ((mip_reg & MIP_STIP) && (mie_reg & MIP_STIP))) &&
      (privilege < 2 && s_irq_enable);

  S_external_interrupt =
      (((mip_reg & MIP_MEIP) && (mie_reg & MIP_MEIP) && (mideleg & MIP_MEIP)) ||
       ((mip_reg & MIP_SEIP) && (mie_reg & MIP_SEIP))) &&
      (privilege < 2 && s_irq_enable);

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

  // WFI 检查 (简单处理)
  if (Instruction == INST_WFI && !asy && !page_fault_inst && !page_fault_load &&
      !page_fault_store) {
    std::cout << "wfi encountered. stopping simulation at sim_time="
              << sim_time << std::endl;
    state.pc += 4;
    sim_end = true;
    return;
  }

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
      csr_addr != number_misa && csr_addr != number_time &&
      csr_addr != number_timeh) {
    ;
  } else if (csr_addr == number_time || csr_addr == number_timeh) {
    illegal_exception = true;
    exception(Instruction);
    return;
  } else {

    int csr_idx = cvt_number_to_csr(csr_addr);
    if (re) {
      state.gpr[rd] = state.csr[csr_idx];
    }

    if (we) {
      uint32_t old_val = state.csr[csr_idx];
      if (wcmd == CSR_W) {
        csr_wdata = wdata;
      } else if (wcmd == CSR_S) {
        csr_wdata = old_val | wdata;
      } else if (wcmd == CSR_C) {
        csr_wdata = old_val & ~wdata;
      }

      if (csr_idx == csr_mie || csr_idx == csr_sie) {
        uint32_t mie_mask =
            0x00000bbb; // MEI(11), SEI(9), MTI(7), STI(5), MSI(3), SSI(1)
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
        uint32_t mip_mask =
            0x00000bbb; // MEIP(11), SEIP(9), MTIP(7), STIP(5), MSIP(3), SSIP(1)
        uint32_t sip_mask =
            0x00000333; // SEIP(9), UEIP(8), STIP(5), UTIP(4), SSIP(1), USIP(0)

        if (csr_idx == csr_sip) {
          // sip: 0x333 (Include User-Level Interrupt bits)
          state.csr[csr_mip] =
              (state.csr[csr_mip] & ~sip_mask) | (csr_wdata & sip_mask);
        } else {
          state.csr[csr_mip] = csr_wdata & mip_mask;
        }
        force_sync = true;
        // sip 始终是 mip 的影子 (masked by 0x333)
        state.csr[csr_sip] = state.csr[csr_mip] & sip_mask;

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
    
    if (p_addr < 0x80000000) {
      is_io = true;
      io_reg_idx = reg_d_index;
    }

    if (page_fault_load) {
      exception(v_addr);
      return;

    } else {
      check_mem_range_or_log("load", p_addr, access_size);
      uint32_t data = load_word(p_addr);
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

      if (p_addr == 0x1fd0e000) {
        data = sim_time;
      }
      if (p_addr == 0x1fd0e004) {
        data = 0;
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
  if (is_ram_range(word_addr, 4)) {
    return memory[(word_addr - kRamBase) >> 2];
  }

  check_mem_range_or_log("word load", word_addr, 4);
  auto it = io_words.find(word_addr);
  return (it == io_words.end()) ? 0 : it->second;
}

void Ref_cpu::store_word(uint32_t addr, uint32_t data) {
  const uint32_t word_addr = addr & ~0x3u;
  if (is_ram_range(word_addr, 4)) {
    memory[(word_addr - kRamBase) >> 2] = data;
    return;
  }

  check_mem_range_or_log("word store", word_addr, 4);
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

  if (state.store) {
    store_word(p_addr, (mask & wdata) | (~mask & old_data));
  }

  if (p_addr == UART_BASE) {
    char temp;
    temp = wdata & 0x000000ff;
    store_word(0x10000000, load_word(0x10000000) & 0xffffff00);
    if (uart_print) {
      std::cout << temp;
    }
  }

  if (p_addr == 0x10000001 && (state.store_data & 0x000000ff) == 7) {
    store_word(0xc201004, 0xa);
    store_word(0x10000000, load_word(0x10000000) & 0xfff0ffff);

    state.csr[csr_mip] = state.csr[csr_mip] | (1 << 9);
    state.csr[csr_sip] = state.csr[csr_sip] | (1 << 9);
    force_sync = true;
  }

  if (p_addr == 0x10000001 && (state.store_data & 0x000000ff) == 5) {
    store_word(0x10000000, (load_word(0x10000000) & 0xfff0ffff) | 0x00030000);
  }

  if (p_addr == 0xc201004 && (state.store_data & 0x000000ff) == 0xa) {
    store_word(0xc201004, 0x0);
    state.csr[csr_mip] = state.csr[csr_mip] & ~(1 << 9);
    state.csr[csr_sip] = state.csr[csr_sip] & ~(1 << 9);
    force_sync = true;
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
