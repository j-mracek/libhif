/*
Copyright (C) 2020 Red Hat, Inc.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/


#ifndef LIBDNF_RPM_SOLV_MAP_HPP
#define LIBDNF_RPM_SOLV_MAP_HPP


#include "map_iterator.hpp"

#include <solv/bitmap.h>
#include <solv/pooltypes.h>

namespace libdnf::rpm {

class SolvSack;

}  // namespace libdnf::rpm

namespace libdnf::rpm::solv {


// clang-format off
// see http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetTable
static const unsigned char BIT_COUNT_LOOKUP[256] = {
    #define B2(n) n, n + 1, n + 1, n + 2
    #define B4(n) B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
    #define B6(n) B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
    B6(0), B6(1), B6(1), B6(2)
};
// clang-format on


class SolvMap {
public:
    using iterator = SolvMapIterator;
    iterator begin() const;
    iterator end() const;

    /// Initialize an empty Map of given size
    explicit SolvMap(int size);

    /// Clone from an existing Map
    explicit SolvMap(const Map * other);

    /// Copy constructor: clone from an existing SolvMap
    SolvMap(const SolvMap & other);

    ~SolvMap();

    // GENERIC OPERATIONS

    /// Return the underlying libsolv Map
    ///
    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.getMap()
    const Map * get_map() const;

    /// Return the number of solvables in the SolvMap (number of 1s in the bitmap).
    ///
    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.size()
    std::size_t size() const noexcept;

    bool empty() const;

    void clear();

    // ITEM OPERATIONS

    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.set(Id id)
    void add(PackageId package_id);

    /// Faster, but unsafe version of add() method that is doesn't check bitmap range
    void add_unsafe(PackageId package_id);

    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.has(Id id)
    bool contains(PackageId package_id) const;

    /// Faster, but unsafe version of contains() method that is doesn't check bitmap range
    bool contains_unsafe(PackageId package_id) const;

    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.remove(Id id)
    void remove(PackageId package_id);

    /// Faster, but unsafe version of remove() method that is doesn't check bitmap range
    void remove_unsafe(PackageId package_id);

    // SET OPERATIONS - Map

    /// Union operator
    ///
    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.operator+=(const Map * other)
    SolvMap & operator|=(const Map * other);

    /// Difference operator
    ///
    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.operator-=(const Map * other)
    SolvMap & operator-=(const Map * other);

    /// Intersection operator
    ///
    /// @replaces libdnf:sack/packageset.hpp:method:PackageSet.operator/=(const Map * other)
    SolvMap & operator&=(const Map * other);

    // SET OPERATIONS - SolvMap

    /// Union operator
    SolvMap & operator|=(const SolvMap & other);

    /// Difference operator
    SolvMap & operator-=(const SolvMap & other);

    /// Intersection operator
    SolvMap & operator&=(const SolvMap & other);

    SolvMap & operator=(const SolvMap & other);
    SolvMap & operator=(SolvMap && other) noexcept;

protected:
    /// Check if `id` is in bitmap range.
    /// Throws std::out_of_range
    void check_id_in_bitmap_range(PackageId package_id) const;

private:
    friend class libdnf::rpm::SolvSack;
    Map map;
};


inline SolvMap::SolvMap(int size) {
    map_init(&map, size);
}


inline SolvMap::SolvMap(const Map * other) {
    map_init_clone(&map, other);
}


inline SolvMap::SolvMap(const SolvMap & other) : SolvMap(other.get_map()) {}


inline SolvMap::~SolvMap() {
    map_free(&map);
}


inline void SolvMap::check_id_in_bitmap_range(PackageId package_id) const {
    // map.size is in bytes, << 3 multiplies the number with 8 and gives size in bits
    if (package_id.id < 0 || package_id.id >= (map.size << 3)) {
        throw std::out_of_range("Id is out of bitmap range");
    }
}


inline SolvMap::iterator SolvMap::begin() const {
    iterator it(&map);
    return it;
}


inline SolvMap::iterator SolvMap::end() const {
    iterator it(&map);
    it.end();
    return it;
}


inline void SolvMap::add(PackageId package_id) {
    check_id_in_bitmap_range(package_id);
    add_unsafe(package_id);
}


inline bool SolvMap::contains(PackageId package_id) const {
    if (package_id.id < 0 || package_id.id >= (map.size << 3)) {
        // if Id is outside bitmap range, then bitmap doesn't contain it
        return false;
    }
    return contains_unsafe(package_id);
}


inline void SolvMap::remove(PackageId package_id) {
    check_id_in_bitmap_range(package_id);
    remove_unsafe(package_id);
}


inline void SolvMap::add_unsafe(PackageId package_id) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
    MAPSET(&map, package_id.id);
#pragma GCC diagnostic pop
}


inline bool SolvMap::contains_unsafe(PackageId package_id) const {
    return MAPTST(&map, package_id.id);
}


inline void SolvMap::remove_unsafe(PackageId package_id) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
    MAPCLR(&map, package_id.id);
#pragma GCC diagnostic pop
}


inline const Map * SolvMap::get_map() const {
    return &map;
}


inline bool SolvMap::empty() const {
    const unsigned char * byte = map.map;
    const unsigned char * end = byte + map.size;

    // iterate through the whole bitmap by moving the address
    while (byte < end) {
        if (*byte++) {
            // return false if a non-zero bit was found
            return false;
        }
    }
    // all bits were zero, return true
    return true;
}


inline std::size_t SolvMap::size() const noexcept {
    unsigned char * byte = map.map;
    unsigned char * end = byte + map.size;
    std::size_t result = 0;

    // iterate through the whole bitmap by moving the address
    while (byte < end) {
        // add number of bits in each byte
        result += BIT_COUNT_LOOKUP[*byte++];
    }
    return result;
}


inline void SolvMap::clear() {
    map_empty(&map);
}


inline SolvMap & SolvMap::operator|=(const Map * other) {
    map_or(&map, const_cast<Map *>(other));
    return *this;
}


inline SolvMap & SolvMap::operator|=(const SolvMap & other) {
    *this |= other.get_map();
    return *this;
}


inline SolvMap & SolvMap::operator-=(const Map * other) {
    map_subtract(&map, const_cast<Map *>(other));
    return *this;
}


inline SolvMap & SolvMap::operator-=(const SolvMap & other) {
    *this -= other.get_map();
    return *this;
}


inline SolvMap & SolvMap::operator&=(const Map * other) {
    map_and(&map, other);
    return *this;
}


inline SolvMap & SolvMap::operator&=(const SolvMap & other) {
    *this &= other.get_map();
    return *this;
}

inline SolvMap & SolvMap::operator=(const SolvMap & other) {
    map_free(&map);
    map_init_clone(&map, &other.map);
    return *this;
}

inline SolvMap & SolvMap::operator=(SolvMap && other) noexcept {
    std::swap(map, other.map);
    return *this;
}

}  // namespace libdnf::rpm::solv


#endif  // LIBDNF_RPM_SOLV_MAP_HPP
