// Core :: Allocator...

#ifndef BASE_HPP_
#define BASE_HPP_

#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#define unreachable                                                                                       \
  {                                                                                                       \
    std::cerr << R"("Unreachable" code was reached: )" << __FILE__ << ":" << __LINE__ << ", at function " \
              << static_cast<char const*>(__func__) << std::endl;                                         \
    std::terminate();                                                                                     \
  }
#define notimplemented                                                                                       \
  {                                                                                                          \
    std::cerr << R"("Not implemented" code was called: )" << __FILE__ << ":" << __LINE__ << ", at function " \
              << static_cast<char const*>(__func__) << std::endl;                                            \
    std::terminate();                                                                                        \
  }

namespace Core {

  // A simple region-based memory allocator: https://news.ycombinator.com/item?id=26433654
  // This ensures that objects stay in the same place
  template <typename T>
  class Allocator {
  public:
    static constexpr std::size_t DefaultBlockSize = 1024;

    Allocator(std::size_t blockSize = DefaultBlockSize): blockSize(blockSize), alloc(), blocks(), next(0) {}

    Allocator(Allocator const&) = delete;
    Allocator(Allocator&& r) noexcept:
      blockSize(r.blockSize), alloc(std::move(r.alloc)), blocks(std::move(r.blocks)), next(std::exchange(r.next, 0)) {}

    Allocator& operator=(Allocator const&) = delete;
    Allocator& operator=(Allocator&& r) noexcept {
      if (this != &r) {
        deallocateBlocks();
        blockSize = r.blockSize;
        alloc = std::move(r.alloc);
        blocks = std::move(r.blocks);
        next = std::exchange(r.next, 0);
      }
      return *this;
    }

    ~Allocator() { deallocateBlocks(); }

    // TODO: add option to preserve allocated space
    void clear() noexcept {
      deallocateBlocks();
      blocks.clear();
      next = 0;
    }

    template <typename... Ts>
    T* emplaceBack(Ts&&... args) {
      if (next == 0) blocks.push_back(alloc.allocate(blockSize));
      T* res = blocks.back() + next;
      std::construct_at(res, std::forward<Ts>(args)...);
      next++;
      if (next >= blockSize) next = 0;
      return res;
    }

    T* pushBack(T const& obj) { return emplaceBack(obj); }

    std::size_t size() const noexcept {
      if (next == 0) return blockSize * blocks.size();
      return blockSize * (blocks.size() - 1) + next;
    }

  private:
    std::size_t blockSize;
    std::allocator<T> alloc;
    std::vector<T*> blocks;
    std::size_t next;

    void deallocateBlocks() noexcept {
      for (std::size_t i = 0; i < blocks.size(); i++) {
        std::destroy_n(blocks[i], (i + 1 == blocks.size() && next > 0) ? next : blockSize);
        alloc.deallocate(blocks[i], blockSize);
      }
    }
  };

}

#endif // BASE_HPP_
