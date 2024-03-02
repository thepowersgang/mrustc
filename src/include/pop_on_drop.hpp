// Helper for building anti-recursion stacks
// 
// 
#pragma once
#include <vector>

template<typename T>
class PopOnDrop {
    std::vector<T>*  stack;
public:
    PopOnDrop(std::vector<T>& stack): stack(&stack) {}
    PopOnDrop(const PopOnDrop&) = delete;
    PopOnDrop& operator=(const PopOnDrop&) = delete;
    PopOnDrop(PopOnDrop&& x): stack(x.stack) { x.stack = nullptr; }
    PopOnDrop& operator=(PopOnDrop&&) = delete;
    ~PopOnDrop() {
        if(stack) stack->pop_back();
        stack = nullptr;
    }
};
template<typename T>
PopOnDrop<T> push_and_pop_at_end(std::vector<T>& stack, T ent) {
    stack.push_back(std::move(ent));
    return PopOnDrop<T>(stack);
}

