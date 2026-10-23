#pragma once
#include <sys/mman.h>
#include <mutex>
#include <new>
#include <vector>

// Process-wide stock of 2 MB chunks. A chunk handed back by a finished build is
// kept and reused by the next build instead of being unmapped and mapped again:
// no mmap/munmap system calls per join, and the memory is already backed by
// physical pages, so reusing it causes no page faults (paper, Section 3.5).
class ChunkPool {
public:
    static constexpr size_t CHUNK_SIZE = 2 * 1024 * 1024;

    static ChunkPool& instance() {
        static ChunkPool p;
        return p;
    }

    void* get() {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!free_.empty()) {
                void* p = free_.back();
                free_.pop_back();
                return p;
            }
        }
        return map_new_chunk();
    }

    void put(void* chunk) {
        std::lock_guard<std::mutex> lk(m_);
        free_.push_back(chunk);
    }

    ~ChunkPool() {
        for (void* p : free_) munmap(p, CHUNK_SIZE);
    }

private:
    static void* map_new_chunk() {
        // Try an explicit huge page first.
        void* ptr = mmap(nullptr, CHUNK_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            // Not configured: ordinary pages, and ask for transparent huge pages.
            ptr = mmap(nullptr, CHUNK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (ptr == MAP_FAILED) throw std::bad_alloc();
            madvise(ptr, CHUNK_SIZE, MADV_HUGEPAGE);
        }
        return ptr;
    }

    std::mutex         m_;
    std::vector<void*> free_;
};

// Level 1 of the slab allocator. Takes 2 MB chunks from the ChunkPool and gives
// them all back when the build that used it is finished.
class GlobalAllocator {
private:
    std::vector<void*> allocated_chunks;

public:
    GlobalAllocator() = default;
    ~GlobalAllocator() {
        for (void* chunk : allocated_chunks) ChunkPool::instance().put(chunk);
    }

    GlobalAllocator(const GlobalAllocator&) = delete;
    GlobalAllocator& operator=(const GlobalAllocator&) = delete;

    void* allocate_large_chunk() {
        void* ptr = ChunkPool::instance().get();
        allocated_chunks.push_back(ptr);
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