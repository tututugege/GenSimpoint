#include "include/api/refcpu_api.h"

#include "include/CSR.h"
#include "include/ref.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *kInitImage = "/dev/null";

struct RefCpuContextImpl {
  Ref_cpu cpu;
  SimConfig cfg;
};

RefCpuState export_state(const Ref_cpu &cpu) {
  RefCpuState state{};
  std::memcpy(state.gpr, cpu.state.gpr, sizeof(state.gpr));
  std::memcpy(state.csr, cpu.state.csr, sizeof(state.csr));
  state.csr[csr_mip] = cpu.visible_mip();
  state.csr[csr_sip] = cpu.visible_sip();
  state.pc = cpu.state.pc;
  state.store_addr = cpu.state.store_addr;
  state.store_data = cpu.state.store_data;
  state.store_strb = cpu.state.store_strb;
  state.store = cpu.state.store;
  state.instruction = cpu.Instruction;
  state.page_fault_inst = cpu.page_fault_inst;
  state.page_fault_load = cpu.page_fault_load;
  state.page_fault_store = cpu.page_fault_store;
  state.inst_idx = 0;
  state.commit_pc = 0;
  state.reserve_valid = cpu.state.reserve_valid;
  state.reserve_addr = cpu.state.reserve_addr;
  state.privilege = cpu.privilege;
  return state;
}

void import_state(Ref_cpu &cpu, const RefCpuState &state) {
  std::memcpy(cpu.state.gpr, state.gpr, sizeof(cpu.state.gpr));
  std::memcpy(cpu.state.csr, state.csr, sizeof(cpu.state.csr));
  cpu.state.pc = state.pc;
  cpu.state.store_addr = state.store_addr;
  cpu.state.store_data = state.store_data;
  cpu.state.store_strb = state.store_strb;
  cpu.state.store = state.store;
  cpu.state.reserve_valid = state.reserve_valid;
  cpu.state.reserve_addr = state.reserve_addr;
  cpu.Instruction = state.instruction;
  cpu.page_fault_inst = state.page_fault_inst;
  cpu.page_fault_load = state.page_fault_load;
  cpu.page_fault_store = state.page_fault_store;
  cpu.privilege = state.privilege;
}

} // namespace

struct RefCpuContext : RefCpuContextImpl {};

RefCpuContext *refcpu_init(uint32_t reset_pc, uint32_t ram_size_bytes) {
  auto *ctx = new RefCpuContext();
  ctx->cfg.mode = SimMode::NORMAL;
  ctx->cfg.image_file = kInitImage;
  ctx->cfg.ram_size = ram_size_bytes;
  ctx->cpu.init(reset_pc, kInitImage, ram_size_bytes);
  ctx->cpu.sim_end = false;
  ctx->cpu.device_effects_enable = false;
  ctx->cpu.interrupt_delivery_enable = false;
  ctx->cpu.ref_only = true;
  ctx->cpu.uart_print = false;
  return ctx;
}

void refcpu_destroy(RefCpuContext *ctx) {
  delete ctx;
}

void refcpu_load_flash_image(RefCpuContext *ctx, const char *path) {
  if (ctx == nullptr || path == nullptr || path[0] == '\0') {
    return;
  }
  ctx->cpu.load_flash_image(path);
}

void refcpu_load_sdcard_image(RefCpuContext *ctx, const char *path) {
  if (ctx == nullptr || path == nullptr || path[0] == '\0') {
    return;
  }
  ctx->cpu.load_sdcard_image(path);
}

void refcpu_get_state(const RefCpuContext *ctx, RefCpuState *state) {
  if (ctx == nullptr || state == nullptr) {
    return;
  }
  *state = export_state(ctx->cpu);
}

void refcpu_set_state(RefCpuContext *ctx, const RefCpuState *state) {
  if (ctx == nullptr || state == nullptr) {
    return;
  }
  import_state(ctx->cpu, *state);
  ctx->cpu.external_mmio_read = {};
  ctx->cpu.external_csr_read = {};
}

void refcpu_get_step_info(const RefCpuContext *ctx, RefCpuStepInfo *info) {
  if (ctx == nullptr || info == nullptr) {
    return;
  }
  info->instruction = ctx->cpu.Instruction;
  info->page_fault_inst = ctx->cpu.page_fault_inst;
  info->page_fault_load = ctx->cpu.page_fault_load;
  info->page_fault_store = ctx->cpu.page_fault_store;
  info->sim_end = ctx->cpu.sim_end;
  info->is_exception = ctx->cpu.is_exception;
  info->is_csr = ctx->cpu.is_csr;
  info->is_br = ctx->cpu.is_br;
  info->br_taken = ctx->cpu.br_taken;
  info->is_io = ctx->cpu.is_io;
}

void refcpu_step(RefCpuContext *ctx, uint64_t steps) {
  if (ctx == nullptr) {
    return;
  }
  for (uint64_t i = 0; i < steps; ++i) {
    if (ctx->cpu.sim_end) {
      break;
    }
    ctx->cpu.RISCV();
  }
}

void refcpu_sync_ram_from_dut(RefCpuContext *ctx, const uint32_t *ram_src,
                              size_t size_bytes) {
  if (ctx == nullptr || ram_src == nullptr || ctx->cpu.memory == nullptr) {
    return;
  }
  const size_t limit = static_cast<size_t>(ctx->cpu.ram_size);
  const size_t bytes = std::min(size_bytes, limit);
  std::memcpy(ctx->cpu.memory, ram_src, bytes);
}

uint32_t *refcpu_get_ram_ptr(RefCpuContext *ctx) {
  return ctx == nullptr ? nullptr : ctx->cpu.memory;
}

uint32_t refcpu_load_word(const RefCpuContext *ctx, uint32_t addr) {
  return ctx == nullptr ? 0u : ctx->cpu.load_word(addr);
}

void refcpu_store_word(RefCpuContext *ctx, uint32_t addr, uint32_t data) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.store_word(addr, data);
}

void refcpu_clear_io_words(RefCpuContext *ctx) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.io_words.clear();
}

void refcpu_set_io_word(RefCpuContext *ctx, uint32_t addr, uint32_t data) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.store_word(addr, data);
}

uint32_t refcpu_get_io_word(const RefCpuContext *ctx, uint32_t addr) {
  return ctx == nullptr ? 0u : ctx->cpu.load_word(addr);
}

void refcpu_set_uart_print(RefCpuContext *ctx, bool enable) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.uart_print = enable;
}

void refcpu_set_ref_only(RefCpuContext *ctx, bool enable) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.ref_only = enable;
}

void refcpu_set_device_effects(RefCpuContext *ctx, bool enable) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.device_effects_enable = enable;
}

void refcpu_set_interrupt_delivery(RefCpuContext *ctx, bool enable) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.interrupt_delivery_enable = enable;
}

void refcpu_set_next_mmio_read(RefCpuContext *ctx, uint32_t paddr,
                               uint32_t result) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.external_mmio_read = {true, paddr, result};
}

void refcpu_set_next_csr_read(RefCpuContext *ctx, uint32_t csr_addr,
                              uint32_t value) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.external_csr_read = {true, csr_addr, value};
}

bool refcpu_take_forced_interrupt(RefCpuContext *ctx, uint32_t cause,
                                  uint8_t target_privilege,
                                  uint32_t pending_snapshot) {
  return ctx != nullptr && ctx->cpu.take_forced_interrupt(
                               cause, target_privilege, pending_snapshot);
}

void refcpu_set_sim_end(RefCpuContext *ctx, bool value) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.sim_end = value;
}

void refcpu_set_sim_time(RefCpuContext *ctx, uint64_t sim_time) {
  if (ctx == nullptr) {
    return;
  }
  ctx->cpu.sim_time = sim_time;
}

bool refcpu_probe_pc_translation(RefCpuContext *ctx) {
  if (ctx == nullptr) {
    return false;
  }
  if ((ctx->cpu.state.csr[csr_satp] & 0x80000000u) == 0 ||
      ctx->cpu.privilege == RISCV_MODE_M) {
    return true;
  }
  uint32_t p_addr = 0;
  return ctx->cpu.va2pa(p_addr, ctx->cpu.state.pc, 0);
}
