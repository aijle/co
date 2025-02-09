#include "co/mem.h"
#include "co/atomic.h"
#include "co/god.h"
#include "co/log.h"
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memoryapi.h>
#include <intrin.h>
#else
#include <sys/mman.h>
#endif


#ifdef _WIN32
inline void* _vm_reserve(size_t n) {
    return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE);
}

inline void _vm_commit(void* p, size_t n) {
    void* x = VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE);
    assert(x == p); (void)x;
}

inline void _vm_decommit(void* p, size_t n) {
    VirtualFree(p, n, MEM_DECOMMIT);
}

inline void _vm_free(void* p, size_t n) {
    VirtualFree(p, 0, MEM_RELEASE);
}

#if __arch64
inline int _find_msb(size_t x) { /* x != 0 */
    unsigned long i;
    _BitScanReverse64(&i, x);
    return (int)i;
}

inline uint32 _find_lsb(size_t x) { /* x != 0 */
    unsigned long r;
    _BitScanForward64(&r, x);
    return r;
}

#else
inline int _find_msb(size_t x) { /* x != 0 */
    unsigned long i;
    _BitScanReverse(&i, x);
    return (int)i;
}

inline uint32 _find_lsb(size_t x) { /* x != 0 */
    unsigned long r;
    _BitScanForward(&r, x);
    return r;
}
#endif

inline uint32 _pow2_align(uint32 n) {
    unsigned long r;
    _BitScanReverse(&r, n - 1);
    return 2u << r;
}

#else
#include <sys/mman.h>

inline void* _vm_reserve(size_t n) {
    return ::mmap(
        NULL, n, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0
    );
}

inline void _vm_commit(void* p, size_t n) {
    void* x = ::mmap(
        p, n, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0
    );
    assert(x == p); (void)x;
}

inline void _vm_decommit(void* p, size_t n) {
    ::mmap(
        p, n, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0
    );
}

inline void _vm_free(void* p, size_t n) {
    ::munmap(p, n);
}

#if __arch64
inline int _find_msb(size_t x) { /* x != 0 */
    return 63 - __builtin_clzll(x);
}

inline uint32 _find_lsb(size_t x) { /* x != 0 */
    return __builtin_ffsll(x) - 1;
}

#else
inline int _find_msb(size_t v) { /* x != 0 */
    return 31 - __builtin_clz(v);
}

inline uint32 _find_lsb(size_t x) { /* x != 0 */
    return __builtin_ffs(x) - 1;
}
#endif

inline uint32 _pow2_align(uint32 n) {
    return 1u << (32 - __builtin_clz(n - 1));
}

#endif


namespace co {
namespace xx {

class StaticAllocator {
  public:
    static const size_t N = 64 * 1024; // block size

    StaticAllocator() : _p(0), _e(0) {}

    void* alloc(size_t n);

  private:
    char* _p;
    char* _e;
};

inline void* StaticAllocator::alloc(size_t n) {
    static size_t k = 0;
    n = god::align_up<8>(n);
    k += n;
    if (_p + n <= _e) return god::fetch_add(&_p, n);

    if (n <= 4096) {
        _p = (char*) ::malloc(N); assert(_p);
        _e = _p + N;
        return god::fetch_add(&_p, n);
    }

    return ::malloc(n);
}


#if __arch64
static const uint32 B = 6;
static const uint32 g_array_size = 32;
#else
static const uint32 B = 5;
static const uint32 g_array_size = 4;
#endif

static const uint32 R = (1 << B) - 1;
static const size_t C = (size_t)1;
static const uint32 g_sb_bits = 15;            // bit size of small block
static const uint32 g_lb_bits = g_sb_bits + B; // bit size of large block
static const uint32 g_hb_bits = g_lb_bits + B; // bit size of huge block
static const size_t g_max_alloc_size = 1u << 17; // 128k

class Bitset {
  public:
    explicit Bitset(void* s) : _s((size_t*)s) {}

    void set(uint32 i) {
        _s[i >> B] |= (C << (i & R));
    }

    void unset(uint32 i) {
        _s[i >> B] &= ~(C << (i & R));
    }

    bool test_and_unset(uint32 i) {
        const size_t x = (C << (i & R));
        return god::fetch_and(&_s[i >> B], ~x) & x;
    }

    // find for a bit from MSB to LSB, starts from position @i
    int rfind(uint32 i) const {
        int n = static_cast<int>(i >> B);
        do {
            const size_t x = _s[n];
            if (x) return _find_msb(x) + (n << B);
        } while (--n >= 0);
        return -1;
    }

    void atomic_set(uint32 i) {
        atomic_or(&_s[i >> B], C << (i & R), mo_relaxed);
    }

  private:
    size_t* _s;
};

// 128M on arch64, or 32M on arch32
// manage and alloc large blocks(2M or 1M)
class HugeBlock;

// 2M on arch64, or 1M on arch32
// manage and alloc small blocks(32K)
class LargeBlock;

// LargeAlloc is a large block, it allocates memory from 4K to 128K(64K) bytes
class LargeAlloc;

// SmallAlloc is a small block, it allocates memory from 16 to 2K bytes
class SmallAlloc;

// manage huge blocks, and alloc large blocks
//   - shared by all threads
class GlobalAlloc;

// thread-local allocator
class ThreadAlloc;

__thread ThreadAlloc* g_thread_alloc = NULL;

class HugeBlock {
  public:
    explicit HugeBlock(void* p) : _p((char*)p), _bits(0) {
        (void)_next; (void)_prev;
    }

    void* alloc(); // alloc a sub block
    bool free(void* p);

  private:
    HugeBlock* _next;
    HugeBlock* _prev;
    char* _p; // beginning address to alloc
    size_t _bits;
    DISALLOW_COPY_AND_ASSIGN(HugeBlock);
};

class LargeBlock {
  public:
    explicit LargeBlock(HugeBlock* parent)
        : _parent(parent), _p((char*)this + (1u << g_sb_bits)), _bits(0) {
        (void)_next; (void)_prev;
    }

    void* alloc(); // alloc a sub block
    bool free(void* p);
    SmallAlloc* make_small_alloc();
    HugeBlock* parent() const { return _parent; }

  private:
    LargeBlock* _next;
    LargeBlock* _prev;
    HugeBlock* _parent;
    char* _p; // beginning address to alloc
    size_t _bits;
    DISALLOW_COPY_AND_ASSIGN(LargeBlock);
};

class LargeAlloc {
  public:
    static const uint32 BS_BITS = 1u << (g_lb_bits - 12);
    static const uint32 LA_SIZE = 64;
    static const uint32 MAX_bIT = BS_BITS - 1;

    explicit LargeAlloc(HugeBlock* parent)
        : _parent(parent), _ta(g_thread_alloc), _cur_bit(0) {
        static_assert(sizeof(*this) <= LA_SIZE, "");
        _p = (char*)this + 4096;
        _pbs = (char*)this + LA_SIZE;
        _xpbs = (char*)this + (LA_SIZE + (BS_BITS >> 3));
        (void)_next; (void)_prev;
    }

    // alloc n units
    void* alloc(uint32 n);
    void* try_hard_alloc(uint32 n);
    bool free(void* p);
    void xfree(void* p);
    void* realloc(void* p, uint32 o, uint32 n);

    HugeBlock* parent() const { return _parent; }
    ThreadAlloc* thread_alloc() const { return _ta; }

  private:
    LargeAlloc* _next;
    LargeAlloc* _prev;
    HugeBlock* _parent;
    ThreadAlloc* _ta;
    char* _p; // beginning address to alloc
    union {
        Bitset _bs;
        char* _pbs;
    };
    union {
        Bitset _xbs;
        char* _xpbs;
    };
    uint32 _cur_bit;
    DISALLOW_COPY_AND_ASSIGN(LargeAlloc);
};

class SmallAlloc {
  public:
    static const uint32 BS_BITS = 1u << (g_sb_bits - 4); // 2048
    static const uint32 SA_SIZE = 64;
    static const uint32 MAX_bIT = BS_BITS - ((SA_SIZE + (BS_BITS >> 2)) >> 4);

    explicit SmallAlloc(LargeBlock* parent)
        : _next(0), _prev(0), _parent(parent), _ta(g_thread_alloc), _cur_bit(0) {
        static_assert(sizeof(*this) <= SA_SIZE, "");
        _p = (char*)this + (SA_SIZE + (BS_BITS >> 2));
        _pbs = (char*)this + SA_SIZE;
        _xpbs = (char*)this + (SA_SIZE + (BS_BITS >> 3));
        (void)_next; (void)_prev;
    }

    // alloc n units
    void* alloc(uint32 n);
    void* try_hard_alloc(uint32 n);
    bool free(void* p);
    void xfree(void* p);
    void* realloc(void* p, uint32 o, uint32 n);

    LargeBlock* parent() const { return _parent; }
    ThreadAlloc* thread_alloc() const { return _ta; }

  private:
    SmallAlloc* _next;
    SmallAlloc* _prev;
    LargeBlock* _parent;
    ThreadAlloc* _ta;
    char* _p; // beginning address to alloc
    union {
        Bitset _bs;
        char* _pbs;
    };
    union {
        Bitset _xbs;
        char* _xpbs;
    };
    uint32 _cur_bit;
    DISALLOW_COPY_AND_ASSIGN(SmallAlloc);
};

class GlobalAlloc {
  public:
    struct _X {
        _X() : mtx(), hb(0) {}
        std::mutex mtx;
        HugeBlock* hb;
    };

    void* alloc(uint32 alloc_id, HugeBlock** parent);
    LargeBlock* make_large_block(uint32 alloc_id);
    LargeAlloc* make_large_alloc(uint32 alloc_id);
    void free(void* p, HugeBlock* hb, uint32 alloc_id);

  private:
    _X _x[g_array_size];
};

class ThreadAlloc {
  public:
    ThreadAlloc()
        : _lb(0), _la(0), _sa(0) {
        static uint32 g_alloc_id = (uint32)-1;
        _id = atomic_inc(&g_alloc_id, mo_relaxed);
    }

    uint32 id() const { return _id; }
    void* static_alloc(size_t n) { return _ka.alloc(n); }
    void* alloc(size_t n);
    void free(void* p, size_t n);
    void* realloc(void* p, size_t o, size_t n);

  private:
    LargeBlock* _lb;
    LargeAlloc* _la;
    SmallAlloc* _sa;
    uint32 _id;
    StaticAllocator _ka;
};


inline GlobalAlloc* galloc() {
    static GlobalAlloc* ga = new GlobalAlloc();
    return ga;
}

inline ThreadAlloc* thread_alloc() {
    return g_thread_alloc ? g_thread_alloc : (g_thread_alloc = new ThreadAlloc());
}


struct DoubleLink {
    DoubleLink* next;
    DoubleLink* prev;
};

typedef DoubleLink* list_t;

inline void list_push_front(list_t& l, DoubleLink* node) {
    if (l) {
        node->next = l;
        node->prev = l->prev;
        l->prev = node;
        l = node;
    } else {
        node->next = NULL;
        node->prev = node;
        l = node;
    }
}

// move non-tailing node to the front
inline void list_move_front(list_t& l, DoubleLink* node) {
    if (node != l) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node->prev = l->prev;
        node->next = l;
        l->prev = node;
        l = node;
    }
}

// move heading node to the back
inline void list_move_head_back(list_t& l) {
    const auto head = l->next;
    l->prev->next = l;
    l->next = NULL;
    l = head;
}

// erase non-heading node
inline void list_erase(list_t& l, DoubleLink* node) {
    node->prev->next = node->next;
    const auto x = node->next ? node->next : l;
    x->prev = node->prev;
}


inline void* HugeBlock::alloc() {
    char* p = NULL;
    const uint32 i = _find_lsb(~_bits);
    if (i < R) {
        _bits |= (C << i);
        p = _p + (((size_t)i) << g_lb_bits);
    }
    return p;
}

inline bool HugeBlock::free(void* p) {
    const uint32 i = (uint32)(((char*)p - _p) >> g_lb_bits);
    return (_bits &= ~(C << i)) == 0;
}

inline HugeBlock* make_huge_block() {
    void* x = _vm_reserve(1u << g_hb_bits);
    if (x) {
        _vm_commit(x, 4096);
        void* p = god::align_up<(1u << g_lb_bits)>(x);
        if (p == x) p = (char*)x + (1u << g_lb_bits);
        return new (x) HugeBlock(p);
    }
    return NULL;
}


#define _try_alloc(l, n) \
    DoubleLink* const h = l; \
    DoubleLink* k = l->next; \
    list_move_head_back(l); \
    for (int i = 0; i < n && k != h; k = k->next, ++i)

inline void* GlobalAlloc::alloc(uint32 alloc_id, HugeBlock** parent) {
    void* p = NULL;
    auto& x = _x[alloc_id & (g_array_size - 1)];

    do {
        std::lock_guard<std::mutex> g(x.mtx);
        if (x.hb && (p = x.hb->alloc())) {
            *parent = x.hb;
            goto end;
        }
        {
            auto& l = (list_t&)x.hb;
            if (l && l->next) {
                _try_alloc(l, 8) {
                    if ((p = ((HugeBlock*)k)->alloc())) {
                        *parent = (HugeBlock*)k;
                        list_move_front(l, k);
                        goto end;
                    }
                }
            }
        }
        {
            auto hb = make_huge_block();
            if (hb) {
                list_push_front((list_t&)x.hb, (DoubleLink*)hb);
                p = hb->alloc();
                *parent = hb;
            }
        }
    } while (0);

  end:
    if (p) _vm_commit(p, 1u << g_lb_bits);
    return p;
}

inline LargeBlock* GlobalAlloc::make_large_block(uint32 alloc_id) {
    HugeBlock* parent;
    auto p = this->alloc(alloc_id, &parent);
    return p ? new (p) LargeBlock(parent) : NULL;
}

inline LargeAlloc* GlobalAlloc::make_large_alloc(uint32 alloc_id) {
    HugeBlock* parent;
    auto p = this->alloc(alloc_id, &parent);
    return p ? new (p) LargeAlloc(parent) : NULL;
}

inline void GlobalAlloc::free(void* p, HugeBlock* hb, uint32 alloc_id) {
    _vm_decommit(p, 1u << g_lb_bits);
    auto& x = _x[alloc_id & (g_array_size - 1)];
    bool r;
    {
        std::lock_guard<std::mutex> g(x.mtx);
        r = hb->free(p) && hb != x.hb;
        if (r) list_erase((list_t&)x.hb, (DoubleLink*)hb);
    }
    if (r) _vm_free(hb, 1u << g_hb_bits);
}


inline void* LargeBlock::alloc() {
    void* p = NULL;
    const uint32 i = _find_lsb(~_bits);
    if (i < R) {
        _bits |= (C << i);
        p = _p + (((size_t)i) << g_sb_bits);
    }
    return p;
}

inline bool LargeBlock::free(void* p) {
    const uint32 i = (uint32)(((char*)p - _p) >> g_sb_bits);
    return (_bits &= ~(C << i)) == 0;
}

inline SmallAlloc* LargeBlock::make_small_alloc() {
    auto x = this->alloc();
    return x ? new (x) SmallAlloc(this) : NULL;
}


void* LargeAlloc::try_hard_alloc(uint32 n) {
    size_t* const p = (size_t*)_pbs;
    size_t* const q = (size_t*)_xpbs;

    const int M = _cur_bit >> B;
    for (int i = M; i >= 0; --i) {
        const size_t x = atomic_load(&q[i], mo_relaxed);
        if (x) {
            atomic_and(&q[i], ~x, mo_relaxed);
            p[i] &= ~x;
            const int lsb = static_cast<int>(_find_lsb(x) + (i << B));
            const int r = _bs.rfind(_cur_bit);
            if (r >= lsb) break;
            _cur_bit = r >= 0 ? lsb : 0;
            if (_cur_bit == 0) break;
        }
    }

    if (_cur_bit + n <= MAX_bIT) {
        _bs.set(_cur_bit);
        return _p + (god::fetch_add(&_cur_bit, n) << 12);
    }
    return NULL;
}

inline void* LargeAlloc::alloc(uint32 n) {
    if (_cur_bit + n <= MAX_bIT) {
        _bs.set(_cur_bit);
        return _p + (god::fetch_add(&_cur_bit, n) << 12);
    }
    return NULL;
}

inline bool LargeAlloc::free(void* p) {
    int i = (int)(((char*)p - _p) >> 12);
    //CHECK(_bs.test_and_unset((uint32)i)) << "free invalid pointer: " << p;
    _bs.unset(i);
    const int r = _bs.rfind(_cur_bit);
    return r < i ? ((_cur_bit = r >= 0 ? i : 0) == 0) : false;
}

inline void LargeAlloc::xfree(void* p) {
    const uint32 i = (uint32)(((char*)p - _p) >> 12);
    _xbs.atomic_set(i);
}

inline void* LargeAlloc::realloc(void* p, uint32 o, uint32 n) {
    uint32 i = (uint32)(((char*)p - _p) >> 12);
    if (_cur_bit == i + o && i + n <= MAX_bIT) {
        _cur_bit = i + n;
        return p;
    }
    return NULL;
}


void* SmallAlloc::try_hard_alloc(uint32 n) {
    size_t* const p = (size_t*)_pbs;
    size_t* const q = (size_t*)_xpbs;

    const int M = _cur_bit >> B;
    for (int i = M; i >= 0; --i) {
        const size_t x = atomic_load(&q[i], mo_relaxed);
        if (x) {
            atomic_and(&q[i], ~x, mo_relaxed);
            p[i] &= ~x;
            const int lsb = static_cast<int>(_find_lsb(x) + (i << B));
            const int r = _bs.rfind(_cur_bit);
            if (r >= lsb) break;
            _cur_bit = r >= 0 ? lsb : 0;
            if (_cur_bit == 0) break;
        }
    }

    if (_cur_bit + n <= MAX_bIT) {
        _bs.set(_cur_bit);
        return _p + (god::fetch_add(&_cur_bit, n) << 4);
    }
    return NULL;
}

inline void* SmallAlloc::alloc(uint32 n) {
    if (_cur_bit + n <= MAX_bIT) {
        _bs.set(_cur_bit);
        return _p + (god::fetch_add(&_cur_bit, n) << 4);
    }
    return NULL;
}

inline bool SmallAlloc::free(void* p) {
    const int i = (int)(((char*)p - _p) >> 4);
    //CHECK(_bs.test_and_unset((uint32)i)) << "free invalid pointer: " << p;
    _bs.unset(i);
    const int r = _bs.rfind(_cur_bit);
    return r < i ? ((_cur_bit = r >= 0 ? i : 0) == 0) : false;
}

inline void SmallAlloc::xfree(void* p) {
    const uint32 i = (uint32)(((char*)p - _p) >> 4);
    _xbs.atomic_set(i);
}

inline void* SmallAlloc::realloc(void* p, uint32 o, uint32 n) {
    uint32 i = (uint32)(((char*)p - _p) >> 4);
    if (_cur_bit == i + o && i + n <= MAX_bIT) {
        _cur_bit = i + n;
        return p;
    }
    return NULL;
}


inline void* ThreadAlloc::alloc(size_t n) {
    void* p = 0;
    SmallAlloc* sa;
    if (n <= 2048) {
        const uint32 u = n > 16 ? (god::align_up<16>((uint32)n) >> 4) : 1;
        if (_sa && (p = _sa->alloc(u))) goto end;
        {
            auto& l = (list_t&)_sa;
            if (l && l->next) {
                //list_move_head_back(l);
                //if ((p = _sa->try_hard_alloc(u))) goto end;
                //list_move_head_back(l);
                _try_alloc(l, 2) {
                    if ((p = ((SmallAlloc*)k)->try_hard_alloc(u))) {
                        list_move_front(l, k);
                        goto end;
                    }
                }
            }
        }

        if (_lb && (sa = _lb->make_small_alloc())) {
            list_push_front((list_t&)_sa, (DoubleLink*)sa);
            p = sa->alloc(u);
            goto end;
        }

        {
            auto& l = (list_t&)_lb;
            if (l && l->next) {
                _try_alloc(l, 4) {
                    if ((sa = ((LargeBlock*)k)->make_small_alloc())) {
                        list_move_front(l, k);
                        p = sa->alloc(u);
                        goto end;
                    }
                }
            }

            auto lb = galloc()->make_large_block(_id);
            if (lb) {
                list_push_front(l, (DoubleLink*)lb);
                sa = lb->make_small_alloc();
                list_push_front((list_t&)_sa, (DoubleLink*)sa);
                p = sa->alloc(u);
            }
            goto end;
        }

    } else if (n <= g_max_alloc_size) {
        const uint32 u = god::align_up<4096>((uint32)n) >> 12;
        if (_la && (p = _la->alloc(u))) goto end;

        {
            auto& l = (list_t&)_la;
            if (l && l->next) {
                _try_alloc(l, 4) {
                    if ((p = ((LargeAlloc*)k)->try_hard_alloc(u))) {
                        list_move_front(l, k);
                        goto end;
                    }
                }
            }

            auto la = galloc()->make_large_alloc(_id);
            if (la) {
                list_push_front(l, (DoubleLink*)la);
                p = la->alloc(u);
            }
            goto end;
        }

    } else {
        p = ::malloc(n);
    }

  end:
    return p;
}

inline void ThreadAlloc::free(void* p, size_t n) {
    if (p) {
        if (n <= 2048) {
            const auto sa = (SmallAlloc*) god::align_down<1u << g_sb_bits>(p);
            const auto ta = sa->thread_alloc();
            if (ta == this) {
                if (sa->free(p) && sa != _sa) {
                    list_erase((list_t&)_sa, (DoubleLink*)sa);
                    const auto lb = sa->parent();
                    if (lb->free(sa) && lb != _lb) {
                        list_erase((list_t&)_lb, (DoubleLink*)lb);
                        galloc()->free(lb, lb->parent(), _id);
                    }
                }
            } else {
                sa->xfree(p);
            }

        } else if (n <= g_max_alloc_size) {
            const auto la = (LargeAlloc*) god::align_down<1u << g_lb_bits>(p);
            const auto ta = la->thread_alloc();
            if (ta == this) {
                if (la->free(p) && la != _la) {
                    list_erase((list_t&)_la, (DoubleLink*)la);
                    galloc()->free(la, la->parent(), _id);
                }
            } else {
                la->xfree(p);
            }

        } else {
            ::free(p);
        }
    }
}

inline void* ThreadAlloc::realloc(void* p, size_t o, size_t n) {
    if (unlikely(!p)) return this->alloc(n);
    if (unlikely(o > g_max_alloc_size)) return ::realloc(p, n);
    CHECK_LT(o, n) << "realloc error, new size must be greater than old size..";

    if (o <= 2048) {
        const uint32 k = (o > 16 ? god::align_up<16>((uint32)o) : 16);
        if (n <= (size_t)k) return p;

        const auto sa = (SmallAlloc*) god::align_down<1u << g_sb_bits>(p);
        if (sa == _sa && n <= 2048) {
            const uint32 l = god::align_up<16>((uint32)n);
            auto x = sa->realloc(p, k >> 4, l >> 4);
            if (x) return x;
        }

    } else {
        const uint32 k = god::align_up<4096>((uint32)o);
        if (n <= (size_t)k) return p;

        const auto la = (LargeAlloc*) god::align_down<1u << g_lb_bits>(p);
        if (la == _la && n <= g_max_alloc_size) {
            const uint32 l = god::align_up<4096>((uint32)n);
            auto x = la->realloc(p, k >> 12, l >> 12);
            if (x) return x;
        }
    }

    auto x = this->alloc(n);
    if (x) { memcpy(x, p, o); this->free(p, o); }
    return x;
}

} // xx

#ifndef CO_USE_SYS_MALLOC
void* static_alloc(size_t n) {
    return xx::thread_alloc()->static_alloc(n);
}

void* alloc(size_t n) {
    return xx::thread_alloc()->alloc(n);
}

void free(void* p, size_t n) {
    return xx::thread_alloc()->free(p, n);
}

void* realloc(void* p, size_t o, size_t n) {
    return xx::thread_alloc()->realloc(p, o, n);
}

#else
void* static_alloc(size_t n) { return ::malloc(n); }
void* alloc(size_t n) { return ::malloc(n); }
void free(void* p, size_t) { ::free(p); }
void* realloc(void* p, size_t, size_t n) { return ::realloc(p, n); }
#endif

void* zalloc(size_t size) {
    auto p = co::alloc(size);
    if (p) memset(p, 0, size);
    return p;
}

} // co
