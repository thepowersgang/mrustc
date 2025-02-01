/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * rc_string.cpp
 * - Reference-counted string
 */
#include <rc_string.hpp>
#include <cstring>
#include <string>
#include <iostream>
#include <algorithm>    // std::max

RcString::RcString(const char* s, size_t len):
    m_ptr(nullptr)
{
    if( len > 0 )
    {
        size_t nwords = (len+1 + sizeof(unsigned int)-1) / sizeof(unsigned int);
        m_ptr = reinterpret_cast<Inner*>(malloc(sizeof(Inner) + (nwords - 1) * sizeof(unsigned int)));
        m_ptr->refcount = 1;
        m_ptr->size = static_cast<unsigned>(len);
        m_ptr->ordering = 0;
        char* data_mut = reinterpret_cast<char*>(m_ptr->data);
        for(unsigned int j = 0; j < len; j ++ )
            data_mut[j] = s[j];
        data_mut[len] = '\0';
    }
}
RcString::~RcString()
{
    if(m_ptr)
    {
        m_ptr->refcount -= 1;
        //::std::cout << "RcString(" << m_ptr << " \"" << *this << "\") - " << *m_ptr << " refs left (drop)" << ::std::endl;
        if( m_ptr->refcount == 0 )
        {
            free(m_ptr);
        }
        m_ptr = nullptr;
    }
}
Ordering RcString::ord(const char* s, size_t len) const
{    
    auto cmp_len = ::std::min(len, this->size());
    if( cmp_len > 0 )
    {
        int cmp = memcmp(this->c_str(), s, cmp_len);
        if(cmp != 0) {
            return ::ord(cmp, 0);
        }
    }
    // Since the prefix is equal, then sort `this` before `s` if it's shorter
    return ::ord(this->size(), len);
}
Ordering RcString::ord(const char* s) const
{
    if( m_ptr == nullptr )
        return (*s == '\0' ? OrdEqual : OrdLess);

    int cmp = strncmp(this->c_str(), s, this->size());
    if( cmp == 0 )
    {
        if( s[this->size()] == '\0' )
            return OrdEqual;
        else
            return OrdLess;
    }
    return ::ord(cmp, 0);
}

::std::ostream& operator<<(::std::ostream& os, const RcString& x)
{
    for(size_t i = 0; i < x.size(); i ++)
    {
        os << x.c_str()[i];
    }
    return os;
}


// Replace the use of `std::set` with a collection of sorted buffers
// Limit each entry to ~1024 items, and split in half when full.
// - This limits the cost of insertion to just needing to move a maximum of 1024 items plus the ~170 items in the outer list (assuming an average of 75% usage)
// - Numbers: libcargo 1.74 has 128,900 interned strings (of which 115,984 are in use at Trans), hence the above estimate of 170 blocks of 1024
namespace {
    struct StringView {
        const char* p;
        size_t l;

        operator RcString() const {
            return RcString(p, l);
        }
    };
    struct Cmp_RcString_Raw {
        bool operator()(const RcString& a, const RcString& b) const {
            return a.ord(b.c_str(), b.size()) == OrdLess;
        }
        bool operator()(const RcString& a, StringView& b) const {
            return a.ord(b.p, b.l) == OrdLess;
        }
    };

    // This is faster than std::set, as it doesn't have to allocate `RcString` instances, and it has lower memory overhead
    const size_t BLOCK_SIZE = 1024;
    class TieredSet {
        struct Block {
            std::vector<RcString>   ents;
            Block() {
                ents.reserve(BLOCK_SIZE);
            }
        };
        std::vector<Block>  blocks;
    public:
        TieredSet()
        {
            blocks.reserve(150'000 * 3 / BLOCK_SIZE / 2);
        }

        std::pair<const RcString*,bool> lookup_or_add(const StringView& sv)
        {
            // Special case: empty collection
            if( blocks.empty() ) {
                blocks.push_back(Block());
                blocks.front().ents.push_back(RcString(sv));
                return ::std::make_pair(&blocks.front().ents.front(), true);
            }

            // Find the block that starts with an element after this string
            auto maybe_after = ::std::lower_bound(blocks.begin(), blocks.end(), sv, [](const Block& b, const StringView& sv) {
                return b.ents.front().ord(sv.p, sv.l) == OrdLess;
                });

            if( maybe_after != blocks.end() && maybe_after->ents.front().ord(sv.p, sv.l) == OrdEqual ) {
                return std::make_pair(&maybe_after->ents.front(), false);
            }
            // Special case: The first block sorts after this string, so we need to add the new string to the start of it (or to a new block before)
            else if( maybe_after == blocks.begin() ) {
                return insert_into_block(maybe_after, maybe_after->ents.begin(), RcString(sv));
            }
            // Since the string sorts before the beginning of `maybe_after`, it should be in (or be added to) the previous block
            else {
                auto maybe_block = maybe_after - 1;
                auto& ents = maybe_block->ents;
                auto maybe_pos = std::lower_bound(ents.begin(), ents.end(), sv, [](const RcString& s, const StringView& sv){ return s.ord(sv.p, sv.l) == OrdLess; });
                if( maybe_pos != ents.end() && maybe_pos->ord(sv.p, sv.l) == OrdEqual ) {
                    return std::make_pair(&*maybe_pos, false);
                }
                else {
                    // Not equal, so it has to be above - so insert
                    return insert_into_block(maybe_block, maybe_pos, RcString(sv));
                }
            }
        }

        struct It {
            std::vector<Block>::iterator block, block_e;
            std::vector<RcString>::iterator slot;

            RcString& operator*() {
                assert(block != block_e);
                assert(slot != block->ents.end());
                return *slot;
            }
            It& operator++() {
                assert(block != block_e);
                assert(slot != block->ents.end());
                ++ slot;
                if( slot == block->ents.end() ) {
                    ++ block;
                    if( block != block_e ) {
                        slot = block->ents.begin();
                    }
                }
                return *this;
            }
            bool operator!=(const It& x) const {
                return block != x.block || slot != x.slot;
            }
        };
        It begin() {
            return It {
                blocks.begin(), blocks.end(),
                blocks.front().ents.begin()
            };
        }
        It end() {
            return It {
                blocks.end(), blocks.end(),
                blocks.back().ents.end()
            };
        }
    private:
        std::pair<const RcString*,bool> insert_into_block(std::vector<Block>::iterator block, std::vector<RcString>::iterator slot, RcString rv) {
            if( block->ents.size() == block->ents.capacity() ) {
                // Block is full, so create a new block and split the contents between the two
                // - The new block should go after the current one, and get half of its contents
                auto new_block = blocks.insert(block + 1, Block());
                block = new_block - 1;
                const auto split_point = block->ents.size() / 2;
                if( static_cast<size_t>(slot - block->ents.begin()) >= split_point ) {
                    // The target location is in the second half of the range, so we're inserting into the new block
                    new_block->ents.insert(new_block->ents.end(), block->ents.begin() + split_point, slot);
                    new_block->ents.push_back(rv);
                    slot = new_block->ents.insert(new_block->ents.end(), slot, block->ents.end()) - 1;
                    block->ents.resize(split_point);
                }
                else {
                    // Target is in the lower half, so copy the entities and then insert
                    new_block->ents.insert(new_block->ents.end(), block->ents.begin() + split_point, block->ents.end());
                    block->ents.resize(split_point);
                    slot = block->ents.insert(slot, rv);
                }
            }
            else {
                slot = block->ents.insert(slot, rv);
            }

#if 0
            StringView    prev { nullptr, 0 };
            for(auto& v : *this) {
                if( prev.p ) {
                    if( v.ord(prev.p, prev.l) > 0 ) {
                    }
                    else {
                        std::cerr << "BUG: Ordering lost after adding `" << rv << "` - '" << prev.p << "' and '" << v << "'\n";
                        abort();
                    }
                }
                prev.p = v.c_str();
                prev.l = v.size();
            }
#endif

            return std::make_pair(&*slot, true);
        }
    };
}
TieredSet   RcString_interned_strings;
bool    RcString_interned_ordering_valid;

RcString RcString::new_interned(const char* s, size_t len)
{
    if(len == 0)
        return RcString();
    auto ret = RcString_interned_strings.lookup_or_add(StringView { s, len });
    // Set interned and invalidate the cache if an insert happened
    if(ret.second)
    {
        ret.first->m_ptr->ordering = 1;
        RcString_interned_ordering_valid = false;
    }
    //assert( ret.first->ord(s, len) == 0 );
    return *ret.first;
}
Ordering RcString::ord_interned(const RcString& s) const
{
    assert(s.is_interned() && this->is_interned());
    if(!RcString_interned_ordering_valid)
    {
        // Populate cache
        unsigned i = 1;
        for(auto& e : RcString_interned_strings)
            e.m_ptr->ordering = i++;
        RcString_interned_ordering_valid = true;
    }
    return ::ord(this->m_ptr->ordering, s.m_ptr->ordering);
}

size_t std::hash<RcString>::operator()(const RcString& s) const noexcept
{
    // http://www.cse.yorku.ca/~oz/hash.html "djb2"
    size_t h = 5381;
    for(auto c : s) {
        h = h * 33 + (unsigned)c;
    }
    return h;
    //return hash<std::string_view>(s.c_str(), s.size());
}
