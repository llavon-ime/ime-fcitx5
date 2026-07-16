#pragma once

#include <functional>

namespace imesvc {

// 有明顯效能優化空間 但我不想鳥他 僅供 debug 用
class before_return {
public:
    before_return(std::function<void()> func) : func_(std::move(func)) {}
    ~before_return() {
        if (func_) func_();
    }

private:
    std::function<void()> func_;
};

}  // namespace imesvc