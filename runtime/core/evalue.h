/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <executorch/runtime/core/exec_aten/exec_aten.h>
#include <executorch/runtime/core/tag.h>
#include <executorch/runtime/platform/assert.h>

namespace executorch {
namespace runtime {

struct EValue;

namespace internal {

// Tensor gets proper reference treatment because its expensive to copy in aten
// mode, all other types are just copied.
template <typename T>
struct evalue_to_const_ref_overload_return {
  using type = T;
};

template <>
struct evalue_to_const_ref_overload_return<executorch::aten::Tensor> {
  using type = const executorch::aten::Tensor&;
};

template <typename T>
struct evalue_to_ref_overload_return {
  using type = T;
};

template <>
struct evalue_to_ref_overload_return<executorch::aten::Tensor> {
  using type = executorch::aten::Tensor&;
};

} // namespace internal

/*
 * Helper class used to correlate EValues in the executor table, with the
 * unwrapped list of the proper type. Because values in the runtime's values
 * table can change during execution, we cannot statically allocate list of
 * objects at deserialization. Imagine the serialized list says index 0 in the
 * value table is element 2 in the list, but during execution the value in
 * element 2 changes (in the case of tensor this means the TensorImpl* stored in
 * the tensor changes). To solve this instead they must be created dynamically
 * whenever they are used.
 */
template <typename T>
class BoxedEvalueList {
 public:
  BoxedEvalueList() = default;
  /*
   * Wrapped_vals is a list of pointers into the values table of the runtime
   * whose destinations correlate with the elements of the list, unwrapped_vals
   * is a container of the same size whose serves as memory to construct the
   * unwrapped vals.
   */
  BoxedEvalueList(EValue** wrapped_vals, T* unwrapped_vals, int size)
      : wrapped_vals_(wrapped_vals, size), unwrapped_vals_(unwrapped_vals) {}
  /*
   * Constructs and returns the list of T specified by the EValue pointers
   */
  executorch::aten::ArrayRef<T> get() const;

 private:
  // Source of truth for the list
  executorch::aten::ArrayRef<EValue*> wrapped_vals_;
  // Same size as wrapped_vals
  mutable T* unwrapped_vals_;
};

template <>
executorch::aten::ArrayRef<std::optional<executorch::aten::Tensor>>
BoxedEvalueList<std::optional<executorch::aten::Tensor>>::get() const;

// Aggregate typing system similar to IValue only slimmed down with less
// functionality, no dependencies on atomic, and fewer supported types to better
// suit embedded systems (ie no intrusive ptr)
struct EValue {
  union Payload {
    // When in ATen mode at::Tensor is not trivially copyable, this nested union
    // lets us handle tensor as a special case while leaving the rest of the
    // fields in a simple state instead of requiring a switch on tag everywhere.
    union TriviallyCopyablePayload {
      TriviallyCopyablePayload() : as_int(0) {}
      // Scalar supported through these 3 types
      int64_t as_int;
      double as_double;
      bool as_bool;
      // TODO(jakeszwe): convert back to pointers to optimize size of this
      // struct
      executorch::aten::ArrayRef<char> as_string;
      executorch::aten::ArrayRef<double> as_double_list;
      executorch::aten::ArrayRef<bool> as_bool_list;
      BoxedEvalueList<int64_t> as_int_list;
      BoxedEvalueList<executorch::aten::Tensor> as_tensor_list;
      BoxedEvalueList<std::optional<executorch::aten::Tensor>>
          as_list_optional_tensor;
    } copyable_union;

    // Since a Tensor just holds a TensorImpl*, there's no value to use Tensor*
    // here.
    executorch::aten::Tensor as_tensor;

    Payload() {}
    ~Payload() {}
  };

  // Data storage and type tag
  Payload payload;
  Tag tag;

  // Basic ctors and assignments
  EValue(const EValue& rhs) : EValue(rhs.payload, rhs.tag) {}

  EValue(EValue&& rhs) noexcept : tag(rhs.tag) {
    moveFrom(std::move(rhs));
  }

  EValue& operator=(EValue&& rhs) & noexcept {
    if (&rhs == this) {
      return *this;
    }

    destroy();
    moveFrom(std::move(rhs));
    return *this;
  }

  EValue& operator=(EValue const& rhs) & {
    // Define copy assignment through copy ctor and move assignment
    *this = EValue(rhs);
    return *this;
  }

  ~EValue() {
    destroy();
  }

  /****** None Type ******/
  EValue() : tag(Tag::None) {
    payload.copyable_union.as_int = 0;
  }

  bool isNone() const {
    return tag == Tag::None;
  }

  /****** Int Type ******/
  /*implicit*/ EValue(int64_t i) : tag(Tag::Int) {
    payload.copyable_union.as_int = i;
  }

  bool isInt() const {
    return tag == Tag::Int;
  }

  int64_t toInt() const {
    ET_CHECK_MSG(isInt(), "EValue is not an int.");
    return payload.copyable_union.as_int;
  }

  /****** Double Type ******/
  /*implicit*/ EValue(double d) : tag(Tag::Double) {
    payload.copyable_union.as_double = d;
  }

  bool isDouble() const {
    return tag == Tag::Double;
  }

  double toDouble() const {
    ET_CHECK_MSG(isDouble(), "EValue is not a Double.");
    return payload.copyable_union.as_double;
  }

  /****** Bool Type ******/
  /*implicit*/ EValue(bool b) : tag(Tag::Bool) {
    payload.copyable_union.as_bool = b;
  }

  bool isBool() const {
    return tag == Tag::Bool;
  }

  bool toBool() const {
    ET_CHECK_MSG(isBool(), "EValue is not a Bool.");
    return payload.copyable_union.as_bool;
  }

  /****** Scalar Type ******/
  /// Construct an EValue using the implicit value of a Scalar.
  /*implicit*/ EValue(executorch::aten::Scalar s) {
    if (s.isIntegral(false)) {
      tag = Tag::Int;
      payload.copyable_union.as_int = s.to<int64_t>();
    } else if (s.isFloatingPoint()) {
      tag = Tag::Double;
      payload.copyable_union.as_double = s.to<double>();
    } else if (s.isBoolean()) {
      tag = Tag::Bool;
      payload.copyable_union.as_bool = s.to<bool>();
    } else {
      ET_CHECK_MSG(false, "Scalar passed to EValue is not initialized.");
    }
  }

  bool isScalar() const {
    return tag == Tag::Int || tag == Tag::Double || tag == Tag::Bool;
  }

  executorch::aten::Scalar toScalar() const {
    // Convert from implicit value to Scalar using implicit constructors.

    if (isDouble()) {
      return toDouble();
    } else if (isInt()) {
      return toInt();
    } else if (isBool()) {
      return toBool();
    } else {
      ET_CHECK_MSG(false, "EValue is not a Scalar.");
    }
  }

  /****** Tensor Type ******/
  /*implicit*/ EValue(executorch::aten::Tensor t) : tag(Tag::Tensor) {
    // When built in aten mode, at::Tensor has a non trivial constructor
    // destructor, so regular assignment to a union field is UB. Instead we must
    // go through placement new (which causes a refcount bump).
    new (&payload.as_tensor) executorch::aten::Tensor(t);
  }

  // Template constructor that allows construction from types that can be
  // dereferenced to produce a type that EValue can be implicitly constructed
  // from.
  template <
      typename T,
      typename = typename std::enable_if<std::is_convertible<
          decltype(*std::forward<T>(std::declval<T>())), // declval to simulate
                                                         // forwarding
          EValue>::value>::type>
  /*implicit*/ EValue(T&& value) {
    ET_CHECK_MSG(value != nullptr, "Pointer is null.");
    // Note that this ctor does not initialize this->tag directly; it is set by
    // moving in the new value.
    moveFrom(*std::forward<T>(value));
  }

  // Delete constructor for raw pointers to ensure they cannot be used.
  template <typename T>
  explicit EValue(T* value) = delete;

  bool isTensor() const {
    return tag == Tag::Tensor;
  }

  executorch::aten::Tensor toTensor() && {
    ET_CHECK_MSG(isTensor(), "EValue is not a Tensor.");
    auto res = std::move(payload.as_tensor);
    clearToNone();
    return res;
  }

  executorch::aten::Tensor& toTensor() & {
    ET_CHECK_MSG(isTensor(), "EValue is not a Tensor.");
    return payload.as_tensor;
  }

  const executorch::aten::Tensor& toTensor() const& {
    ET_CHECK_MSG(isTensor(), "EValue is not a Tensor.");
    return payload.as_tensor;
  }

  /****** String Type ******/
  /*implicit*/ EValue(const char* s, size_t size) : tag(Tag::String) {
    payload.copyable_union.as_string =
        executorch::aten::ArrayRef<char>(s, size);
  }

  bool isString() const {
    return tag == Tag::String;
  }

  std::string_view toString() const {
    ET_CHECK_MSG(isString(), "EValue is not a String.");
    return std::string_view(
        payload.copyable_union.as_string.data(),
        payload.copyable_union.as_string.size());
  }

  /****** Int List Type ******/
  /*implicit*/ EValue(BoxedEvalueList<int64_t> i) : tag(Tag::ListInt) {
    payload.copyable_union.as_int_list = i;
  }

  bool isIntList() const {
    return tag == Tag::ListInt;
  }

  executorch::aten::ArrayRef<int64_t> toIntList() const {
    ET_CHECK_MSG(isIntList(), "EValue is not an Int List.");
    return payload.copyable_union.as_int_list.get();
  }

  /****** Bool List Type ******/
  /*implicit*/ EValue(executorch::aten::ArrayRef<bool> b) : tag(Tag::ListBool) {
    payload.copyable_union.as_bool_list = b;
  }

  bool isBoolList() const {
    return tag == Tag::ListBool;
  }

  executorch::aten::ArrayRef<bool> toBoolList() const {
    ET_CHECK_MSG(isBoolList(), "EValue is not a Bool List.");
    return payload.copyable_union.as_bool_list;
  }

  /****** Double List Type ******/
  /*implicit*/ EValue(executorch::aten::ArrayRef<double> d)
      : tag(Tag::ListDouble) {
    payload.copyable_union.as_double_list = d;
  }

  bool isDoubleList() const {
    return tag == Tag::ListDouble;
  }

  executorch::aten::ArrayRef<double> toDoubleList() const {
    ET_CHECK_MSG(isDoubleList(), "EValue is not a Double List.");
    return payload.copyable_union.as_double_list;
  }

  /****** Tensor List Type ******/
  /*implicit*/ EValue(BoxedEvalueList<executorch::aten::Tensor> t)
      : tag(Tag::ListTensor) {
    payload.copyable_union.as_tensor_list = t;
  }

  bool isTensorList() const {
    return tag == Tag::ListTensor;
  }

  executorch::aten::ArrayRef<executorch::aten::Tensor> toTensorList() const {
    ET_CHECK_MSG(isTensorList(), "EValue is not a Tensor List.");
    return payload.copyable_union.as_tensor_list.get();
  }

  /****** List Optional Tensor Type ******/
  /*implicit*/ EValue(
      BoxedEvalueList<std::optional<executorch::aten::Tensor>> t)
      : tag(Tag::ListOptionalTensor) {
    payload.copyable_union.as_list_optional_tensor = t;
  }

  bool isListOptionalTensor() const {
    return tag == Tag::ListOptionalTensor;
  }

  executorch::aten::ArrayRef<std::optional<executorch::aten::Tensor>>
  toListOptionalTensor() const {
    return payload.copyable_union.as_list_optional_tensor.get();
  }

  /****** ScalarType Type ******/
  executorch::aten::ScalarType toScalarType() const {
    ET_CHECK_MSG(isInt(), "EValue is not a ScalarType.");
    return static_cast<executorch::aten::ScalarType>(
        payload.copyable_union.as_int);
  }

  /****** MemoryFormat Type ******/
  executorch::aten::MemoryFormat toMemoryFormat() const {
    ET_CHECK_MSG(isInt(), "EValue is not a MemoryFormat.");
    return static_cast<executorch::aten::MemoryFormat>(
        payload.copyable_union.as_int);
  }

  /****** Layout Type ******/
  executorch::aten::Layout toLayout() const {
    ET_CHECK_MSG(isInt(), "EValue is not a Layout.");
    return static_cast<executorch::aten::Layout>(payload.copyable_union.as_int);
  }

  /****** Device Type ******/
  executorch::aten::Device toDevice() const {
    ET_CHECK_MSG(isInt(), "EValue is not a Device.");
    return executorch::aten::Device(
        static_cast<executorch::aten::DeviceType>(
            payload.copyable_union.as_int),
        -1);
  }

  template <typename T>
  T to() &&;
  template <typename T>
  typename internal::evalue_to_const_ref_overload_return<T>::type to() const&;
  template <typename T>
  typename internal::evalue_to_ref_overload_return<T>::type to() &;

  /**
   * Converts the EValue to an optional object that can represent both T and
   * an uninitialized state.
   */
  template <typename T>
  inline std::optional<T> toOptional() const {
    if (this->isNone()) {
      return executorch::aten::nullopt;
    }
    return this->to<T>();
  }

 private:
  // Pre cond: the payload value has had its destructor called
  void clearToNone() noexcept {
    payload.copyable_union.as_int = 0;
    tag = Tag::None;
  }

  // Shared move logic
  void moveFrom(EValue&& rhs) noexcept {
    if (rhs.isTensor()) {
      new (&payload.as_tensor)
          executorch::aten::Tensor(std::move(rhs.payload.as_tensor));
      rhs.payload.as_tensor.~Tensor();
    } else {
      payload.copyable_union = rhs.payload.copyable_union;
    }
    tag = rhs.tag;
    rhs.clearToNone();
  }

  // Destructs stored tensor if there is one
  void destroy() {
    // Necessary for ATen tensor to refcount decrement the intrusive_ptr to
    // tensorimpl that got a refcount increment when we placed it in the evalue,
    // no-op if executorch tensor #ifdef could have a
    // minor performance bump for a code maintainability hit
    if (isTensor()) {
      payload.as_tensor.~Tensor();
    } else if (isTensorList()) {
      for (auto& tensor : toTensorList()) {
        tensor.~Tensor();
      }
    } else if (isListOptionalTensor()) {
      for (auto& optional_tensor : toListOptionalTensor()) {
        optional_tensor.~optional();
      }
    }
  }

  EValue(const Payload& p, Tag t) : tag(t) {
    if (isTensor()) {
      new (&payload.as_tensor) executorch::aten::Tensor(p.as_tensor);
    } else {
      payload.copyable_union = p.copyable_union;
    }
  }
};

#define EVALUE_DEFINE_TO(T, method_name)                                       \
  template <>                                                                  \
  inline T EValue::to<T>()&& {                                                 \
    return static_cast<T>(std::move(*this).method_name());                     \
  }                                                                            \
  template <>                                                                  \
  inline ::executorch::runtime::internal::evalue_to_const_ref_overload_return< \
      T>::type                                                                 \
  EValue::to<T>() const& {                                                     \
    typedef ::executorch::runtime::internal::                                  \
        evalue_to_const_ref_overload_return<T>::type return_type;              \
    return static_cast<return_type>(this->method_name());                      \
  }                                                                            \
  template <>                                                                  \
  inline ::executorch::runtime::internal::evalue_to_ref_overload_return<       \
      T>::type                                                                 \
  EValue::to<T>()& {                                                           \
    typedef ::executorch::runtime::internal::evalue_to_ref_overload_return<    \
        T>::type return_type;                                                  \
    return static_cast<return_type>(this->method_name());                      \
  }

EVALUE_DEFINE_TO(executorch::aten::Scalar, toScalar)
EVALUE_DEFINE_TO(int64_t, toInt)
EVALUE_DEFINE_TO(bool, toBool)
EVALUE_DEFINE_TO(double, toDouble)
EVALUE_DEFINE_TO(std::string_view, toString)
EVALUE_DEFINE_TO(executorch::aten::ScalarType, toScalarType)
EVALUE_DEFINE_TO(executorch::aten::MemoryFormat, toMemoryFormat)
EVALUE_DEFINE_TO(executorch::aten::Layout, toLayout)
EVALUE_DEFINE_TO(executorch::aten::Device, toDevice)
// Tensor and Optional Tensor
EVALUE_DEFINE_TO(
    std::optional<executorch::aten::Tensor>,
    toOptional<executorch::aten::Tensor>)
EVALUE_DEFINE_TO(executorch::aten::Tensor, toTensor)

// IntList and Optional IntList
EVALUE_DEFINE_TO(executorch::aten::ArrayRef<int64_t>, toIntList)
EVALUE_DEFINE_TO(
    std::optional<executorch::aten::ArrayRef<int64_t>>,
    toOptional<executorch::aten::ArrayRef<int64_t>>)

// DoubleList and Optional DoubleList
EVALUE_DEFINE_TO(executorch::aten::ArrayRef<double>, toDoubleList)
EVALUE_DEFINE_TO(
    std::optional<executorch::aten::ArrayRef<double>>,
    toOptional<executorch::aten::ArrayRef<double>>)

// BoolList and Optional BoolList
EVALUE_DEFINE_TO(executorch::aten::ArrayRef<bool>, toBoolList)
EVALUE_DEFINE_TO(
    std::optional<executorch::aten::ArrayRef<bool>>,
    toOptional<executorch::aten::ArrayRef<bool>>)

// TensorList and Optional TensorList
EVALUE_DEFINE_TO(
    executorch::aten::ArrayRef<executorch::aten::Tensor>,
    toTensorList)
EVALUE_DEFINE_TO(
    std::optional<executorch::aten::ArrayRef<executorch::aten::Tensor>>,
    toOptional<executorch::aten::ArrayRef<executorch::aten::Tensor>>)

// List of Optional Tensor
EVALUE_DEFINE_TO(
    executorch::aten::ArrayRef<std::optional<executorch::aten::Tensor>>,
    toListOptionalTensor)
#undef EVALUE_DEFINE_TO

template <typename T>
executorch::aten::ArrayRef<T> BoxedEvalueList<T>::get() const {
  for (typename executorch::aten::ArrayRef<T>::size_type i = 0;
       i < wrapped_vals_.size();
       i++) {
    ET_CHECK(wrapped_vals_[i] != nullptr);
    unwrapped_vals_[i] = wrapped_vals_[i]->template to<T>();
  }
  return executorch::aten::ArrayRef<T>{unwrapped_vals_, wrapped_vals_.size()};
}

} // namespace runtime
} // namespace executorch

namespace torch {
namespace executor {
// TODO(T197294990): Remove these deprecated aliases once all users have moved
// to the new `::executorch` namespaces.
using ::executorch::runtime::BoxedEvalueList;
using ::executorch::runtime::EValue;
} // namespace executor
} // namespace torch
