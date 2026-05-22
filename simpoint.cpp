#include "ref.h"
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <zlib.h>

namespace {
constexpr uint32_t CKPT_MAGIC = 0x006d6552u; // "Rem\0" (little-endian)
constexpr uint32_t CKPT_VERSION = 2u;
constexpr uint32_t BOOT_IO_BASE = 0x00000000u;
constexpr uint32_t BOOT_IO_SIZE = 0x00002000u;

struct CkptHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t ram_size;
  uint32_t io_range_count;
};

struct CkptIoRange {
  uint32_t base;
  uint32_t size;
};

std::vector<CkptIoRange> expected_io_layout() {
  return {
      {BOOT_IO_BASE, BOOT_IO_SIZE},
      {UART_BASE, UART_MMIO_SIZE},
      {PLIC_BASE, PLIC_MMIO_SIZE},
      {TIMER_BASE, TIMER_MMIO_SIZE},
  };
}

template <typename T> void gz_write_pod(gzFile file, const T &data) {
  if (gzwrite(file, &data, sizeof(T)) != sizeof(T)) {
    std::cerr << "Error writing checkpoint data." << std::endl;
    exit(1);
  }
}

template <typename T> void gz_read_pod(gzFile file, T &data) {
  if (gzread(file, &data, sizeof(T)) != sizeof(T)) {
    std::cerr << "Error reading checkpoint data." << std::endl;
    exit(1);
  }
}

void gz_write_bytes(gzFile file, const void *buf, size_t len) {
  if (len == 0) {
    return;
  }
  if (gzwrite(file, buf, static_cast<unsigned int>(len)) != static_cast<int>(len)) {
    std::cerr << "Error writing checkpoint payload." << std::endl;
    exit(1);
  }
}

void gz_read_bytes(gzFile file, void *buf, size_t len) {
  if (len == 0) {
    return;
  }
  if (gzread(file, buf, static_cast<unsigned int>(len)) != static_cast<int>(len)) {
    std::cerr << "Error reading checkpoint payload." << std::endl;
    exit(1);
  }
}
} // namespace

void Ref_cpu::bbv_init_file(const char *filename) {
  bbv_file.open(filename, std::ios::out | std::ios::trunc);
  if (!bbv_file.is_open()) {
    std::cerr << "Error: Could not open BBV output file!" << std::endl;
  }
  bbv_counts.resize(65536, 0);
}

void Ref_cpu::bbv_commit() {
  // [ID 映射]：将 PC 转换为 SimPoint ID
  uint32_t bb_id = 0;
  auto it = global_pc_to_id.find(current_bb_head_pc);
  if (it != global_pc_to_id.end()) {
    bb_id = it->second; // 以前见过，用老 ID
  } else {
    bb_id = next_bb_id++; // 没见过，分配新 ID
    global_pc_to_id[current_bb_head_pc] = bb_id;
  }

  if (bb_id >= bbv_counts.size()) {
    // 策略：指数级扩容 (Geometric Growth)
    // 比如每次扩大 2 倍，避免每次只加 1 导致频繁申请内存
    // 如果 id 突然变得很大，至少要扩容到 id + 1
    size_t new_size = bbv_counts.size() * 2;
    if (new_size <= bb_id)
      new_size = bb_id + 1024;

    // resize 会自动把新增加的部分初始化为 0
    bbv_counts.resize(new_size, 0);
  }

  // 3. [统计]：当前切片计数
  bbv_counts[bb_id] += current_bb_len;
}

void Ref_cpu::dump_bbv() {
  static int count = 0;
  bbv_file << "T";
  // 遍历 vector，只要非零的都输出
  for (size_t id = 1; id < bbv_counts.size(); ++id) {
    if (bbv_counts[id] > 0) {
      bbv_file << " :" << id << ":" << bbv_counts[id];
      bbv_counts[id] = 0; // 输出完清零，为下一轮做准备
    }
  }
  bbv_file << "\n";
  std::cout << "bbv dump:" << count++ << std::endl;
}

void Ref_cpu::save_checkpoint(const std::string &filename) {
  std::string final_name = filename;
  if (final_name.size() < 3 ||
      final_name.substr(final_name.size() - 3) != ".gz") {
    final_name += ".gz";
  }

  gzFile file = gzopen(final_name.c_str(), "wb1"); // wb1 速度最快
  if (!file) {
    std::cerr << "Error: Could not open file: " << final_name << std::endl;
    exit(1);
  }

  const auto io_layout = expected_io_layout();
  CkptHeader header = {
      CKPT_MAGIC,
      CKPT_VERSION,
      ram_size,
      static_cast<uint32_t>(io_layout.size()),
  };
  gz_write_pod(file, header);

  // 1. 保存 CPU 状态
  gz_write_pod(file, state);
  gz_write_pod(file, interval_inst_count);

  // 2. 保存 RAM
  if (ram_size == 0 || memory == nullptr) {
    std::cerr << "Error: Invalid RAM buffer when saving checkpoint." << std::endl;
    exit(1);
  }
  std::cout << "Saving Memory: " << (ram_size / sizeof(uint32_t)) << " words ("
            << (ram_size / 1024 / 1024) << " MB)..." << std::endl;
  gz_write_bytes(file, reinterpret_cast<const uint8_t *>(memory), ram_size);

  // 3. 保存 IO 布局（base + size）
  for (const auto &r : io_layout) {
    gz_write_pod(file, r);
    std::vector<uint8_t> io_bytes(r.size, 0);
    for (const auto &kv : io_words) {
      if (kv.first < r.base) {
        continue;
      }
      const uint64_t off = static_cast<uint64_t>(kv.first) - r.base;
      if (off + sizeof(uint32_t) > r.size) {
        continue;
      }
      io_bytes[off + 0] = static_cast<uint8_t>(kv.second & 0xFF);
      io_bytes[off + 1] = static_cast<uint8_t>((kv.second >> 8) & 0xFF);
      io_bytes[off + 2] = static_cast<uint8_t>((kv.second >> 16) & 0xFF);
      io_bytes[off + 3] = static_cast<uint8_t>((kv.second >> 24) & 0xFF);
    }
    gz_write_bytes(file, io_bytes.data(), io_bytes.size());
  }

  gzclose(file);
  std::cout << "Checkpoint saved to " << final_name << std::endl;
}

void Ref_cpu::restore_checkpoint(const std::string &filename) {
  std::string final_name = filename;
  gzFile file = gzopen(final_name.c_str(), "rb");
  if (!file && final_name.find(".gz") == std::string::npos) {
    final_name += ".gz";
    file = gzopen(final_name.c_str(), "rb");
  }

  if (!file) {
    std::cerr << "Error: Could not open file: " << filename << std::endl;
    exit(1);
  }

  CkptHeader header = {};
  gz_read_pod(file, header);
  if (header.magic != CKPT_MAGIC) {
    std::cerr << "Error: Invalid checkpoint magic." << std::endl;
    exit(1);
  }
  if (header.version != CKPT_VERSION) {
    std::cerr << "Error: Unsupported checkpoint version: " << header.version
              << std::endl;
    exit(1);
  }
  if (header.ram_size != ram_size) {
    std::cerr << "Error: Checkpoint RAM size mismatch. file=0x" << std::hex
              << header.ram_size << " sim=0x" << ram_size << std::dec
              << std::endl;
    exit(1);
  }

  // 1. 恢复 CPU 状态
  gz_read_pod(file, state);
  gz_read_pod(file, interval_inst_count);

  // 2. 恢复 RAM
  if (memory == nullptr) {
    std::cerr << "Error: Memory not allocated." << std::endl;
    exit(1);
  }
  std::cout << "Restoring Memory..." << std::endl;
  gz_read_bytes(file, reinterpret_cast<uint8_t *>(memory), ram_size);

  // 3. 恢复并校验 IO 布局（base + size）
  const auto io_layout = expected_io_layout();
  if (header.io_range_count != io_layout.size()) {
    std::cerr << "Error: IO layout count mismatch. file="
              << header.io_range_count << " sim=" << io_layout.size()
              << std::endl;
    exit(1);
  }
  io_words.clear();
  for (uint32_t i = 0; i < header.io_range_count; ++i) {
    CkptIoRange r = {};
    gz_read_pod(file, r);
    if (r.base != io_layout[i].base || r.size != io_layout[i].size) {
      std::cerr << "Error: IO layout mismatch at index " << i << ". file=[0x"
                << std::hex << r.base << ", 0x" << r.size << "] sim=[0x"
                << io_layout[i].base << ", 0x" << io_layout[i].size << "]"
                << std::dec << std::endl;
      exit(1);
    }
    std::vector<uint8_t> io_bytes(r.size, 0);
    gz_read_bytes(file, io_bytes.data(), io_bytes.size());
    for (uint32_t off = 0; off + 4 <= r.size; off += 4) {
      uint32_t word = static_cast<uint32_t>(io_bytes[off + 0]) |
                      (static_cast<uint32_t>(io_bytes[off + 1]) << 8) |
                      (static_cast<uint32_t>(io_bytes[off + 2]) << 16) |
                      (static_cast<uint32_t>(io_bytes[off + 3]) << 24);
      if (word != 0) {
        io_words[r.base + off] = word;
      }
    }
  }

  gzclose(file);
  std::cout << "Checkpoint restored from " << final_name << std::endl;
}

// 辅助结构：记录 SimPoint ID 和 对应的 Interval ID
struct PointInfo {
  uint32_t sp_id;       // SimPoint ID (例如 0, 1, 2...)
  uint32_t interval_id; // 对应的 Interval 编号 (例如 105, 2000...)
};

std::map<uint32_t, uint32_t> load_simpoints(const std::string &filename) {
  std::map<uint32_t, uint32_t> targets;
  std::ifstream in(filename);
  if (!in) {
    std::cerr << "Error: Could not open points file: " << filename << std::endl;
    exit(1);
  }

  std::string line;
  while (std::getline(in, line)) {
    // 跳过空行和注释
    if (line.empty() || line[0] == '#')
      continue;

    std::stringstream ss(line);
    uint32_t interval_id, sp_id;

    // data format: <Interval_ID> <SimPoint_ID>
    // Example: 1824 0
    if (ss >> interval_id >> sp_id) {
      targets[interval_id] = sp_id;
      // 调试输出，确保读入正确
      std::cout << "Target: Interval " << interval_id << " -> SP " << sp_id
                << std::endl;
    }
  }
  return targets;
}
