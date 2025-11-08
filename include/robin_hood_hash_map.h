#pragma once

#include <vector>
#include <functional>
#include <stdexcept>
#include <cstddef>
#include <utility>
#include <iterator>
#include <int_hasher.h> // Include the new hasher

namespace Contest {

template <class Key, class Value, class Hasher = std::hash<Key>>
class RobinHoodHashMap {
private:


    // Η δομή bucket αποθηκεύει το αντιείμενο, το PSL του και το αν ο κουβάς χρησιμοποιείται
    struct Bucket {
        Key    key;
        Value  value;
        size_t psl = 0;  
        bool   used = false;    //true αν ο κουβάς είναι γεμάτος

        Bucket() = default; 
        
        Bucket(Key k, Value v, size_t p, bool u) 
            : key(std::move(k)), value(std::move(v)), psl(p), used(u) {}
    };

    std::vector<Bucket> _table;
    size_t              _size = 0;
    Hasher              _hasher;
    
    // max load factor
    static constexpr double MAX_LOAD_FACTOR = 0.85;
    size_t _capacity_mask;  //μάσκα για αποφυγή υπολογισμού modulo
    // Η μάσκα έχει την τιμή capacity - 1.
    // Λειτουργεί αποδοτικότερα από το modulo παράγοντας το ίδιο αποτέλεσμα.
    // Αυτό ισχύει επειδή η χωρητικότητα μας είναι δύναμη του 2.

    size_t hash(const Key& key) const {
        if (capacity() == 0) return 0;
        return _hasher(key) & _capacity_mask;   //bitwise and αντί για modulo
    }

    //  τρέχουσα χωρητικότητα
    size_t capacity() const {
        return _table.size();
    }

    
    // Internal insert.
    // Προσπαθούμε να εισάγουμε ζεύγη τιμών (key, value) 
    // Αν το ζεύγος τιμών που προσπαθούμε να εισάγουμε έχει μεγαλύτερο PSL από το ήδη υπάρχον τα αλλάζουμε.
    // Προσπαθούμε να εισάγουμε το ζεύγος που μετακινηθηκε.
    bool insert_internal(Key key, Value value) {
        size_t start_pos = hash(key);
        size_t psl_in_hand = 0;

        for (size_t i = 0; i < capacity(); ++i) {

            size_t current_pos = (start_pos + i) & _capacity_mask;
            auto& bucket = _table[current_pos];

            if (!bucket.used) {
                // κενός κουβάς.
                bucket.key = std::move(key);
                bucket.value = std::move(value);
                bucket.psl = psl_in_hand;
                bucket.used = true;
                return true;
            }

            if (bucket.psl < psl_in_hand) {
                // Το αντικείμενο στον κουβά έχει χαμηλότερο PSL από αυτό που προσπαθούμε να εισάγουμε
                // "Steal from the rich"
                std::swap(bucket.key, key);
                std::swap(bucket.value, value);
                std::swap(bucket.psl, psl_in_hand);
            }
            psl_in_hand++;
        }
        return false;
    }

    // Rehash function.
    // Κάνουμε rehash όταν ξεπεραστεί το max load factor
    void rehash(Key item_in_hand_key, Value item_in_hand_val) {
        size_t new_cap = (capacity() == 0) ? 128 : capacity() * 2; // Υπολογισμός νέας χωρητικότητας
        std::vector<Bucket> new_table;  // προσωρινός παλιός πίνακας
        new_table.resize(new_cap);  // αλλαγή μεγέθους 

        _table.swap(new_table); // swap τον παλιό πίνακα με τον προσωρινό

        _capacity_mask = new_cap - 1;
        _size = 0; 

        //επαναεισαγωγή όλων των στοιχείων από το new_table
        for (auto& bucket : new_table)
        {
            if (bucket.used) {
                if (insert_internal(std::move(bucket.key), std::move(bucket.value))){
                    _size++;
                } else {
                    // This should not happen as we doubled the capacity
                    throw std::runtime_error("Rehash failed during reinsertion");
                }
            }
        }

        if (insert_internal(std::move(item_in_hand_key), std::move(item_in_hand_val)))
        {
            _size++;
        } else {
            // This should not happen as we doubled the capacity
            throw std::runtime_error("Rehash failed during insertion of new item");
        }
        
    }


public:

    // Iterator που βάλαμε και στο CuckooHashMap.
    class iterator {
    private:
        Bucket* _bucket;

        // Proxy struct για να επιτρέψουμε την πρόσβαση στο Value με την ονομασία second.
        struct PairProxy {
            Value& second;
        };
        PairProxy _proxy;

    public:
        // Constructor για αρχικοποίηση iterator με συγκεκριμένο bucket.
        iterator(Bucket* b) : _bucket(b), _proxy{b ? b->value : *((Value*)nullptr)} {}
        // Constructor για end iterator (δείχνει σε null).
        iterator() : _bucket(nullptr), _proxy{*((Value*)nullptr)} {}


        // Έτσι επιτρέπεται η σύνταξη itr->second για πρόσβαση στην τιμή.
        PairProxy* operator->() { return &_proxy; }

        bool operator==(const iterator& other) const {
            return _bucket == other._bucket;
        }
        bool operator!=(const iterator& other) const {
            return _bucket != other._bucket;
        }
    };

    explicit RobinHoodHashMap(size_t initial_capacity = 128) {
        size_t cap = 1;
        while (cap < initial_capacity) cap <<= 1;
        _table.resize(cap);
        _capacity_mask = cap - 1;
    }

    iterator end() {
        return iterator(nullptr);
    }

    iterator find(const Key& key) {
        if (capacity() == 0) return end();

        size_t pos = hash(key);

        for (size_t i = 0; i < capacity(); ++i) {
            size_t idx = (pos + i) & _capacity_mask;
            auto& bucket = _table[idx];

            if (!bucket.used) {
                return end();
            }

            // Αν το PSL του κουβά είναι μικρότερο από το i,
            // τότε το στοιχείο που ψάχνουμε θα του είχε κλέψει την 
            // θέση, άρα δεν υπάρχει στον πίνακα.
            if (bucket.psl < i) {
                return end();
            }

            if (bucket.key == key) {
                // Found it.
                return iterator(&bucket);
            }
        }

        // Δεν βρέθηκε το στοιχείο.
        return end();
    }

    void emplace(Key key, Value value) { // Back to original signature
        if (find(key) != end())
        {
            return; // Key already exists, do not insert duplicates.
        }
        
        // έλεγχος για max load factor
        // αν ξεπερνάμε το max load factor κάνουμε rehash
        if (_size + 1 > (size_t)(capacity() * MAX_LOAD_FACTOR)) {
            rehash(std::move(key), std::move(value)); 
            return;
        }

        // προσπάθεια εισαγωγής
        if (insert_internal(std::move(key), std::move(value))) {
            ++_size;
        } else {
            // ΛΟγικά δεν θα έπρεπε να φτάσουμε εδώ ποτέ, 
            // αλλά αν συμβεί κάνουμε rehash.
            rehash(std::move(key), std::move(value));
        }
    }
};

}