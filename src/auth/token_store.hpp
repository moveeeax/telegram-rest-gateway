#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tgw::auth {

// Хранилище API-токенов (§8.1). Хранит ТОЛЬКО SHA-256 токенов (не сам секрет),
// сравнение — timing-safe по всем хешам без раннего выхода. Заполняется один раз на
// старте (single-threaded), далее только чтение (потокобезопасно, hashes_ иммутабельны).
class TokenStore {
   public:
    static TokenStore& instance();

    void load(const std::vector<std::string>& tokens);
    bool verify(std::string_view presented) const;

    std::size_t size() const { return hashes_.size(); }
    bool empty() const { return hashes_.empty(); }

   private:
    std::vector<std::array<unsigned char, 32>> hashes_;
};

}  // namespace tgw::auth
