#ifndef ENTT_META_META_HPP
#define ENTT_META_META_HPP


#include <algorithm>
#include <cstddef>
#include <iterator>
#include <functional>
#include <type_traits>
#include <utility>
#include "../config/config.h"
#include "../core/fwd.hpp"
#include "ctx.hpp"
#include "internal.hpp"
#include "range.hpp"


namespace entt {


class meta_type;


/**
 * @brief Opaque container for values of any type.
 *
 * This class uses a technique called small buffer optimization (SBO) to get rid
 * of memory allocations if possible. This should improve overall performance.
 */
class meta_any {
    using storage_type = std::aligned_storage_t<sizeof(void *), alignof(void *)>;
    using copy_fn_type = void(meta_any &, const meta_any &);
    using steal_fn_type = void(meta_any &, meta_any &);
    using destroy_fn_type = void(meta_any &);

    template<typename Type, typename = std::void_t<>>
    struct type_traits {
        template<typename... Args>
        static void instance(meta_any &any, Args &&... args) {
            any.instance = new Type{std::forward<Args>(args)...};
            new (&any.storage) Type *{static_cast<Type *>(any.instance)};
        }

        static void destroy(meta_any &any) {
            const auto * const node = internal::meta_info<Type>::resolve();
            if(node->dtor) { node->dtor->invoke(any.instance); }
            delete static_cast<Type *>(any.instance);
        }

        static void copy(meta_any &to, const meta_any &from) {
            auto *instance = new Type{*static_cast<const Type *>(from.instance)};
            new (&to.storage) Type *{instance};
            to.instance = instance;
        }

        static void steal(meta_any &to, meta_any &from) {
            new (&to.storage) Type *{static_cast<Type *>(from.instance)};
            to.instance = from.instance;
        }
    };

    template<typename Type>
    struct type_traits<Type, std::enable_if_t<sizeof(Type) <= sizeof(void *) && std::is_nothrow_move_constructible_v<Type>>> {
        template<typename... Args>
        static void instance(meta_any &any, Args &&... args) {
            any.instance = new (&any.storage) Type{std::forward<Args>(args)...};
        }

        static void destroy(meta_any &any) {
            const auto * const node = internal::meta_info<Type>::resolve();
            if(node->dtor) { node->dtor->invoke(any.instance); }
            static_cast<Type *>(any.instance)->~Type();
        }

        static void copy(meta_any &to, const meta_any &from) {
            to.instance = new (&to.storage) Type{*static_cast<const Type *>(from.instance)};
        }

        static void steal(meta_any &to, meta_any &from) {
            to.instance = new (&to.storage) Type{std::move(*static_cast<Type *>(from.instance))};
            destroy(from);
        }
    };

    meta_any(const internal::meta_type_node *curr, void *ref) ENTT_NOEXCEPT
        : meta_any{}
    {
        node = curr;
        instance = ref;
    }

public:
    /*! @brief Default constructor. */
    meta_any() ENTT_NOEXCEPT
        : storage{},
          instance{},
          node{},
          destroy_fn{},
          copy_fn{},
          steal_fn{}
    {}

    /**
     * @brief Constructs a meta any by directly initializing the new object.
     * @tparam Type Type of object to use to initialize the container.
     * @tparam Args Types of arguments to use to construct the new instance.
     * @param args Parameters to use to construct the instance.
     */
    template<typename Type, typename... Args>
    explicit meta_any(std::in_place_type_t<Type>, [[maybe_unused]] Args &&... args)
        : meta_any{}
    {
        node = internal::meta_info<Type>::resolve();

        if constexpr(!std::is_void_v<Type>) {
            using traits_type = type_traits<std::remove_cv_t<std::remove_reference_t<Type>>>;
            traits_type::instance(*this, std::forward<Args>(args)...);
            destroy_fn = &traits_type::destroy;
            copy_fn = &traits_type::copy;
            steal_fn = &traits_type::steal;
        }
    }

    /**
     * @brief Constructs a meta any that holds an unmanaged object.
     * @tparam Type Type of object to use to initialize the container.
     * @param value An instance of an object to use to initialize the container.
     */
    template<typename Type>
    meta_any(std::reference_wrapper<Type> value)
        : meta_any{internal::meta_info<Type>::resolve(), &value.get()}
    {}

    /**
     * @brief Constructs a meta any from a given value.
     * @tparam Type Type of object to use to initialize the container.
     * @param value An instance of an object to use to initialize the container.
     */
    template<typename Type, typename = std::enable_if_t<!std::is_same_v<std::remove_cv_t<std::remove_reference_t<Type>>, meta_any>>>
    meta_any(Type &&value)
        : meta_any{std::in_place_type<std::remove_cv_t<std::remove_reference_t<Type>>>, std::forward<Type>(value)}
    {}

    /**
     * @brief Copy constructor.
     * @param other The instance to copy from.
     */
    meta_any(const meta_any &other)
        : meta_any{}
    {
        node = other.node;
        (other.copy_fn ? other.copy_fn : [](meta_any &to, const meta_any &from) { to.instance = from.instance; })(*this, other);
        destroy_fn = other.destroy_fn;
        copy_fn = other.copy_fn;
        steal_fn = other.steal_fn;
    }

    /**
     * @brief Move constructor.
     *
     * After move construction, instances that have been moved from are placed
     * in a valid but unspecified state.
     *
     * @param other The instance to move from.
     */
    meta_any(meta_any &&other)
        : meta_any{}
    {
        swap(*this, other);
    }

    /*! @brief Frees the internal storage, whatever it means. */
    ~meta_any() {
        if(destroy_fn) {
            destroy_fn(*this);
        }
    }

    /**
     * @brief Assignment operator.
     * @tparam Type Type of object to use to initialize the container.
     * @param value An instance of an object to use to initialize the container.
     * @return This meta any object.
     */
    template<typename Type>
    meta_any & operator=(Type &&value) {
        return (*this = meta_any{std::forward<Type>(value)});
    }

    /**
     * @brief Assignment operator.
     * @param other The instance to assign from.
     * @return This meta any object.
     */
    meta_any & operator=(meta_any other) {
        swap(other, *this);
        return *this;
    }

    /**
     * @brief Returns the meta type of the underlying object.
     * @return The meta type of the underlying object, if any.
     */
    [[nodiscard]] inline meta_type type() const ENTT_NOEXCEPT;

    /**
     * @brief Returns an opaque pointer to the contained instance.
     * @return An opaque pointer the contained instance, if any.
     */
    [[nodiscard]] const void * data() const ENTT_NOEXCEPT {
        return instance;
    }

    /*! @copydoc data */
    [[nodiscard]] void * data() ENTT_NOEXCEPT {
        return const_cast<void *>(std::as_const(*this).data());
    }

    /**
     * @brief Tries to cast an instance to a given type.
     * @tparam Type Type to which to cast the instance.
     * @return A (possibly null) pointer to the contained instance.
     */
    template<typename Type>
    [[nodiscard]] const Type * try_cast() const {
        void *ret = nullptr;

        if(node) {
            if(const auto type_id = internal::meta_info<Type>::resolve()->type_id; node->type_id == type_id) {
                ret = instance;
            } else if(const auto *base = internal::find_if<&internal::meta_type_node::base>([type_id](const auto *curr) { return curr->type()->type_id == type_id; }, node); base) {
                ret = base->cast(instance);
            }
        }

        return static_cast<const Type *>(ret);
    }

    /*! @copydoc try_cast */
    template<typename Type>
    [[nodiscard]] Type * try_cast() {
        return const_cast<Type *>(std::as_const(*this).try_cast<Type>());
    }

    /**
     * @brief Tries to cast an instance to a given type.
     *
     * The type of the instance must be such that the cast is possible.
     *
     * @warning
     * Attempting to perform a cast that isn't viable results in undefined
     * behavior.<br/>
     * An assertion will abort the execution at runtime in debug mode in case
     * the cast is not feasible.
     *
     * @tparam Type Type to which to cast the instance.
     * @return A reference to the contained instance.
     */
    template<typename Type>
    [[nodiscard]] const Type & cast() const {
        auto * const actual = try_cast<Type>();
        ENTT_ASSERT(actual);
        return *actual;
    }

    /*! @copydoc cast */
    template<typename Type>
    [[nodiscard]] Type & cast() {
        return const_cast<Type &>(std::as_const(*this).cast<Type>());
    }

    /**
     * @brief Tries to convert an instance to a given type and returns it.
     * @tparam Type Type to which to convert the instance.
     * @return A valid meta any object if the conversion is possible, an invalid
     * one otherwise.
     */
    template<typename Type>
    [[nodiscard]] meta_any convert() const {
        meta_any any{};

        if(node) {
            if(const auto type_id = internal::meta_info<Type>::resolve()->type_id; node->type_id == type_id) {
                any = *this;
            } else if(const auto * const conv = internal::find_if<&internal::meta_type_node::conv>([type_id](const auto *curr) { return curr->type()->type_id == type_id; }, node); conv) {
                any = conv->conv(instance);
            }
        }

        return any;
    }

    /**
     * @brief Tries to convert an instance to a given type.
     * @tparam Type Type to which to convert the instance.
     * @return True if the conversion is possible, false otherwise.
     */
    template<typename Type>
    bool convert() {
        bool valid = (node && node->type_id == internal::meta_info<Type>::resolve()->type_id);

        if(!valid) {
            if(auto any = std::as_const(*this).convert<Type>(); any) {
                swap(any, *this);
                valid = true;
            }
        }

        return valid;
    }

    /**
     * @brief Replaces the contained object by initializing a new instance
     * directly.
     * @tparam Type Type of object to use to initialize the container.
     * @tparam Args Types of arguments to use to construct the new instance.
     * @param args Parameters to use to construct the instance.
     */
    template<typename Type, typename... Args>
    void emplace(Args &&... args) {
        *this = meta_any{std::in_place_type_t<Type>{}, std::forward<Args>(args)...};
    }

    /**
     * @brief Aliasing constructor.
     * @return A meta any that shares a reference to an unmanaged object.
     */
    [[nodiscard]] meta_any ref() const ENTT_NOEXCEPT {
        return meta_any{node, instance};
    }

    /**
     * @brief Indirection operator for aliasing construction.
     * @return A meta any that shares a reference to an unmanaged object.
     */
    [[nodiscard]] meta_any operator *() const ENTT_NOEXCEPT {
        return ref();
    }

    /**
     * @brief Returns false if a container is empty, true otherwise.
     * @return False if the container is empty, true otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

    /**
     * @brief Checks if two containers differ in their content.
     * @param other Container with which to compare.
     * @return False if the two containers differ in their content, true
     * otherwise.
     */
    [[nodiscard]] bool operator==(const meta_any &other) const {
        return (!node && !other.node) || (node && other.node && node->type_id == other.node->type_id && node->compare(instance, other.instance));
    }

    /**
     * @brief Swaps two meta any objects.
     * @param lhs A valid meta any object.
     * @param rhs A valid meta any object.
     */
    friend void swap(meta_any &lhs, meta_any &rhs) {
        if(lhs.steal_fn && rhs.steal_fn) {
            meta_any buffer{};
            lhs.steal_fn(buffer, lhs);
            rhs.steal_fn(lhs, rhs);
            lhs.steal_fn(rhs, buffer);
        } else if(lhs.steal_fn) {
            lhs.steal_fn(rhs, lhs);
        } else if(rhs.steal_fn) {
            rhs.steal_fn(lhs, rhs);
        } else {
            std::swap(lhs.instance, rhs.instance);
        }

        std::swap(lhs.node, rhs.node);
        std::swap(lhs.destroy_fn, rhs.destroy_fn);
        std::swap(lhs.copy_fn, rhs.copy_fn);
        std::swap(lhs.steal_fn, rhs.steal_fn);
    }

private:
    storage_type storage;
    void *instance;
    const internal::meta_type_node *node;
    destroy_fn_type *destroy_fn;
    copy_fn_type *copy_fn;
    steal_fn_type *steal_fn;
};


/**
 * @brief Checks if two containers differ in their content.
 * @param lhs A meta any object, either empty or not.
 * @param rhs A meta any object, either empty or not.
 * @return True if the two containers differ in their content, false otherwise.
 */
[[nodiscard]] inline bool operator!=(const meta_any &lhs, const meta_any &rhs) ENTT_NOEXCEPT {
    return !(lhs == rhs);
}


/**
 * @brief Opaque pointers to instances of any type.
 *
 * A handle doesn't perform copies and isn't responsible for the contained
 * object. It doesn't prolong the lifetime of the pointed instance.<br/>
 * Handles are used to generate meta references to actual objects when needed.
 */
struct meta_handle {
    /*! @brief Default constructor. */
    meta_handle()
        : any{}
    {}

    /**
     * @brief Creates a handle that points to an unmanaged object.
     * @tparam Type Type of object to use to initialize the container.
     * @param value An instance of an object to use to initialize the container.
     */
    template<typename Type>
    meta_handle(Type &&value) ENTT_NOEXCEPT
        : meta_handle{}
    {
        if constexpr(std::is_same_v<std::remove_cv_t<std::remove_reference_t<Type>>, meta_any>) {
            any = *value;
        } else {
            static_assert(std::is_lvalue_reference_v<Type>, "Lvalue reference required");
            any = std::ref(value);
        }
    }

    /*! @copydoc meta_any::operator* */
    [[nodiscard]] meta_any operator *() const {
        return *any;
    }

private:
    meta_any any;
};


/*! @brief Opaque container for meta properties of any type. */
struct meta_prop {
    /*! @brief Node type. */
    using node_type = internal::meta_prop_node;

    /**
     * @brief Constructs an instance from a given node.
     * @param curr The underlying node with which to construct the instance.
     */
    meta_prop(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /**
     * @brief Returns the stored key.
     * @return A meta any containing the key stored with the property.
     */
    [[nodiscard]] meta_any key() const {
        return node->key();
    }

    /**
     * @brief Returns the stored value.
     * @return A meta any containing the value stored with the property.
     */
    [[nodiscard]] meta_any value() const {
        return node->value();
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

private:
    const node_type *node;
};


/*! @brief Opaque container for meta base classes. */
struct meta_base {
    /*! @brief Node type. */
    using node_type = internal::meta_base_node;

    /*! @copydoc meta_prop::meta_prop */
    meta_base(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /**
     * @brief Returns the meta type to which a meta object belongs.
     * @return The meta type to which the meta object belongs.
     */
    [[nodiscard]] inline meta_type parent() const ENTT_NOEXCEPT;

    /*! @copydoc meta_any::type */
    [[nodiscard]] inline meta_type type() const ENTT_NOEXCEPT;

    /**
     * @brief Casts an instance from a parent type to a base type.
     * @param instance The instance to cast.
     * @return An opaque pointer to the base type.
     */
    [[nodiscard]] void * cast(void *instance) const ENTT_NOEXCEPT {
        return node->cast(instance);
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

private:
    const node_type *node;
};


/*! @brief Opaque container for meta conversion functions. */
struct meta_conv {
    /*! @brief Node type. */
    using node_type = internal::meta_conv_node;

    /*! @copydoc meta_prop::meta_prop */
    meta_conv(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /*! @copydoc meta_base::parent */
    [[nodiscard]] inline meta_type parent() const ENTT_NOEXCEPT;

    /*! @copydoc meta_any::type */
    [[nodiscard]] inline meta_type type() const ENTT_NOEXCEPT;

    /**
     * @brief Converts an instance to the underlying type.
     * @param instance The instance to convert.
     * @return An opaque pointer to the instance to convert.
     */
    [[nodiscard]] meta_any convert(const void *instance) const {
        return node->conv(instance);
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

private:
    const node_type *node;
};


/*! @brief Opaque container for meta constructors. */
struct meta_ctor {
    /*! @brief Node type. */
    using node_type = internal::meta_ctor_node;
    /*! @brief Unsigned integer type. */
    using size_type = typename node_type::size_type;

    /*! @copydoc meta_prop::meta_prop */
    meta_ctor(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /*! @copydoc meta_base::parent */
    [[nodiscard]] inline meta_type parent() const ENTT_NOEXCEPT;

    /**
     * @brief Returns the number of arguments accepted by a meta constructor.
     * @return The number of arguments accepted by the meta constructor.
     */
    [[nodiscard]] size_type size() const ENTT_NOEXCEPT {
        return node->size;
    }

    /**
     * @brief Returns the meta type of the i-th argument of a meta constructor.
     * @param index The index of the argument of which to return the meta type.
     * @return The meta type of the i-th argument of a meta constructor, if any.
     */
    [[nodiscard]] meta_type arg(size_type index) const ENTT_NOEXCEPT;

    /**
     * @brief Creates an instance of the underlying type, if possible.
     *
     * To create a valid instance, the parameters must be such that a cast or
     * conversion to the required types is possible. Otherwise, an empty and
     * thus invalid container is returned.
     *
     * @tparam Args Types of arguments to use to construct the instance.
     * @param args Parameters to use to construct the instance.
     * @return A meta any containing the new instance, if any.
     */
    template<typename... Args>
    [[nodiscard]] meta_any invoke([[maybe_unused]] Args &&... args) const {
        if constexpr(sizeof...(Args) == 0) {
            return sizeof...(Args) == size() ? node->invoke(nullptr) : meta_any{};
        } else {
            meta_any arguments[]{std::forward<Args>(args)...};
            return sizeof...(Args) == size() ? node->invoke(arguments) : meta_any{};
        }
    }

    /**
     * @brief Returns a range to use to visit all meta properties.
     * @return An iterable range to use to visit all meta properties.
     */
    meta_range<meta_prop> prop() const ENTT_NOEXCEPT {
        return node->prop;
    }

    /**
     * @brief Iterates all meta properties assigned to a meta constructor.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use prop() and entt::meta_range<meta_prop> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_prop>>
    prop(Op op) const {
        for(auto curr: prop()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the property associated with a given key.
     * @param key The key to use to search for a property.
     * @return The property associated with the given key, if any.
     */
    [[nodiscard]] meta_prop prop(meta_any key) const {
        internal::meta_range range{node->prop};
        return std::find_if(range.begin(), range.end(), [&key](const auto &curr) { return curr.key() == key; }).operator->();
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

private:
    const node_type *node;
};


/*! @brief Opaque container for meta data. */
struct meta_data {
    /*! @brief Node type. */
    using node_type = internal::meta_data_node;

    /*! @copydoc meta_prop::meta_prop */
    meta_data(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /*! @copydoc meta_type::id */
    [[nodiscard]] id_type id() const ENTT_NOEXCEPT {
        return node->id;
    }

    /*! @copydoc meta_base::parent */
    [[nodiscard]] inline meta_type parent() const ENTT_NOEXCEPT;

    /**
     * @brief Indicates whether a meta data is constant or not.
     * @return True if the meta data is constant, false otherwise.
     */
    [[nodiscard]] bool is_const() const ENTT_NOEXCEPT {
        return (node->set == nullptr);
    }

    /**
     * @brief Indicates whether a meta data is static or not.
     * @return True if the meta data is static, false otherwise.
     */
    [[nodiscard]] bool is_static() const ENTT_NOEXCEPT {
        return node->is_static;
    }

    /*! @copydoc meta_any::type */
    [[nodiscard]] inline meta_type type() const ENTT_NOEXCEPT;

    /**
     * @brief Sets the value of a given variable.
     *
     * It must be possible to cast the instance to the parent type of the meta
     * data. Otherwise, invoking the setter results in an undefined
     * behavior.<br/>
     * The type of the value must be such that a cast or conversion to the type
     * of the variable is possible. Otherwise, invoking the setter does nothing.
     *
     * @tparam Type Type of value to assign.
     * @param instance An opaque instance of the underlying type.
     * @param value Parameter to use to set the underlying variable.
     * @return True in case of success, false otherwise.
     */
    template<typename Type>
    bool set(meta_handle instance, Type &&value) const {
        return node->set && node->set(*instance, std::forward<Type>(value));
    }

    /**
     * @brief Gets the value of a given variable.
     *
     * It must be possible to cast the instance to the parent type of the meta
     * data. Otherwise, invoking the getter results in an undefined behavior.
     *
     * @param instance An opaque instance of the underlying type.
     * @return A meta any containing the value of the underlying variable.
     */
    [[nodiscard]] meta_any get(meta_handle instance) const {
        return node->get(*instance);
    }

    /*! @copydoc meta_ctor::prop */
    meta_range<meta_prop> prop() const ENTT_NOEXCEPT {
        return node->prop;
    }

    /**
     * @brief Iterates all meta properties assigned to a meta data.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use prop() and entt::meta_range<meta_prop> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_prop>>
    prop(Op op) const {
        for(auto curr: prop()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the property associated with a given key.
     * @param key The key to use to search for a property.
     * @return The property associated with the given key, if any.
     */
    [[nodiscard]] meta_prop prop(meta_any key) const {
        internal::meta_range range{node->prop};
        return std::find_if(range.begin(), range.end(), [&key](const auto &curr) { return curr.key() == key; }).operator->();
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

private:
    const node_type *node;
};


/*! @brief Opaque container for meta functions. */
struct meta_func {
    /*! @brief Node type. */
    using node_type = internal::meta_func_node;
    /*! @brief Unsigned integer type. */
    using size_type = typename node_type::size_type;

    /*! @copydoc meta_prop::meta_prop */
    meta_func(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /*! @copydoc meta_type::id */
    [[nodiscard]] id_type id() const ENTT_NOEXCEPT {
        return node->id;
    }

    /*! @copydoc meta_base::parent */
    [[nodiscard]] inline meta_type parent() const ENTT_NOEXCEPT;

    /**
     * @brief Returns the number of arguments accepted by a meta function.
     * @return The number of arguments accepted by the meta function.
     */
    [[nodiscard]] size_type size() const ENTT_NOEXCEPT {
        return node->size;
    }

    /**
     * @brief Indicates whether a meta function is constant or not.
     * @return True if the meta function is constant, false otherwise.
     */
    [[nodiscard]] bool is_const() const ENTT_NOEXCEPT {
        return node->is_const;
    }

    /**
     * @brief Indicates whether a meta function is static or not.
     * @return True if the meta function is static, false otherwise.
     */
    [[nodiscard]] bool is_static() const ENTT_NOEXCEPT {
        return node->is_static;
    }

    /**
     * @brief Returns the meta type of the return type of a meta function.
     * @return The meta type of the return type of the meta function.
     */
    [[nodiscard]] inline meta_type ret() const ENTT_NOEXCEPT;

    /**
     * @brief Returns the meta type of the i-th argument of a meta function.
     * @param index The index of the argument of which to return the meta type.
     * @return The meta type of the i-th argument of a meta function, if any.
     */
    [[nodiscard]] inline meta_type arg(size_type index) const ENTT_NOEXCEPT;

    /**
     * @brief Invokes the underlying function, if possible.
     *
     * To invoke a meta function, the parameters must be such that a cast or
     * conversion to the required types is possible. Otherwise, an empty and
     * thus invalid container is returned.<br/>
     * It must be possible to cast the instance to the parent type of the meta
     * function. Otherwise, invoking the underlying function results in an
     * undefined behavior.
     *
     * @tparam Args Types of arguments to use to invoke the function.
     * @param instance An opaque instance of the underlying type.
     * @param args Parameters to use to invoke the function.
     * @return A meta any containing the returned value, if any.
     */
    template<typename... Args>
    meta_any invoke(meta_handle instance, Args &&... args) const {
        meta_any arguments[]{*instance, std::forward<Args>(args)...};
        return sizeof...(Args) == size() ? node->invoke(arguments[0], &arguments[sizeof...(Args) != 0]) : meta_any{};
    }

    /*! @copydoc meta_ctor::prop */
    meta_range<meta_prop> prop() const ENTT_NOEXCEPT {
        return node->prop;
    }

    /**
     * @brief Iterates all meta properties assigned to a meta function.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use prop() and entt::meta_range<meta_prop> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_prop>>
    prop(Op op) const {
        for(auto curr: prop()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the property associated with a given key.
     * @param key The key to use to search for a property.
     * @return The property associated with the given key, if any.
     */
    [[nodiscard]] meta_prop prop(meta_any key) const {
        internal::meta_range range{node->prop};
        return std::find_if(range.begin(), range.end(), [&key](const auto &curr) { return curr.key() == key; }).operator->();
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

private:
    const node_type *node;
};


/*! @brief Opaque container for meta types. */
class meta_type {
    template<typename... Args, std::size_t... Indexes>
    [[nodiscard]] auto ctor(std::index_sequence<Indexes...>) const {
        internal::meta_range range{node->ctor};

        return std::find_if(range.begin(), range.end(), [](const auto &candidate) {
            return candidate.size == sizeof...(Args) && ([](auto *from, auto *to) {
                return (from->type_id == to->type_id)
                        || internal::find_if<&node_type::base>([to](const auto *curr) { return curr->type()->type_id == to->type_id; }, from)
                        || internal::find_if<&node_type::conv>([to](const auto *curr) { return curr->type()->type_id == to->type_id; }, from);
            }(internal::meta_info<Args>::resolve(), candidate.arg(Indexes)) && ...);
        }).operator->();
    }

public:
    /*! @brief Node type. */
    using node_type = internal::meta_type_node;
    /*! @brief Unsigned integer type. */
    using size_type = typename node_type::size_type;

    /*! @copydoc meta_prop::meta_prop */
    meta_type(const node_type *curr = nullptr) ENTT_NOEXCEPT
        : node{curr}
    {}

    /**
     * @brief Returns the type id of the underlying type.
     * @return The type id of the underlying type.
     */
    [[nodiscard]] id_type type_id() const ENTT_NOEXCEPT {
        return node->type_id;
    }

    /**
     * @brief Returns the identifier assigned to a meta object.
     * @return The identifier assigned to the meta object.
     */
    [[nodiscard]] id_type id() const ENTT_NOEXCEPT {
        return node->id;
    }

    /**
     * @brief Indicates whether a meta type refers to void or not.
     * @return True if the underlying type is void, false otherwise.
     */
    [[nodiscard]] bool is_void() const ENTT_NOEXCEPT {
        return node->is_void;
    }

    /**
     * @brief Indicates whether a meta type refers to an integral type or not.
     * @return True if the underlying type is an integral type, false otherwise.
     */
    [[nodiscard]] bool is_integral() const ENTT_NOEXCEPT {
        return node->is_integral;
    }

    /**
     * @brief Indicates whether a meta type refers to a floating-point type or
     * not.
     * @return True if the underlying type is a floating-point type, false
     * otherwise.
     */
    [[nodiscard]] bool is_floating_point() const ENTT_NOEXCEPT {
        return node->is_floating_point;
    }

    /**
     * @brief Indicates whether a meta type refers to an array type or not.
     * @return True if the underlying type is an array type, false otherwise.
     */
    [[nodiscard]] bool is_array() const ENTT_NOEXCEPT {
        return node->is_array;
    }

    /**
     * @brief Indicates whether a meta type refers to an enum or not.
     * @return True if the underlying type is an enum, false otherwise.
     */
    [[nodiscard]] bool is_enum() const ENTT_NOEXCEPT {
        return node->is_enum;
    }

    /**
     * @brief Indicates whether a meta type refers to an union or not.
     * @return True if the underlying type is an union, false otherwise.
     */
    [[nodiscard]] bool is_union() const ENTT_NOEXCEPT {
        return node->is_union;
    }

    /**
     * @brief Indicates whether a meta type refers to a class or not.
     * @return True if the underlying type is a class, false otherwise.
     */
    [[nodiscard]] bool is_class() const ENTT_NOEXCEPT {
        return node->is_class;
    }

    /**
     * @brief Indicates whether a meta type refers to a pointer or not.
     * @return True if the underlying type is a pointer, false otherwise.
     */
    [[nodiscard]] bool is_pointer() const ENTT_NOEXCEPT {
        return node->is_pointer;
    }

    /**
     * @brief Indicates whether a meta type refers to a function pointer or not.
     * @return True if the underlying type is a function pointer, false
     * otherwise.
     */
    [[nodiscard]] bool is_function_pointer() const ENTT_NOEXCEPT {
        return node->is_function_pointer;
    }

    /**
     * @brief Indicates whether a meta type refers to a pointer to data member
     * or not.
     * @return True if the underlying type is a pointer to data member, false
     * otherwise.
     */
    [[nodiscard]] bool is_member_object_pointer() const ENTT_NOEXCEPT {
        return node->is_member_object_pointer;
    }

    /**
     * @brief Indicates whether a meta type refers to a pointer to member
     * function or not.
     * @return True if the underlying type is a pointer to member function,
     * false otherwise.
     */
    [[nodiscard]] bool is_member_function_pointer() const ENTT_NOEXCEPT {
        return node->is_member_function_pointer;
    }

    /**
     * @brief If a meta type refers to an array type, provides the number of
     * dimensions of the array.
     * @return The number of dimensions of the array if the underlying type is
     * an array type, 0 otherwise.
     */
    [[nodiscard]] size_type rank() const ENTT_NOEXCEPT {
        return node->rank;
    }

    /**
     * @brief If a meta type refers to an array type, provides the number of
     * elements along the given dimension of the array.
     * @param dim
     * @return The number of elements along the given dimension of the array if
     * the underlying type is an array type, 0 otherwise.
     */
    [[nodiscard]] size_type extent(size_type dim = {}) const ENTT_NOEXCEPT {
        return node->extent(dim);
    }

    /**
     * @brief Provides the meta type for which the pointer is defined.
     * @return The meta type for which the pointer is defined or this meta type
     * if it doesn't refer to a pointer type.
     */
    [[nodiscard]] meta_type remove_pointer() const ENTT_NOEXCEPT {
        return node->remove_pointer();
    }

    /**
     * @brief Provides the meta type for which the array is defined.
     * @return The meta type for which the array is defined or this meta type
     * if it doesn't refer to an array type.
     */
    [[nodiscard]] meta_type remove_extent() const ENTT_NOEXCEPT {
        return node->remove_extent();
    }

    /**
     * @brief Returns a range to use to visit top-level meta bases.
     * @return An iterable range to use to visit top-level meta bases.
     */
    meta_range<meta_base> base() const ENTT_NOEXCEPT {
        return node->base;
    }

    /**
     * @brief Iterates all top-level meta bases of a meta type.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use base() and entt::meta_range<meta_base> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_base>>
    base(Op op) const {
        for(auto curr: base()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the meta base associated with a given identifier.
     * @param id Unique identifier.
     * @return The meta base associated with the given identifier, if any.
     */
    [[nodiscard]] meta_base base(const id_type id) const {
        return internal::find_if<&node_type::base>([id](const auto *curr) {
            return curr->type()->id == id;
        }, node);
    }

    /**
     * @brief Returns a range to use to visit top-level meta conversion
     * functions.
     * @return An iterable range to use to visit top-level meta conversion
     * functions.
     */
    meta_range<meta_conv> conv() const ENTT_NOEXCEPT {
        return node->conv;
    }

    /**
     * @brief Iterates all top-level meta conversion functions of a meta type.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use conv() and entt::meta_range<meta_conv> instead")]]
    void conv(Op op) const {
        for(auto curr: conv()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the meta conversion function associated with a given type.
     * @tparam Type The type to use to search for a meta conversion function.
     * @return The meta conversion function associated with the given type, if
     * any.
     */
    template<typename Type>
    [[nodiscard]] meta_conv conv() const {
        return internal::find_if<&node_type::conv>([type_id = internal::meta_info<Type>::resolve()->type_id](const auto *curr) {
            return curr->type()->type_id == type_id;
        }, node);
    }

    /**
     * @brief Returns a range to use to visit top-level meta constructors.
     * @return An iterable range to use to visit top-level meta constructors.
     */
    meta_range<meta_ctor> ctor() const ENTT_NOEXCEPT {
        return node->ctor;
    }

    /**
     * @brief Iterates all top-level meta constructors of a meta type.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use ctor() and entt::meta_range<meta_ctor> instead")]]
    void ctor(Op op) const {
        for(auto curr: ctor()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the meta constructor that accepts a given list of types of
     * arguments.
     * @return The requested meta constructor, if any.
     */
    template<typename... Args>
    [[nodiscard]] meta_ctor ctor() const {
        return ctor<Args...>(std::index_sequence_for<Args...>{});
    }

    /**
     * @brief Returns a range to use to visit top-level meta data.
     * @return An iterable range to use to visit top-level meta data.
     */
    meta_range<meta_data> data() const ENTT_NOEXCEPT {
        return node->data;
    }

    /**
     * @brief Iterates all top-level meta data of a meta type.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use ctor() and entt::meta_range<meta_ctor> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_data>>
    data(Op op) const {
        for(auto curr: data()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the meta data associated with a given identifier.
     *
     * The meta data of the base classes will also be visited, if any.
     *
     * @param id Unique identifier.
     * @return The meta data associated with the given identifier, if any.
     */
    [[nodiscard]] meta_data data(const id_type id) const {
        return internal::find_if<&node_type::data>([id](const auto *curr) {
            return curr->id == id;
        }, node);
    }

    /**
     * @brief Returns a range to use to visit top-level meta functions.
     * @return An iterable range to use to visit top-level meta functions.
     */
    meta_range<meta_func> func() const ENTT_NOEXCEPT {
        return node->func;
    }

    /**
     * @brief Iterates all top-level meta functions of a meta type.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use ctor() and entt::meta_range<meta_ctor> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_func>>
    func(Op op) const {
        for(auto curr: func()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the meta function associated with a given identifier.
     *
     * The meta functions of the base classes will also be visited, if any.
     *
     * @param id Unique identifier.
     * @return The meta function associated with the given identifier, if any.
     */
    [[nodiscard]] meta_func func(const id_type id) const {
        return internal::find_if<&node_type::func>([id](const auto *curr) {
            return curr->id == id;
        }, node);
    }

    /**
     * @brief Creates an instance of the underlying type, if possible.
     *
     * To create a valid instance, the parameters must be such that a cast or
     * conversion to the required types is possible. Otherwise, an empty and
     * thus invalid container is returned.
     *
     * @tparam Args Types of arguments to use to construct the instance.
     * @param args Parameters to use to construct the instance.
     * @return A meta any containing the new instance, if any.
     */
    template<typename... Args>
    [[nodiscard]] meta_any construct(Args &&... args) const {
        auto construct_if = [this](meta_any *params) {
            meta_any any{};

            internal::find_if<&node_type::ctor>([params, &any](const auto *curr) {
                return (curr->size == sizeof...(args)) && (any = curr->invoke(params));
            }, node);

            return any;
        };

        if constexpr(sizeof...(Args) == 0) {
            return construct_if(nullptr);
        } else {
            meta_any arguments[]{std::forward<Args>(args)...};
            return construct_if(arguments);
        }
    }

    /**
     * @brief Returns a range to use to visit top-level meta properties.
     * @return An iterable range to use to visit top-level meta properties.
     */
    meta_range<meta_prop> prop() const ENTT_NOEXCEPT {
        return node->prop;
    }

    /**
     * @brief Iterates all top-level meta properties assigned to a meta type.
     * @tparam Op Type of the function object to invoke.
     * @param op A valid function object.
     */
    template<typename Op>
    [[deprecated("use prop() and entt::meta_range<meta_prop> instead")]]
    std::enable_if_t<std::is_invocable_v<Op, meta_prop>>
    prop(Op op) const {
        for(auto curr: prop()) {
            op(curr);
        }
    }

    /**
     * @brief Returns the property associated with a given key.
     *
     * Properties of the base classes will also be visited, if any.
     *
     * @param key The key to use to search for a property.
     * @return The property associated with the given key, if any.
     */
    [[nodiscard]] meta_prop prop(meta_any key) const {
        return internal::find_if<&node_type::prop>([key = std::move(key)](const auto *curr) {
            return curr->key() == key;
        }, node);
    }

    /**
     * @brief Returns true if a meta object is valid, false otherwise.
     * @return True if the meta object is valid, false otherwise.
     */
    [[nodiscard]] explicit operator bool() const ENTT_NOEXCEPT {
        return !(node == nullptr);
    }

    /**
     * @brief Checks if two meta objects refer to the same type.
     * @param other The meta object with which to compare.
     * @return True if the two meta objects refer to the same type, false
     * otherwise.
     */
    [[nodiscard]] bool operator==(const meta_type &other) const ENTT_NOEXCEPT {
        return (!node && !other.node) || (node && other.node && node->type_id == other.node->type_id);
    }

    /*! @brief Removes a meta object from the list of searchable types. */
    void detach() ENTT_NOEXCEPT {
        internal::meta_context::detach(node);
    }

private:
    const node_type *node;
};


/**
 * @brief Checks if two meta objects refer to the same type.
 * @param lhs A meta object, either valid or not.
 * @param rhs A meta object, either valid or not.
 * @return False if the two meta objects refer to the same node, true otherwise.
 */
[[nodiscard]] inline bool operator!=(const meta_type &lhs, const meta_type &rhs) ENTT_NOEXCEPT {
    return !(lhs == rhs);
}


[[nodiscard]] inline meta_type meta_any::type() const ENTT_NOEXCEPT {
    return node;
}


[[nodiscard]] inline meta_type meta_base::parent() const ENTT_NOEXCEPT {
    return node->parent;
}


[[nodiscard]] inline meta_type meta_base::type() const ENTT_NOEXCEPT {
    return node->type();
}


[[nodiscard]] inline meta_type meta_conv::parent() const ENTT_NOEXCEPT {
    return node->parent;
}


[[nodiscard]] inline meta_type meta_conv::type() const ENTT_NOEXCEPT {
    return node->type();
}


[[nodiscard]] inline meta_type meta_ctor::parent() const ENTT_NOEXCEPT {
    return node->parent;
}


[[nodiscard]] inline meta_type meta_ctor::arg(size_type index) const ENTT_NOEXCEPT {
    return index < size() ? node->arg(index) : nullptr;
}


[[nodiscard]] inline meta_type meta_data::parent() const ENTT_NOEXCEPT {
    return node->parent;
}


[[nodiscard]] inline meta_type meta_data::type() const ENTT_NOEXCEPT {
    return node->type();
}


[[nodiscard]] inline meta_type meta_func::parent() const ENTT_NOEXCEPT {
    return node->parent;
}


[[nodiscard]] inline meta_type meta_func::ret() const ENTT_NOEXCEPT {
    return node->ret();
}


[[nodiscard]] inline meta_type meta_func::arg(size_type index) const ENTT_NOEXCEPT {
    return index < size() ? node->arg(index) : nullptr;
}


}


#endif
