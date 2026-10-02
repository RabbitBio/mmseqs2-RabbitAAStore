// 文件职责：从原生数据库的最终 ID 顺序构建序列专用 AAStore。
#ifndef AASTORE_DATABASE_BUILDER_H
#define AASTORE_DATABASE_BUILDER_H

#include <cstddef>
#include <string>

// 复用已关联的库，或按原生最终 ID 建库；createdb 重建时强制更新本地库。
void prepareAAStoreDatabase(const std::string& database, size_t threads,
                            bool rebuild = false);

#endif
