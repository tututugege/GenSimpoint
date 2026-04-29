#pragma once

#include "fesvr/memif.h"
#include "riscv/cfg.h"
#include "riscv/devices.h"
#include "riscv/log_file.h"
#include "riscv/mmu.h"
#include "riscv/processor.h"
#include "riscv/sim.h"

#include "ref.h"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

// Memory for Spike, isolated from DUT to prevent corruption
class SpikeMem : public mem_t {
public:
  SpikeMem(size_t s) : mem_t(s) {}
  // No special store/load to keep it simple and safe
};

class SpikeRef {
public:
  std::unique_ptr<sim_t> sim;
  std::unique_ptr<cfg_t> cfg;
  // We don't store mems vector here to avoid confusion with sim_t's internal
  // copy But we need to keep track of the pointers if we want to access them
  // directly
  abstract_mem_t *main_mem_ptr;
  abstract_mem_t *plic_mem_ptr;
  abstract_mem_t *uart_mem_ptr;
  abstract_mem_t *timer_mem_ptr;
  abstract_mem_t *boot_mem_ptr;

  SpikeRef(const char *isa, reg_t ram_base, reg_t ram_size,
           const char *image_file) {
    auto align_up_4k = [](reg_t size) -> reg_t {
      const reg_t kPage = 4096;
      return (size + kPage - 1) & ~(kPage - 1);
    };

    std::cout << "[SpikeRef] Initializing with ISA=" << isa << " Base=0x"
              << std::hex << ram_base << std::dec << " Size=0x" << std::hex
              << ram_size << std::dec << std::endl;

    cfg = std::make_unique<cfg_t>();
    cfg->isa = isa;
    cfg->initrd_bounds = std::make_pair(0, 0);
    cfg->bootargs = nullptr;
    cfg->priv = "MSU";
    cfg->endianness = endianness_little;
    cfg->mem_layout.push_back(mem_cfg_t(ram_base, ram_size));
    cfg->hartids.push_back(0);
    cfg->start_pc = 0x80000000;

    main_mem_ptr = new SpikeMem(ram_size);
    plic_mem_ptr = new mem_t(align_up_4k(PLIC_MMIO_SIZE)); // PLIC area from DTB
    uart_mem_ptr = new mem_t(align_up_4k(UART_MMIO_SIZE)); // UART area from DTB
    timer_mem_ptr = new mem_t(align_up_4k(0x1000)); // Timer area
    boot_mem_ptr = new mem_t(0x1000); // Boot ROM at 0x1000

    // Initialize Boot ROM at 0x1000
    uint32_t boot_code[] = {
        0x00000297, // 0x1000: auipc t0,0
        0x02828613, // 0x1004: addi a2,t0,40
        0xf1402573, // 0x1008: csrrs a0,mhartid,zero
        0x0202a583, // 0x100c: lw a1,32(t0)
        0x0182a283, // 0x1010: lw t0,24(t0)
        0x00028067, // 0x1014: jr              t0
        0x80000000, // 0x1018:
        0x00000000, // 0x101c: padding
        0x8fe00000  // 0x1020:
    };
    boot_mem_ptr->store(0, sizeof(boot_code), (const uint8_t *)boot_code);

    // Initialize 0x10000004 to 0x00006000 for OpenSBI/Linux compatibility
    uint32_t opensbi_val = 0x00006000;
    uart_mem_ptr->store(0x10000004 - UART_BASE, 4,
                        (const uint8_t *)&opensbi_val);

    std::vector<std::pair<reg_t, abstract_mem_t *>> mems;
    mems.push_back(std::make_pair(ram_base, main_mem_ptr));
    mems.push_back(std::make_pair(PLIC_BASE, plic_mem_ptr));
    mems.push_back(std::make_pair(UART_BASE, uart_mem_ptr));
    mems.push_back(std::make_pair(TIMER_BASE, timer_mem_ptr));
    mems.push_back(std::make_pair(0x1000, boot_mem_ptr));

    std::vector<std::string> args;
    args.push_back("spike");

    std::vector<device_factory_sargs_t> plugin_device_factories;
    debug_module_config_t dm_config;

    std::cout << "[SpikeRef] Creating sim_t with ISA string: " << isa
              << std::endl;
    try {
      sim = std::make_unique<sim_t>(
          cfg.get(), false, mems, plugin_device_factories, args, dm_config,
          nullptr, false, nullptr, false, nullptr, std::nullopt);
      std::cout << "[SpikeRef] sim_t created successfully (Isolated Memory)."
                << std::endl;
    } catch (const std::exception &e) {
      std::cerr << "[SpikeRef] Exception creating sim_t: " << e.what()
                << std::endl;
      exit(1);
    }

    // Manually load image into memory
    std::ifstream file(image_file, std::ios::binary | std::ios::ate);
    if (file) {
      std::streamsize fsize = file.tellg();
      file.seekg(0, std::ios::beg);
      std::vector<char> buffer(fsize);
      if (file.read(buffer.data(), fsize)) {
        main_mem_ptr->store(0, fsize, (const uint8_t *)buffer.data());
        std::cout << "[SpikeRef] Loaded " << fsize << " bytes into memory at 0x"
                  << std::hex << ram_base << std::dec << std::endl;
      }
    }

    sim->get_core(0)->get_state()->pc = 0x80000000;
    sim->get_core(0)->set_max_vaddr_bits(32);

    processor_t *p = sim->get_core(0);
    p->put_csr(0x3B0, 0xFFFFFFFF); // pmpaddr0
    p->put_csr(0x3A0, 0x0F);       // pmpcfg0
  }

  ~SpikeRef() {
    // sim_t destructor will delete main_mem_ptr and uart_shadow_ptr
  }

  void step(uint64_t n) {
    try {
      sim->get_core(0)->step(n);
    } catch (const std::exception &e) {
      std::cerr << "[SpikeRef] Exception during step: " << e.what()
                << std::endl;
      exit(1);
    } catch (...) {
      std::cerr << "[SpikeRef] Unknown exception during step" << std::endl;
      exit(1);
    }
  }

  bool reg_check(const CPU_state &dut_state, uint8_t dut_priv) {
    processor_t *p = sim->get_core(0);
    state_t *s = p->get_state();

    bool pc_mismatch = (uint32_t)s->pc != (uint32_t)dut_state.pc;
    bool reg_mismatch = false;
    bool mismatches[32] = {false};

    for (int i = 0; i < 32; ++i) {
      if ((uint32_t)s->XPR[i] != (uint32_t)dut_state.gpr[i]) {
        reg_mismatch = true;
        mismatches[i] = true;
      }
    }

    // Surgical CSR Check (the 21 implemented CSRs)
    static const uint32_t implemented_csr_addrs[21] = {
        0x305, 0x341, 0x342, 0x304, 0x344, 0x343, 0x340,
        0x300, 0x303, 0x302, 0x141, 0x105, 0x142, 0x140,
        0x143, 0x100, 0x104, 0x144, 0x180, 0xf14, 0x301};
    bool csr_mismatch = false;
    bool csr_mismatches[21] = {false};
    uint32_t ref_csr_vals[21];

    for (int i = 0; i < 21; ++i) {
      ref_csr_vals[i] = (uint32_t)p->get_csr(implemented_csr_addrs[i]);
      if (ref_csr_vals[i] != dut_state.csr[i]) {
        // Special case for mstatus/sstatus where Spike might have different
        // SD/UXL bits For now, let's report all mismatches to be strict.
        csr_mismatch = true;
        csr_mismatches[i] = true;
      }
    }

    if (pc_mismatch || reg_mismatch || csr_mismatch) {
      // Fetch instructions for context
      uint32_t ref_insn = 0;
      uint32_t dut_insn = 0;
      uint32_t ref_pc_val = (uint32_t)s->pc;
      uint32_t dut_pc_val = (uint32_t)dut_state.pc;

      auto fetch_insn = [&](uint32_t pc) {
        uint32_t val = 0;
        try {
          // Use Spike's MMU to fetch instruction, handling virtual addresses
          val = p->get_mmu()->load_insn(pc).insn.bits();
        } catch (...) {
          // Fallback to physical load if MMU fetch fails
          uint32_t phys_pc = pc;
          if (pc >= 0xc0000000)
            phys_pc = pc - 0xc0000000 + 0x80000000;
          if (phys_pc >= 0x80000000 &&
              (phys_pc - 0x80000000 + 4) <= main_mem_ptr->size()) {
            uint8_t buf[4];
            if (main_mem_ptr->load(phys_pc - 0x80000000, 4, buf)) {
              val = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            }
          }
        }
        return val;
      };

      ref_insn = fetch_insn(ref_pc_val);
      dut_insn = fetch_insn(dut_pc_val);

      std::cout << "\n" << std::string(80, '=') << std::endl;
      std::cout << "\033[1;31m[Difftest Mismatch Detected!]\033[0m"
                << std::endl;
      std::cout << "Reason:    "
                << (pc_mismatch ? "PC Mismatch" : "Register Mismatch")
                << std::endl;
      std::cout << "Raw Ref PC: 0x" << std::hex << s->pc << std::dec
                << std::endl;
      std::cout << "Raw DUT PC: 0x" << std::hex << dut_state.pc << std::dec
                << std::endl;
      std::cout << "PC:        Ref=0x" << std::hex << ref_pc_val << "  DUT=0x"
                << dut_pc_val << (pc_mismatch ? " <--" : "") << std::dec
                << std::endl;

      if (pc_mismatch) {
        std::cout << "Ref Insn:  0x" << std::hex << ref_insn << std::endl;
        std::cout << "DUT Insn:  0x" << std::hex << dut_insn << std::dec
                  << std::endl;
      } else {
        std::cout << "Insn:      0x" << std::hex << ref_insn << std::dec
                  << std::endl;
      }

      // Print critical CSRs for exception debugging
      uint32_t mcause = p->get_csr(0x342);
      uint32_t mepc = p->get_csr(0x341);
      uint32_t scause = p->get_csr(0x142);
      uint32_t sepc = p->get_csr(0x141);
      uint32_t mstatus = p->get_csr(0x300);
      uint32_t satp = p->get_csr(0x180);
      uint32_t medeleg = p->get_csr(0x302);
      uint32_t mideleg = p->get_csr(0x303);
      reg_t pmpcfg0 = p->get_csr(0x3A0);
      reg_t pmpaddr0 = p->get_csr(0x3B0);
      uint8_t ref_priv = p->get_state()->prv;

      std::cout << "Spike State: PC=0x" << std::hex << s->pc
                << " Priv=" << (int)ref_priv << " mstatus=0x" << mstatus
                << " satp=0x" << satp << std::dec << std::endl;
      std::cout << "Spike Mem Size: 0x" << std::hex << main_mem_ptr->size()
                << std::dec << std::endl;
      std::cout << "Spike PMP:      pmpcfg0=0x" << std::hex << pmpcfg0
                << " pmpaddr0=0x" << pmpaddr0 << std::dec << std::endl;

      if (mcause != 0) {
        std::cout << "\033[1;33m[Trap Detected in Spike]\033[0m mepc=0x"
                  << std::hex << mepc << " mcause=0x" << mcause << std::dec
                  << std::endl;

        uint32_t mepc_insn_ref = 0;
        uint32_t phys_mepc = mepc;
        if (mepc >= 0xc0000000)
          phys_mepc = mepc - 0xc0000000 + 0x80000000;

        if (phys_mepc >= 0x80000000 &&
            (phys_mepc - 0x80000000 + 4) <= main_mem_ptr->size()) {
          uint8_t buf[4];
          if (main_mem_ptr->load(phys_mepc - 0x80000000, 4, buf)) {
            mepc_insn_ref = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                            ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
          }
        }
        std::cout << "  Insn (Ref Mem):    0x" << std::hex << mepc_insn_ref
                  << std::endl;
        extern Ref_cpu ref_cpu;
        std::cout << "  Insn (DUT State):  0x" << ref_cpu.Instruction
                  << std::endl;

        // Decode Store/Load and Walk Page Table
        uint32_t insn = ref_cpu.Instruction;
        uint32_t opcode = insn & 0x7F;
        reg_t v_addr = 0;
        bool has_vaddr = false;

        if (opcode == 0x23) { // Store
          uint32_t rs1 = (insn >> 15) & 0x1F;
          int32_t imm = (int32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1F));
          if (imm & 0x800)
            imm |= 0xFFFFF000;
          v_addr = s->XPR[rs1] + imm;
          has_vaddr = true;
          std::cout << "  Store VAddr Analysis: 0x" << std::hex << v_addr
                    << std::dec << std::endl;
        } else if (opcode == 0x03) { // Load
          uint32_t rs1 = (insn >> 15) & 0x1F;
          int32_t imm = (int32_t)(insn >> 20);
          if (imm & 0x800)
            imm |= 0xFFFFF000;
          v_addr = s->XPR[rs1] + imm;
          has_vaddr = true;
          std::cout << "  Load VAddr Analysis: 0x" << std::hex << v_addr
                    << std::dec << std::endl;
        }

        if (has_vaddr) {
          if (satp & 0x80000000) {
            uint32_t root_ppn = satp & 0x3FFFFF;
            uint32_t vpn1 = (v_addr >> 22) & 0x3FF;
            uint32_t vpn0 = (v_addr >> 12) & 0x3FF;
            uint32_t pte1_addr = (root_ppn << 12) + (vpn1 << 2);

            std::cout << "  [Walk] Level 1 PTE Addr (PA): 0x" << std::hex
                      << pte1_addr << std::endl;

            uint32_t pte1 = 0;
            if (pte1_addr >= 0x80000000 &&
                (pte1_addr - 0x80000000 + 4) <= main_mem_ptr->size()) {
              uint8_t buf[4];
              if (main_mem_ptr->load(pte1_addr - 0x80000000, 4, buf)) {
                pte1 = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
                std::cout << "  [Walk] Level 1 PTE: 0x" << pte1 << std::endl;

                if (pte1 & 0x1) {
                  if ((pte1 & 0x2) || (pte1 & 0x8)) { // Superpage
                    uint32_t pa =
                        ((pte1 << 2) & 0xFFC00000) | (v_addr & 0x3FFFFF);
                    std::cout << "  [Walk] Superpage Leaf -> Final PA: 0x" << pa
                              << std::endl;
                  } else {
                    uint32_t ppn1 = (pte1 >> 10) & 0x3FFFFF;
                    uint32_t pte0_addr = (ppn1 << 12) + (vpn0 << 2);
                    std::cout << "  [Walk] Level 0 PTE Addr (PA): 0x"
                              << pte0_addr << std::endl;
                    if (pte0_addr >= 0x80000000 &&
                        (pte0_addr - 0x80000000 + 4) <= main_mem_ptr->size()) {
                      uint8_t buf0[4];
                      if (main_mem_ptr->load(pte0_addr - 0x80000000, 4, buf0)) {
                        uint32_t pte0 = (uint32_t)buf0[0] |
                                        ((uint32_t)buf0[1] << 8) |
                                        ((uint32_t)buf0[2] << 16) |
                                        ((uint32_t)buf0[3] << 24);
                        std::cout << "  [Walk] Level 0 PTE: 0x" << pte0
                                  << std::endl;
                        if (pte0 & 0x1) {
                          uint32_t pa = ((pte0 >> 10) << 12) | (v_addr & 0xFFF);
                          std::cout << "  [Walk] Final PA: 0x" << pa
                                    << std::endl;
                        } else {
                          std::cout << "  [Walk] Level 0 PTE INVALID!"
                                    << std::endl;
                        }
                      }
                    }
                  }
                } else {
                  std::cout << "  [Walk] Level 1 PTE INVALID!" << std::endl;
                }
              }
            }
          } else {
            std::cout << "  [Walk] MMU Off -> Final PA: 0x" << std::hex
                      << v_addr << std::endl;
          }
        }
        std::cout << std::dec << std::endl;
      }

      std::cout << std::string(80, '-') << std::endl;

      // Print table header
      std::cout << std::left << std::setw(10) << "Register" << std::setw(20)
                << "Reference (Spike)" << std::setw(20) << "DUT (Simulator)"
                << std::endl;

      for (int i = 0; i < 32; ++i) {
        uint32_t ref_val = (uint32_t)s->XPR[i];
        uint32_t dut_val = (uint32_t)dut_state.gpr[i];

        if (mismatches[i]) {
          std::cout << "\033[1;31m"; // Red for mismatch
        }

        std::cout << std::left << "x" << std::setw(9) << std::to_string(i)
                  << "0x" << std::setw(18) << std::hex << ref_val << "0x"
                  << std::setw(18) << std::hex << dut_val;

        if (mismatches[i]) {
          std::cout << "<-- MISMATCH\033[0m";
        }
        std::cout << std::dec << std::endl;
      }
      std::cout << std::string(80, '-') << std::endl;
      std::cout << std::left << std::setw(10) << "CSR Name" << std::setw(20)
                << "Address" << std::setw(20) << "Reference (Spike)"
                << "DUT (Simulator)" << std::endl;

      static const char *csr_names[21] = {
          "mtvec",    "mepc",     "mcause",  "mie",     "mip",  "mtval",
          "mscratch", "mstatus",  "mideleg", "medeleg", "sepc", "stvec",
          "scause",   "sscratch", "stval",   "sstatus", "sie",  "sip",
          "satp",     "mhartid",  "misa"};

      for (int i = 0; i < 21; ++i) {
        if (csr_mismatches[i]) {
          std::cout << "\033[1;31m"; // Red for mismatch
          std::cout << std::left << std::setw(10) << csr_names[i] << "0x"
                    << std::setw(18) << std::hex << implemented_csr_addrs[i]
                    << "0x" << std::setw(18) << std::hex << ref_csr_vals[i]
                    << "0x" << std::setw(18) << std::hex << dut_state.csr[i]
                    << " <-- MISMATCH\033[0m" << std::dec << std::endl;
        }
      }
      std::cout << std::string(80, '=') << std::endl;

      // Immediate exit on first error as requested
      exit(1);
    }

    return true;
  }

  void sync_state(const CPU_state &dut_state, uint8_t privilege) {
    processor_t *p = sim->get_core(0);
    state_t *s = p->get_state();

    // Map DUT state.csr indices to architectural addresses
    static const int csr_map[] = {
        0x305, // csr_mtvec
        0x341, // csr_mepc
        0x342, // csr_mcause
        0x304, // csr_mie
        0x344, // csr_mip
        0x343, // csr_mtval
        0x340, // csr_mscratch
        0x300, // csr_mstatus
        0x303, // csr_mideleg
        0x302, // csr_medeleg
        0x141, // csr_sepc
        0x105, // csr_stvec
        0x142, // csr_scause
        0x140, // csr_sscratch
        0x143, // csr_stval
        0x100, // csr_sstatus
        0x104, // csr_sie
        0x144, // csr_sip
        0x180, // csr_satp
        0xf14, // csr_mhartid
        0x301  // csr_misa
    };

    // Synchronize all 21 CSRs
    for (int i = 0; i < 21; ++i) {
      p->put_csr(csr_map[i], (reg_t)dut_state.csr[i]);
      reg_t actual = p->get_csr(csr_map[i]);
      if (actual != (reg_t)dut_state.csr[i]) {
        std::cout << "[SpikeRef] Sync Info: CSR 0x" << std::hex << csr_map[i]
                  << " Ref=0x" << actual << " DUT=0x" << dut_state.csr[i]
                  << std::dec << std::endl;
      }
    }

    // Set privilege mode (passing false for 'virt' mode)
    p->set_privilege(privilege, false);

    s->pc = dut_state.pc;
    for (int i = 0; i < 32; ++i) {
      s->XPR.write(i, (reg_t)dut_state.gpr[i]);
    }
  }

  void sync_reg_from_dut(int reg_idx, uint32_t val) {
    processor_t *p = sim->get_core(0);
    state_t *s = p->get_state();
    s->XPR.write(reg_idx, (reg_t)val);
  }
};
