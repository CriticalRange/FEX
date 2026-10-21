#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <vector>

// Owns bytes, never GL objects: a hit creates a fresh program with independent
// uniforms. Exact source keys avoid hash collisions and program-name reuse.
class VexaCloneBinaryCache {
public:
  struct Entry {
    std::string Source;
    uint32_t Format;
    std::vector<uint8_t> Bytes;
  };
  explicit VexaCloneBinaryCache(size_t budget = 8 * 1024 * 1024, size_t limit = 64)
    : Budget {budget}, Limit {limit} {}

  const Entry* Find(const std::string& source) {
    for (auto it = Entries.begin(); it != Entries.end(); ++it) {
      if (it->Source == source) {
        Entries.splice(Entries.begin(), Entries, it);
        return &Entries.front();
      }
    }
    return nullptr;
  }
  void Erase(const std::string& source) {
    for (auto it = Entries.begin(); it != Entries.end(); ++it) {
      if (it->Source == source) {
        Used -= it->Source.size() + it->Bytes.size();
        Entries.erase(it);
        return;
      }
    }
  }
  void Insert(std::string source, uint32_t format, std::vector<uint8_t> bytes) {
    const size_t size = source.size() + bytes.size();
    if (bytes.empty() || size > Budget || !Limit) return;
    Erase(source);
    while (!Entries.empty() && (Used + size > Budget || Entries.size() >= Limit)) {
      Used -= Entries.back().Source.size() + Entries.back().Bytes.size();
      Entries.pop_back();
    }
    Used += size;
    Entries.push_front({std::move(source), format, std::move(bytes)});
  }
  size_t SizeBytes() const { return Used; }
  size_t Size() const { return Entries.size(); }

private:
  size_t Budget, Limit, Used {};
  std::list<Entry> Entries;
};
