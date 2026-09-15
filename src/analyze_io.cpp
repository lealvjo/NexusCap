#include "nexuscap/analyze.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nexuscap {

bool write_analyze_files(const std::string& dir, const AnalyzeWriter& writer, double t_ms,
                         std::string& jsonl_path, std::string& summary_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(dir, ec);

  SYSTEMTIME st{};
  GetLocalTime(&st);
  std::ostringstream stamp;
  stamp << std::setfill('0')
        << std::setw(4) << st.wYear << std::setw(2) << st.wMonth << std::setw(2) << st.wDay
        << "-" << std::setw(2) << st.wHour << std::setw(2) << st.wMinute << std::setw(2) << st.wSecond;

  jsonl_path = (fs::path(dir) / ("nexuscap-analyze-" + stamp.str() + ".jsonl")).string();
  summary_path = (fs::path(dir) / ("nexuscap-analyze-" + stamp.str() + "-summary.txt")).string();

  const auto lines = writer.lines_copy();
  {
    std::ofstream f(jsonl_path, std::ios::binary);
    if (!f) return false;
    for (const auto& line : lines) f << line << "\n";
  }
  {
    std::ofstream f(summary_path, std::ios::binary);
    if (!f) return false;
    f << writer.summary_text(t_ms);
    f << "jsonl: " << jsonl_path << "\n";
  }
  return true;
}

}  // namespace nexuscap
