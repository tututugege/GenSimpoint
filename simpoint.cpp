#include "ref.h"
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <zlib.h>

// --- 辅助函数：简化 zlib 读写 POD 类型 ---
template <typename T> void gz_write_pod(gzFile file, const T &data) {
  if (gzwrite(file, &data, sizeof(T)) != sizeof(T)) {
    std::cerr << "Error writing data to gzip file." << std::endl;
    exit(1);
  }
}

template <typename T> void gz_read_pod(gzFile file, T &data) {
  if (gzread(file, &data, sizeof(T)) != sizeof(T)) {
    std::cerr << "Error reading data from gzip file." << std::endl;
    exit(1);
  }
}

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

#include <iostream>
#include <string>
#include <zlib.h>

// 单次写入的安全块大小 (1GB)
const uint64_t GZ_CHUNK_SIZE = 1ULL * 1024 * 1024 * 1024;

void Ref_cpu::save_checkpoint(const std::string &filename) {
  std::string final_name = filename;
  if (final_name.size() < 3 ||
      final_name.substr(final_name.size() - 3) != ".gz") {
    final_name += ".gz";
  }

  gzFile file = gzopen(final_name.c_str(), "wb1"); // wb1 速度最快
  if (!file) {
    std::cerr << "Error: Could not open file: " << final_name << std::endl;
    return;
  }

  // 1. 保存各种状态 (POD)
  gz_write_pod(file, state);
  gz_write_pod(file, interval_inst_count);

  // 2. 保存内存 (关键修改)
  if (ram_size > 0 && memory != nullptr) {
    uint64_t total_bytes = ram_size;

    uint8_t *byte_ptr = reinterpret_cast<uint8_t *>(memory);
    uint64_t remain = total_bytes;

    std::cout << "Saving Memory: " << (ram_size / sizeof(uint32_t)) << " words ("
              << (total_bytes / 1024 / 1024) << " MB)..." << std::endl;

    while (remain > 0) {
      // 每次最多写入 1GB，确保不超出 zlib 限制
      unsigned int chunk = (remain > GZ_CHUNK_SIZE)
                               ? (unsigned int)GZ_CHUNK_SIZE
                               : (unsigned int)remain;

      int written = gzwrite(file, byte_ptr, chunk);
      if (written <= 0) {
        std::cerr << "Error: gzwrite failed." << std::endl;
        break;
      }

      byte_ptr += chunk; // 指针按字节移动
      remain -= chunk;
    }
  }

  // 3. 保存离散 I/O 空间
  uint32_t io_count = static_cast<uint32_t>(io_words.size());
  gz_write_pod(file, io_count);
  for (const auto &kv : io_words) {
    gz_write_pod(file, kv.first);
    gz_write_pod(file, kv.second);
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

  // 1. 恢复状态
  gz_read_pod(file, state);
  gz_read_pod(file, interval_inst_count);

  // 2. 恢复内存
  if (memory == nullptr) {
    std::cerr << "Error: Memory not allocated." << std::endl;
    exit(1);
  }

  uint64_t total_bytes = ram_size;
  uint8_t *byte_ptr = reinterpret_cast<uint8_t *>(memory);
  uint64_t remain = total_bytes;

  std::cout << "Restoring Memory..." << std::endl;

  while (remain > 0) {
    unsigned int chunk = (remain > GZ_CHUNK_SIZE) ? (unsigned int)GZ_CHUNK_SIZE
                                                  : (unsigned int)remain;

    int read_bytes = gzread(file, byte_ptr, chunk);
    if (read_bytes < 0) {
      std::cerr << "Error: gzread failed." << std::endl;
      exit(1);
    }
    if (read_bytes == 0) {
      std::cerr << "Error: Unexpected EOF." << std::endl;
      exit(1);
    }

    byte_ptr += read_bytes;
    remain -= read_bytes;
  }

  // 3. 恢复离散 I/O 空间 (兼容旧 checkpoint：读不到则保持 init 默认值)
  io_words.clear();
  uint32_t io_count = 0;
  int io_count_bytes = gzread(file, &io_count, sizeof(io_count));
  if (io_count_bytes == (int)sizeof(io_count)) {
    for (uint32_t i = 0; i < io_count; ++i) {
      uint32_t addr = 0;
      uint32_t data = 0;
      gz_read_pod(file, addr);
      gz_read_pod(file, data);
      io_words[addr] = data;
    }
  } else if (io_count_bytes != 0) {
    std::cerr << "Error: Corrupted checkpoint (bad io_count)." << std::endl;
    exit(1);
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
