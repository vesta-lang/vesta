/**
 * @file util/oa_u64_map.h
 * @brief Mapa hash open-addressing generico con clave @c uint64_t.
 *
 * Reemplaza @c std::unordered_map<uint64_t, V> en codigo que debe compilar
 * FREESTANDING (sin libstdc++ @c _Prime_rehash_policy) y/o en hot paths donde
 * la localidad de cache importa.  Razones (identicas a los mapas custom del
 * GC): (a) sin malloc por nodo -> sin la dependencia de @c operator @c new
 * node-based de @c std::unordered_map; (b) tabla contigua -> cache-friendly;
 * (c) mas rapido en find/insert (~5 ns vs ~20-30 ns).  Universal: el mismo
 * tipo sirve para la VM, el JIT y el AOT, asi que NO hay perdida de
 * rendimiento al unificar.
 *
 * Usa 3 estados por slot (EMPTY/FULL/TOMB) en un byte aparte, por lo que
 * CUALQUIER valor de clave (incluido 0 y @c UINT64_MAX) es valido.
 */
#ifndef VESTA_UTIL_OA_U64_MAP_H
#define VESTA_UTIL_OA_U64_MAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace util {

/**
 * @class OAU64Map
 * @brief Mapa open-addressing (linear probing) clave @c uint64_t -> @p V.
 * @tparam V Tipo del valor almacenado.
 */
template <typename V> class OAU64Map {
  public:
    /// Estado de un slot de la tabla.
    enum SlotState : uint8_t { EMPTY = 0, FULL = 1, TOMB = 2 };

    /// Slot de la tabla: clave + valor + estado.
    struct Slot {
        uint64_t key = 0;
        V val{};
        uint8_t st = EMPTY;
    };

    OAU64Map() { rehash_to(8); }

    /// Devuelve puntero al valor de @p k, o nullptr si no esta.
    V *find(uint64_t k) noexcept {
        size_t i = hash(k) & mask_;
        for (;;) {
            Slot &s = table_[i];
            if (s.st == EMPTY) return nullptr;
            if (s.st == FULL && s.key == k) return &s.val;
            i = (i + 1) & mask_;
        }
    }
    /// Sobrecarga const.
    const V *find(uint64_t k) const noexcept {
        size_t i = hash(k) & mask_;
        for (;;) {
            const Slot &s = table_[i];
            if (s.st == EMPTY) return nullptr;
            if (s.st == FULL && s.key == k) return &s.val;
            i = (i + 1) & mask_;
        }
    }

    /// Acceso/insercion: devuelve el valor de @p k, creandolo si no existe.
    V &operator[](uint64_t k) {
        if (used_ + 1 > grow_at_) grow();
        size_t i = hash(k) & mask_;
        size_t first_tomb = SIZE_MAX;
        for (;;) {
            Slot &s = table_[i];
            if (s.st == EMPTY) {
                Slot &dst = (first_tomb != SIZE_MAX) ? table_[first_tomb] : s;
                if (dst.st != TOMB) ++used_; // EMPTY consume un slot nuevo
                dst.key = k;
                dst.val = V{};
                dst.st = FULL;
                ++live_;
                return dst.val;
            }
            if (s.st == TOMB) {
                if (first_tomb == SIZE_MAX) first_tomb = i;
            } else if (s.key == k) {
                return s.val;
            }
            i = (i + 1) & mask_;
        }
    }

    /// Borra la entrada de @p k.  Devuelve true si existia.
    bool erase(uint64_t k) {
        size_t i = hash(k) & mask_;
        for (;;) {
            Slot &s = table_[i];
            if (s.st == EMPTY) return false;
            if (s.st == FULL && s.key == k) {
                s.st = TOMB;
                s.val = V{};
                --live_;
                return true;
            }
            i = (i + 1) & mask_;
        }
    }

    bool empty() const noexcept { return live_ == 0; }
    size_t size() const noexcept { return live_; }
    void clear() { rehash_to(8); }

    // --- Iteracion (salta EMPTY/TOMB) -------------------------------------
    template <typename SlotT, typename TableT> class IterImpl {
      public:
        IterImpl(TableT *t, size_t i) : t_(t), i_(i) { skip(); }
        SlotT &operator*() const { return (*t_)[i_]; }
        SlotT *operator->() const { return &(*t_)[i_]; }
        IterImpl &operator++() {
            ++i_;
            skip();
            return *this;
        }
        bool operator!=(const IterImpl &o) const { return i_ != o.i_; }

      private:
        void skip() {
            while (i_ < t_->size() && (*t_)[i_].st != FULL)
                ++i_;
        }
        TableT *t_;
        size_t i_;
    };
    using iterator = IterImpl<Slot, std::vector<Slot>>;
    using const_iterator = IterImpl<const Slot, const std::vector<Slot>>;

    iterator begin() { return iterator(&table_, 0); }
    iterator end() { return iterator(&table_, table_.size()); }
    const_iterator begin() const { return const_iterator(&table_, 0); }
    const_iterator end() const {
        return const_iterator(&table_, table_.size());
    }

  private:
    static inline size_t hash(uint64_t k) noexcept {
        k *= 0x9E3779B97F4A7C15ull;
        return static_cast<size_t>(k >> 32);
    }
    void rehash_to(size_t cap) {
        table_.assign(cap, Slot{});
        mask_ = cap - 1;
        grow_at_ = (cap * 3) / 4;
        used_ = 0;
        live_ = 0;
    }
    void grow() {
        std::vector<Slot> old = std::move(table_);
        rehash_to((mask_ + 1) * 2);
        for (Slot &s : old)
            if (s.st == FULL) (*this)[s.key] = std::move(s.val);
    }

    std::vector<Slot> table_;
    size_t mask_ = 0, live_ = 0, used_ = 0, grow_at_ = 0;
};

} // namespace util

#endif // VESTA_UTIL_OA_U64_MAP_H
