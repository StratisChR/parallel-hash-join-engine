#pragma once

#include <cstdint>
#include <cstddef>

namespace Contest {

// Η std::hash για ακέραιους δεν λειτουργούσε αποδοτικά,
// οπότε φτιάξαμε δικό μας hasher για ακέραιους.

// Συγκεκριμένα παρατηρήσαμε ότι όταν χρειαζόταν να γίνουν join
// σε πίνακες με ακέραια κλειδιά, η απόδοση ήταν πολύ χαμηλή επειδή
// υπήρχαν πολλές συγκρούσεις στον πίνακα κατακερματισμού.
// Ο IntHasher χρησιμοποιεί έναν απλό αλλά αποτελεσματικό αλγόριθμο
// που αποφεύγει τις συγκρούσεις και διανέμει ομοιόμορφα τις τιμές.
// Βασίζεται στο MurmurHash3 finalizer.
struct IntHasher {
    size_t operator()(uint64_t x) const {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }
    size_t operator()(uint32_t x) const {
        return operator()(static_cast<uint64_t>(x));
    }
    size_t operator()(int32_t x) const {
        return operator()(static_cast<uint64_t>(x));
    }
    
    size_t operator()(int64_t x) const {
        return operator()(static_cast<uint64_t>(x));
    }
};

} 