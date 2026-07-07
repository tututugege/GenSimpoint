#ifdef ENABLE_SPIKE
#include "spike_ref.h"
#endif
#include "RISCV.h"
#include "ref.h"
#include <cstdlib>
#include <iostream>

Ref_cpu ref_cpu;
int main(int argc, char *argv[]) {
  SimConfig config;

  // 简易参数解析
  if (argc == 2) {
    config.image_file = argv[1];
  } else {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--image")
        config.image_file = argv[++i];
      else if (arg == "--flash-image")
        config.flash_image_file = argv[++i];
      else if (arg == "--sdcard-image")
        config.sdcard_image_file = argv[++i];
      else if (arg == "--reset-pc")
        config.reset_pc = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 0));

      // 模式选择
      else if (arg == "--mode") {
        std::string m = argv[++i];
        if (m == "normal")
          config.mode = SimMode::NORMAL;
        else if (m == "bbv")
          config.mode = SimMode::GEN_BBV;
        else if (m == "ckpt")
          config.mode = SimMode::GEN_CHECKPOINT;
        else if (m == "restore")
          config.mode = SimMode::RESTORE;
      }

      // 路径参数
      else if (arg == "--out-bbv")
        config.bbv_output_file = argv[++i];
      else if (arg == "--points")
        config.points_file = argv[++i];
      else if (arg == "--ckpt-dir")
        config.checkpoint_dir = argv[++i];
      else if (arg == "--restore-file")
        config.restore_file = argv[++i];
      else if (arg == "--max-insts")
        config.max_insts = std::stoull(argv[++i]);
      else if (arg == "--diff")
        config.difftest = true;
    }
  }

  // 限制：Difftest 仅在 NORMAL 模式下可用
  if (config.difftest && config.mode != SimMode::NORMAL) {
    std::cout << "\033[1;33m[Warning] Difftest is only supported in NORMAL mode. Disabling Difftest.\033[0m" << std::endl;
    config.difftest = false;
  }

#ifndef ENABLE_SPIKE
  if (config.difftest) {
    std::cout << "\033[1;33m[Warning] This binary is built without Spike support (SPIKE=0). Disabling Difftest.\033[0m"
              << std::endl;
    config.difftest = false;
  }
#endif

  ref_cpu.init(config.reset_pc,
               config.image_file.empty() ? nullptr : config.image_file.c_str(),
               PHYSICAL_MEMORY_LENGTH);
  if (!config.flash_image_file.empty())
    ref_cpu.load_flash_image(config.flash_image_file);
  if (!config.sdcard_image_file.empty())
    ref_cpu.load_sdcard_image(config.sdcard_image_file);
  ref_cpu.uart_print = true;
  if (const char *env = std::getenv("REF_FORCE_REF_ONLY");
      env != nullptr && env[0] != '\0' && env[0] != '0') {
    ref_cpu.ref_only = true;
  }
  ref_cpu.exec(config);

  return 0;
}
