#include <sys/mman.h>
#include <vector>
#include <iostream>

class GlobalAllocator {
private:
    std::vector<std::pair<void*, size_t>> allocated_chunks;
    
    static constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024; 

public:
    GlobalAllocator() = default;
    ~GlobalAllocator() {
        // We free all chunks.
        for (auto& chunk : allocated_chunks) {
            munmap(chunk.first, chunk.second);
        }
    }

    GlobalAllocator(const GlobalAllocator&) = delete;
    GlobalAllocator& operator=(const GlobalAllocator&) = delete;

    void* allocate_large_chunk() {
        // We try to get a Huge Page. 
        void* ptr = mmap(nullptr, HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

        // If the huge page isn't configured, we request 2MB again
        // but in 4KB pages.
        if (ptr == MAP_FAILED) {
            ptr = mmap(nullptr, HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            
            if (ptr != MAP_FAILED) {
                // Transparent Huge Pages here if available
                madvise(ptr, HUGE_PAGE_SIZE, MADV_HUGEPAGE); 
            }
        }

        // WE push it back for cleanup.
        allocated_chunks.push_back({ptr, HUGE_PAGE_SIZE});
        return ptr;
    }
};

struct BumpAlloc {

    struct Chunk {
        Chunk* next;
        char data[];
    };

    Chunk* head = nullptr;
    char* ptr = nullptr;
    char* end = nullptr;

    // We check whether the current chunk has enoigh space or not.
    // Based on the tuple size, we know that no chunk is filled completely.
    inline bool hasSpace(size_t size) {
        return ptr && (ptr + size <= end);
    }

    // We move the pointer forward, and return the old position.
    inline void* allocate(size_t size) {
        void* ret = ptr;
        ptr += size;
        return ret;
    }

    // We need more space.
    void addSpace(void* raw_memory, size_t size) {
        Chunk* new_chunk = reinterpret_cast<Chunk*>(raw_memory);
        // Chunk* new_chunk = new (raw_memory) Chunk{head};

        new_chunk->next = head;
        head = new_chunk;
        
        // Start of usable memory.
        ptr = new_chunk->data;
        // The end is the start of the memory block (the whole block, not the 
        // start of the usable part) + size.
        end = reinterpret_cast<char*>(raw_memory) + size;
    }
};