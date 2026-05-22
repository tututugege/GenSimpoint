#pragma once

#include <cstddef>
#include <cstdint>

struct RefCpuContext;

struct RefCpuState {
  uint32_t gpr[32];
  uint32_t csr[21];
  uint32_t pc;

  uint32_t store_addr;
  uint32_t store_data;
  uint32_t store_strb;
  bool store;
  uint32_t instruction;
  bool page_fault_inst;
  bool page_fault_load;
  bool page_fault_store;
  uint32_t inst_idx;
  uint32_t commit_pc;
  bool reserve_valid;
  uint32_t reserve_addr;
  uint8_t privilege;
};

struct RefCpuStepInfo {
  uint32_t instruction;
  bool page_fault_inst;
  bool page_fault_load;
  bool page_fault_store;
  bool sim_end;
  bool is_exception;
  bool is_csr;
  bool is_br;
  bool br_taken;
  bool is_io;
};

RefCpuContext *refcpu_init(uint32_t reset_pc, uint32_t ram_size_bytes);
void refcpu_destroy(RefCpuContext *ctx);

void refcpu_get_state(const RefCpuContext *ctx, RefCpuState *state);
void refcpu_set_state(RefCpuContext *ctx, const RefCpuState *state);
void refcpu_get_step_info(const RefCpuContext *ctx, RefCpuStepInfo *info);

void refcpu_step(RefCpuContext *ctx, uint64_t steps);
void refcpu_set_dut_expected_faults(RefCpuContext *ctx, bool inst, bool load,
                                    bool store);
void refcpu_sync_gprs_from_dut(RefCpuContext *ctx, const uint32_t *gpr,
                               size_t count);

void refcpu_sync_ram_from_dut(RefCpuContext *ctx, const uint32_t *ram_src,
                              size_t size_bytes);
uint32_t *refcpu_get_ram_ptr(RefCpuContext *ctx);
uint32_t refcpu_load_word(const RefCpuContext *ctx, uint32_t addr);
void refcpu_store_word(RefCpuContext *ctx, uint32_t addr, uint32_t data);

void refcpu_clear_io_words(RefCpuContext *ctx);
void refcpu_set_io_word(RefCpuContext *ctx, uint32_t addr, uint32_t data);
uint32_t refcpu_get_io_word(const RefCpuContext *ctx, uint32_t addr);

void refcpu_set_uart_print(RefCpuContext *ctx, bool enable);
void refcpu_set_ref_only(RefCpuContext *ctx, bool enable);
void refcpu_set_sim_end(RefCpuContext *ctx, bool value);

bool refcpu_probe_pc_translation(RefCpuContext *ctx);
