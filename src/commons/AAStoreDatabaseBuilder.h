// 文件职责：按原生最终 ID 构建 RabbitAAStore 的序列和文本列。
#ifndef AASTORE_DATABASE_BUILDER_H
#define AASTORE_DATABASE_BUILDER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Capture text fields directly while createdb parses the input. Shards follow
// native shuffle partitions; no native header database is created.
class AAStoreCreatedbTextWriter {
public:
    AAStoreCreatedbTextWriter(const std::string& database, size_t splits);
    ~AAStoreCreatedbTextWriter();
    void append(std::string_view header, size_t split);
    void finish();
    void writeLookup(const std::string& database,
                     const std::vector<unsigned int>* sourceLookup);
    void installColumns(const std::string& destination, uint64_t records);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 复用已关联的库，或按原生最终 ID 建库；createdb 重建时强制更新本地库。
void prepareAAStoreDatabase(const std::string& database, size_t threads,
                            bool rebuild = false,
                            AAStoreCreatedbTextWriter* texts = nullptr);

#endif
