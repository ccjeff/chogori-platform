/*
MIT License

Copyright(c) 2022 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#pragma once
#include <k2/logging/Log.h>
#include <skvhttp/common/Binary.h>
#include <skvhttp/common/Common.h>
#include <skvhttp/common/Serialization.h>

#include <skvhttp/mpack/mpack.h>

namespace skv::http {
namespace log {
    inline thread_local k2::logging::Logger mpack("skv::http::MPack");
}

class MPackNodeReader {
public:
    MPackNodeReader(mpack_node_t node, Binary& source): _node(node), _source(source) {}

    // read something that isn't optional
    template <typename T>
    bool read(T& obj) {
        if (mpack_node_is_nil(_node)) {
            K2LOG_V(log::mpack, "unable to read type {} since node is nil", k2::type_name<T>());
            return false;
        }

        if (!_readFromNode(obj)) {
            K2LOG_V(log::mpack, "unable to read type {} from node type {} with error {}", k2::type_name<T>(), mpack_node_type(_node), mpack_node_error(_node));
            return false;
        }
        return true;
    }

    // read a value that can be optional (can be nil in the mpack stream)
    template <typename T>
    bool read(std::optional<T>& obj) {
        obj.reset();
        if (mpack_node_is_nil(_node)) {
            return true; // it's fine if the stored value is Nil - that'd be an empty optional
        }
        // value is not Nil so we'd better be able to read it as T
        T val;
        if (!_readFromNode(val)) {
            K2LOG_V(log::mpack, "unable to read type {} from node type {} with error {}", k2::type_name<T>(), mpack_node_type(_node), mpack_node_error(_node));
            return false;
        }
        obj = std::make_optional<T>(std::move(val));
        return true;
    }

    // convenience variadic read used to read more than one value at a time
    template <typename First_T, typename Second_T, typename... Rest_T>
    bool read(First_T& obj1, Second_T& obj2, Rest_T&... rest) {
        return read(obj1) && read(obj2) && read(rest...);
    }

public:
    using Binary = skv::http::Binary;

    class MPackStructReader {
    public:
        using Binary = skv::http::Binary;
        MPackStructReader(mpack_node_t& arrayNode, Binary& source) : _arrayNode(arrayNode), _idx(0), _source(source) {}

        template<typename T>
        bool read(T& obj) {
            mpack_node_t vnode = mpack_node_array_at(_arrayNode, _idx++);
            MPackNodeReader reader(vnode, _source);
            return reader.read(obj);
        }

        // convenience variadic read used to read more than one value at a time
        template <typename First_T, typename Second_T, typename... Rest_T>
        bool read(First_T& obj1, Second_T& obj2, Rest_T&... rest) {
            return read(obj1) && read(obj2) && read(rest...);
        }

        bool read() {return true;}

    private:
        mpack_node_t& _arrayNode;
        size_t _idx;
        Binary& _source;
    };

private:
    template <typename T>
    std::enable_if_t<isK2SerializableR<T, MPackNodeReader>::value, bool>
    _readFromNode(T& val) {
        // PayloadSerializable types are packed as a list of values
        // get a reader for the node array and pass it to the struct itself for (recursive) unpacking
        K2LOG_V(log::mpack, "reading serializable of type {}", k2::type_name<T>());
        MPackStructReader sreader(_node, _source);
        return val.k2UnpackFrom(sreader);
    }

    bool _readFromNode(bool& val) {
        K2LOG_V(log::mpack, "reading bool");
        val = mpack_node_bool(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(uint8_t& val) {
        K2LOG_V(log::mpack, "reading uint8_t");
        val = mpack_node_u8(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(int8_t& val) {
        K2LOG_V(log::mpack, "reading int8_t");
        val = mpack_node_i8(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(uint16_t& val) {
        K2LOG_V(log::mpack, "reading uint16_t");
        val = mpack_node_u16(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(int16_t& val) {
        K2LOG_V(log::mpack, "reading int16_t");
        val = mpack_node_i16(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(uint32_t& val) {
        K2LOG_V(log::mpack, "reading uint32_t");
        val = mpack_node_u32(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(int32_t& val) {
        K2LOG_V(log::mpack, "reading int32_t");
        val = mpack_node_i32(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(uint64_t& val) {
        K2LOG_V(log::mpack, "reading uint64_t");
        val = mpack_node_u64(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(int64_t& val) {
        K2LOG_V(log::mpack, "reading int64_t");
        val = mpack_node_i64(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(float& val) {
        K2LOG_V(log::mpack, "reading float");
        val = mpack_node_float_strict(_node);
        return mpack_node_error(_node) == mpack_ok;
    }
    bool _readFromNode(double& val) {
        K2LOG_V(log::mpack, "reading double");
        val = mpack_node_double_strict(_node);
        return mpack_node_error(_node) == mpack_ok;
    }

    // this reads data by sharing. The returned pointer is valid if the backing node is valid
    bool _readData(const char*& data, size_t& sz) {
        sz = mpack_node_bin_size(_node);
        if (mpack_node_error(_node) != mpack_ok) {
            return false;
        }
        data = mpack_node_bin_data(_node);
        if (mpack_node_error(_node) != mpack_ok) {
            return false;
        }

        return true;
    }
    bool _readFromNode(String& val) {
        // String is packed as a BINARY msgpack type
        K2LOG_V(log::mpack, "reading string");
        size_t sz;
        const char* data;
        if (!_readData(data, sz)) {
            return false;
        }

        // we can't share ownership with string so have to copy here
        val = String(data, sz);
        return true;
    }

    // read a Binary value by sharing data.
    // The returned binary shares data and holds a refcount for the entire stream which means that
    // if you hold the Binary, you are holding the entire memory backing the stream
    // In cases where you want to avoid this loss of memory, you should copy the binary (Binary::copy)
    bool _readFromNode(Binary& val) {
        // binary is packed as a BINARY msgpack type
        K2LOG_V(log::mpack, "reading binary");
        size_t sz;
        const char* data;
        if (!_readData(data, sz)) {
            return false;
        }

        // share ownership so we can avoid copy here
        val = Binary(const_cast<char*>(data), sz, _source);
        return true;
    }

    template <typename T>
    std::enable_if_t<isVectorLikeType<T>::value, bool>
    _readFromNode(T& val) {
        K2LOG_V(log::mpack, "reading vector-like of type {}", k2::type_name<T>());
        size_t sz = mpack_node_array_length(_node);
        MPackStructReader sreader(_node, _source);

        for (size_t i = 0; i < sz; i++) {
            auto it = val.end();
            typename T::value_type v;
            if (!sreader.read(v)) {
                val.clear();
                return false;
            }
            val.insert(it, std::move(v));
        }
        return true;
    }

    template <typename ...T>
    bool _readFromNode(std::tuple<T...>& val) {
        K2LOG_V(log::mpack, "reading tuple of type {}", k2::type_name<typename std::remove_reference<decltype(val)>::type>());
        return std::apply([this, reader = MPackStructReader(_node, _source)](auto&&... args) mutable {
            return ((reader.read(args)) && ...);
        },
                          val);
    }

    template <typename T>
    std::enable_if_t<isMapLikeType<T>::value, bool>
    _readFromNode(T& val) {
        K2LOG_V(log::mpack, "reading map-like of type {}", k2::type_name<T>());
        size_t sz = mpack_node_array_length(_node);
        for (size_t i = 0; i < sz; i++) {
            auto it = val.end();
            typename T::key_type k;
            typename T::mapped_type v;
            mpack_node_t element = mpack_node_array_at(_node, i);
            MPackStructReader sreader(element, _source);
            if (!sreader.read(k)) {
                val.clear();
                return false;
            }
            if (!sreader.read(v)) {
                val.clear();
                return false;
            }
            val.insert(it, {std::move(k), std::move(v)});
        }
        return true;
    }

    template <typename T>
    std::enable_if_t<isCompleteType<Serializer<T>>::value, bool>
    _readFromNode(T& value) {
        K2LOG_V(log::mpack, "reading externally serialized object of type {}", k2::type_name<T>());
        Serializer<T> serializer;
        return serializer.k2UnpackFrom(*this, value);
    }

    bool _readFromNode(Duration& dur) {
        K2LOG_V(log::mpack, "reading decimal64");
        if (typeid(std::remove_reference_t<decltype(dur)>::rep) != typeid(long int)) {
            return false;
        }
        long int ticks = 0;
        if (!read(ticks)) return false;
        dur = Duration(ticks);
        return true;
    }

    bool _readFromNode(Decimal64& value) {
        // decimal is packed as a BINARY msgpack type
        K2LOG_V(log::mpack, "reading decimal64");
        size_t sz;
        const char* data;

        if (!_readData(data, sz)) {
            return false;
        }
        if (sizeof(Decimal64::__decfloat64) != sz) {
            return false;
        }

        value.__setval(*((Decimal64::__decfloat64*)data));
        return true;
    }

    bool _readFromNode(Decimal128& value) {
        // decimal is packed as a BINARY msgpack type
        K2LOG_V(log::mpack, "reading decimal128");
        size_t sz;
        const char* data;

        if (!_readData(data, sz)) {
            return false;
        }
        if (sizeof(Decimal128::__decfloat128) != sz) {
            return false;
        }

        value.__setval(*((Decimal128::__decfloat128*)data));
        return true;
    }

    template <typename T>
    std::enable_if_t<std::is_enum<T>::value, bool>
    _readFromNode(T& value) {
        K2LOG_V(log::mpack, "reading enum of type {}", k2::type_name<T>());
        using UT = typename std::remove_reference<decltype(k2::to_integral(value))>::type;
        UT data;
        if (!_readFromNode(data)) {
            return false;
        }

        value = static_cast<T>(data);
        return true;
    }

private:
    mpack_node_t _node;
    Binary& _source;
};

class MPackReader {
public:
    MPackReader(){}
    MPackReader(const Binary& bin): _binary(bin){
        mpack_tree_init_data(&_tree, _binary.data(), _binary.size());  // initialize a parser + parse a tree
    }
    template<typename T>
    bool read(T& obj) {
        // read an entire node tree as a single object.
        mpack_tree_parse(&_tree);
        auto node = mpack_tree_root(&_tree);
        if (mpack_tree_error(&_tree) != mpack_ok) {
            K2LOG_V(log::mpack, "unable to read type {} with error {}", k2::type_name<T>(), mpack_tree_error(&_tree));
            return false;
        }
        MPackNodeReader reader(node, _binary);
        return reader.read(obj);
    }

private:
    Binary _binary;
    mpack_tree_t _tree{};
};

class MPackNodeWriter {
public:
    using Binary = skv::http::Binary;

    MPackNodeWriter(mpack_writer_t& writer): _writer(writer){
    }

    // Write the given binary as an object. The bytes are copied to the underlying stream
    void write(const Binary& val) {
        K2LOG_V(log::mpack, "writing binary {}", val);
        K2ASSERT(log::mpack, val.size() < std::numeric_limits<uint32_t>::max(), "cannot write binary of size {}", val.size());
        mpack_write_bin(&_writer, val.data(), (uint32_t)val.size());
    }

    template <typename T>
    void write(const std::optional<T>& obj) {
        K2LOG_V(log::mpack, "writing nil optional of type {}", k2::type_name<T>());
        if (!obj) {
            mpack_write_nil(&_writer);
        } else {
            write(*obj);
        }
    }

    // convenience variadic write used to write more than one value at a time
    template <typename First_T, typename Second_T, typename... Rest_T>
    void write(const First_T& obj1, const Second_T& obj2, const Rest_T&... rest) {
        write(std::forward<const First_T>(obj1));
        write(std::forward<const Second_T>(obj2));
        write(std::forward<const Rest_T>(rest)...);
    }

    template <typename T>
    std::enable_if_t<isK2SerializableW<T, MPackNodeWriter>::value, void>
    write(const T& val) {
        K2LOG_V(log::mpack, "writing serializable type {}", val);
        mpack_start_array(&_writer, val.k2GetNumberOfPackedFields());
        val.k2PackTo(*this);
        mpack_finish_array(&_writer);
    }

    template <typename T>
    std::enable_if_t<isVectorLikeType<T>::value, void>
    write(const T& val) {
        K2LOG_V(log::mpack, "writing vector-like of type {} and size {}", k2::type_name<T>(), val.size());
        mpack_start_array(&_writer, val.size());
        for (const auto& el: val) {
            write(el);
        }
        mpack_finish_array(&_writer);
    }

    template <typename ...T>
    void write(const std::tuple<T...>& val) {
        K2LOG_V(log::mpack, "writing tuple of type {} and size {}", k2::type_name<std::tuple<T...>>(), std::tuple_size_v<std::tuple<T...>>);
        mpack_start_array(&_writer, (uint32_t)std::tuple_size_v<std::tuple<T...>>);
        std::apply([this](const auto&... args) { write(args...); }, val);
        mpack_finish_array(&_writer);
    }

    template <typename T>
    std::enable_if_t<isMapLikeType<T>::value, void>
    write(const T& val) {
        K2LOG_V(log::mpack, "writing map-like of type {} and size {}", k2::type_name<T>(), val.size());
        mpack_start_array(&_writer, val.size());
        for (const auto& [k,v] : val) {
            mpack_start_array(&_writer, 2);
            write(k);
            write(v);
            mpack_finish_array(&_writer);
        }
        mpack_finish_array(&_writer);
    }

    void write(int8_t value) {
        K2LOG_V(log::mpack, "writing int8 type {}", value);
        mpack_write_i8(&_writer, value);
    }
    void write(uint8_t value) {
        K2LOG_V(log::mpack, "writing uint8 type {}", value);
        mpack_write_u8(&_writer, value);
    }
    void write(int16_t value) {
        K2LOG_V(log::mpack, "writing int16 type {}", value);
        mpack_write_i16(&_writer, value);
    }
    void write(uint16_t value) {
        K2LOG_V(log::mpack, "writing uint16 type {}", value);
        mpack_write_u16(&_writer, value);
    }
    void write(int32_t value) {
        K2LOG_V(log::mpack, "writing int32 type {}", value);
        mpack_write_i32(&_writer, value);
    }
    void write(uint32_t value) {
        K2LOG_V(log::mpack, "writing uint32 type {}", value);
        mpack_write_u32(&_writer, value);
    }
    void write(int64_t value) {
        K2LOG_V(log::mpack, "writing int64 type {}", value);
        mpack_write_i64(&_writer, value);
    }
    void write(uint64_t value) {
        K2LOG_V(log::mpack, "writing uint64 type {}", value);
        mpack_write_u64(&_writer, value);
    }
    void write(bool value) {
        K2LOG_V(log::mpack, "writing bool type {}", value);
        mpack_write_bool(&_writer, value);
    }
    void write(float value) {
        K2LOG_V(log::mpack, "writing float type {}", value);
        mpack_write_float(&_writer, value);
    }
    void write(double value) {
        K2LOG_V(log::mpack, "writing double type {}", value);
        mpack_write_double(&_writer, value);
    }
    void write(const Duration& dur) {
        K2LOG_V(log::mpack, "writing duration type {}", dur);
        write(dur.count());  // write the tick count
    }
    void write(const Decimal64& value) {
        K2LOG_V(log::mpack, "writing decimal64 type {}", value);
        Decimal64::__decfloat64 data = const_cast<Decimal64&>(value).__getval();
        mpack_write_bin(&_writer, (const char*)&data, sizeof(Decimal64::__decfloat64));
    }
    void write(const Decimal128& value) {
        K2LOG_V(log::mpack, "writing decimal128 type {}", value);
        Decimal128::__decfloat128 data = const_cast<Decimal128&>(value).__getval();
        mpack_write_bin(&_writer, (const char*)&data, sizeof(Decimal128::__decfloat128));
    }
    void write(const String& val) {
        K2LOG_V(log::mpack, "writing string type {}", val);
        K2ASSERT(log::mpack, val.size() < std::numeric_limits<uint32_t>::max(), "cannot write binary of size {}", val.size());
        mpack_write_bin(&_writer, val.data(), (uint32_t)val.size());
    }

    template <typename T>
    std::enable_if_t<std::is_enum<T>::value, void>
    write(const T value) {
        K2LOG_V(log::mpack, "writing enum type {}", value);
        write(k2::to_integral(value));
    }

    template <typename T>
    std::enable_if_t<isCompleteType<Serializer<T>>::value, void>
    write(const T& value) {
        K2LOG_V(log::mpack, "writing externally serialized object of type {}", k2::type_name<T>());
        Serializer<T> serializer;
        serializer.k2PackTo(*this, value);
    }

    void write(){
        K2LOG_V(log::mpack, "writing base case");
    }

private:
    mpack_writer_t &_writer;
};


class MPackWriter {
public:
    MPackWriter() {
        mpack_writer_init_growable(&_writer, &_data, &_size);
    }
    template <typename T>
    void write(T&& obj) {
        MPackNodeWriter writer(_writer);
        writer.write(std::forward<T>(obj));
    }

    void write(){
    }

    bool flush(Binary& binary) {
        if (mpack_writer_destroy(&_writer) != mpack_ok) {
            return false;
        }
        binary = Binary(_data, _size, [ptr=_data]() { MPACK_FREE(ptr);});
        _data = 0;
        _size = 0;
        return true;
    }
private:
    char* _data;
    size_t _size;
    mpack_writer_t _writer;
};
}
