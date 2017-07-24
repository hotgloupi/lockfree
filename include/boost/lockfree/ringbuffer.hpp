//  lock-free single-producer/single-consumer ringbuffer
//  this algorithm is implemented in various projects (linux kernel)
//
//  Copyright (C) 2009-2013 Tim Blechmann
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_LOCKFREE_RING_BUFFER_HPP_INCLUDED
#define BOOST_LOCKFREE_RING_BUFFER_HPP_INCLUDED

#include <boost/config.hpp>
#include <boost/move/move.hpp>
#include <boost/lockfree/detail/atomic.hpp>

#include <limits>
#include <cstdlib>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace lockfree {

// Default policies used by basic_ringbuffer
struct default_ringbuffer_policies
{
    // buffer's byte type
    typedef std::uint8_t byte_t;

    // type used to represent ring size and indices
    typedef std::size_t ring_size_t;

    // type used to represent a buffer size
    typedef std::size_t buffer_size_t;

    // buffer type
    struct buffer
    {
        buffer_size_t size;
        byte_t data[];
    };

    // The size of an empty buffer
    BOOST_STATIC_CONSTEXPR buffer_size_t empty_buffer_size = sizeof(buffer);

    // a buffer_size > 0 means that we have fixed size buffers
    BOOST_STATIC_CONSTEXPR buffer_size_t buffer_size = 0;

    // Alignment of buffers inside the storage
    BOOST_STATIC_CONSTEXPR buffer_size_t buffer_alignment = 1;

    // The size used internally to mark a buffer as invalid. Any value is usable
    // (like 0) as long as this is size is never used as valid buffer size.
    BOOST_STATIC_CONSTEXPR buffer_size_t invalid_buffer_size =
        std::numeric_limits<buffer_size_t>::max();
};

// basic_ringbuffer encapsulate the logic about lockfree buffer
// manipulations.
template <typename Self, typename Policies>
struct basic_ringbuffer
{
    // In order to be backward compatible when new policies are added
    BOOST_STATIC_ASSERT((
        std::is_base_of<default_ringbuffer_policies, Policies>::value));

public:
    typedef typename Policies::byte_t byte_t;
    typedef typename Policies::ring_size_t ring_size_t;
    typedef typename Policies::buffer_size_t buffer_size_t;
    typedef typename Policies::buffer buffer_t;

protected:
    atomic<ring_size_t> _write_end;
    /* force read_end and write_end to be in different cache lines */
    BOOST_ALIGNMENT(BOOST_LOCKFREE_CACHELINE_BYTES) atomic<ring_size_t> _read_end;

public:
    basic_ringbuffer()
      : _write_end(0)
      , _read_end(0)
    {}

public:
    // Reserve a buffer in the ring and returns it. Returns nullptr if the
    // ringbuffer is full.
    buffer_t* start_write(buffer_size_t const size) BOOST_NOEXCEPT
    {
        BOOST_ASSERT(size != Policies::invalid_buffer_size);
        BOOST_ASSERT_MSG(_next_index(0, size) <= _internal_capacity(),
                         "Buffer too big");
        ring_size_t head = _write_end.load(memory_order_relaxed);
        ring_size_t tail = _read_end.load(memory_order_acquire);
        ring_size_t next_head = _next_index(head, size);
        if (BOOST_UNLIKELY(next_head > _internal_capacity())) // buffer too big
        {
            if (head < tail) // we cannot skip the tail
                return NULL;

            // We cannot store the buffer, we need to loop back to the
            // beginning. We create a invalid-sized buffer to indicate
            // that.
            next_head = _next_index(0, size);
            if (next_head >= tail) // queue is full
                return NULL;

            // invalid-sized buffer at the old place
            _buffer_at(head)->size = Policies::invalid_buffer_size;
            head = 0;

            // We need to store the new head so that commit_write does not
            // need to check anything.
            _write_end.store(0, memory_order_relaxed);
        }
        else if (BOOST_UNLIKELY(head < tail && next_head >= tail))
        {
            // We have too little available space (the new write end would
            // skip the read end)
            return NULL;
        }
        return _buffer_at(head);
    }

    // Commit buffer into the ringbuffer.
    // precondition: buffer was previously returned by start_write()
    // precondition: buffer->size <= previously asked size
    void commit_write(buffer_t const* buffer) BOOST_NOEXCEPT
    {
        _write_end.fetch_add(
            _aligned_index(buf->size + _header_size()),
            memory_order_release);
    }

    // Returns the next buffer or nullptr if the ringbuffer is empty.
    buffer_t const* start_read() BOOST_NOEXCEPT
    {
        ring_size_t const tail = _read_end.load(memory_order_relaxed);
        ring_size_t const head = _write_end.load(memory_order_acquire);
        if (BOOST_UNLIKELY(tail == head)) return nullptr;
        Buffer const* buf = _buffer_at(tail);
        if (BOOST_LIKELY(buf->size != Policies::invalid_buffer_size))
            return buf;

        BOOST_ASSERT_MSG(head < tail,  "corrupted ring buffer");

        // This happens when the end of the buffer was too short to
        // hold this buffer.
        if (head == 0) return NULL;

        // We need to set it correctly so that commit_read does not need
        // to check anything. We don't need a strong memory ordering
        // because we will update this value in the commit_read(), and
        // the old value has the same meaning for the writing thread.
        _read_end.store(0, memory_order_relaxed);
        return _buffer_at(0);
    }

    // Commit the last read buffer.
    void commit_read(buffer_t const* buffer) BOOST_NOEXCEPT
    {
        _read_end.fetch_add(_aligned_index(buffer->size + _header_size()),
                            memory_order_release);
    }

private:
    Self& _self() { return static_cast<Self&>(*this); }
    Self const& _self() const { return static_cast<Self const&>(*this); }
    byte_t* _ring() { return _self().ring(); }
    byte_t const* _ring() const { return _self().ring(); }
    ring_size_t _internal_capacity() const
    { return _self().capacity() - _header_size(); }
    buffer_t* _buffer_at(ring_size_t index)
    {
        BOOST_ASSERT_MSG(index == _aligned_index(index), "invalid index");
        return reinterpret_cast<buffer_size_t*>(_ring() + head);
    }

protected:
    static ring_size_t _aligned_index(ring_size_t index)
    {
        ring_size_t remainder = index % Policies::buffer_alignment;
        if (remainder == 0)
            return index;
        return index + (Policies::buffer_alignment - remainder);
    }

    static ring_size_t _next_index(ring_size_t current, buffer_size_t size)
    {
        return _aligned_index(
            current + static_cast<ring_size_t>(size) + _header_size());
    }

    static ring_size_t _header_size()
    { return Policies::empty_buffer_size; }
};

namespace storage {

// malloc/free storage
template <typename Policies> struct heap
{
public:
    typedef typename Policies::ring_size_t ring_size_t;
    typedef typename Policies::byte_t byte_t;

private:
    ring_size_t const _capacity;
    byte_t* const _ring;

public:
    explicit heap_storage(ring_size_t const size)
      : _capacity(size)
      , _ring((byte_t*)std::malloc(size))
    {
        if (_ring == nullptr) boost::throw_exception(std::bad_alloc());
    }

    BOOST_DELETED_FUNCTION(heap(heap const&));
    BOOST_DELETED_FUNCTION(heap& operator=(heap const&));

    ~heap() { std::free(_ring); }

public:
    ring_size_t capacity() const { return _capacity; }
    byte_t const* ring() const { return _ring; }
    byte_t* ring() { return _ring; }
};

// stack storage
template<size_t Size, typename Policies> struct stack_storage
{
public:
    typedef typename Policies::ring_size_t ring_size_t;
    typedef typename Policies::byte_t byte_t;

private:
    byte_t _ring[Size];

public:
    stack_storage() : _ring() {}
    BOOST_DELETED_FUNCTION(stack_storage(stack_storage const&));
    BOOST_DELETED_FUNCTION(stack_storage& operator=(stack_storage const&));

public:
    ring_size_t capacity() const { return Size; }
    byte_t const* ring() const { return &_ring[0]; }
    byte_t* ring() { return &_ring[0]; }
};

} /* namespace storage */

template <typename Storage, typename Policies = default_ringbuffer_policies>
struct ringbuffer
    : public Storage
    , public basic_ringbuffer<ringbuffer<Storage, Policies>, Policies>
{
public:
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
    ringbuffer() : Storage() {}
    template <typename A0> explicit ringbuffer(BOOST_FWD_REF(A0) a0)
        : Storage(boost::forward<T1>(a0))
    {}
    template <typename A0, typename A1>
    explicit ringbuffer(BOOST_FWD_REF(A0) a0, BOOST_FWD_REF(A1) a1)
        : Storage(boost::forward<A0>(a0), boost::forward<A1>(a1))
    {}
#else
    template <typename... Args> explicit ringbuffer(Args&&... args)
        : Storage(std::forward<Args>(args)...)
    {}
#endif
};

template <std::size_t Size,  typename Policies = default_ringbuffer_policies>
struct fixed_ringbuffer
    : public storage::stack_storage<Size, Policies>
    , public basic_ringbuffer<fixed_ringbuffer<Size, Policies>>
{};

} /* namespace lockfree */
} /* namespace boost */
