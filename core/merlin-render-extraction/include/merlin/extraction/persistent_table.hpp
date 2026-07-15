#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace merlin::extraction {

// Immutable, structurally shared random-access sequence. Updating one record
// allocates a new value and copies only the balanced-tree path to it; all other
// records and subtrees remain shared with older table revisions.
//
// The vector-like surface is intentional: FrameSnapshot was originally
// exposed with std::vector tables, so consumers can continue to use size(),
// iteration, indexing, front(), and back(). Mutating operations use copy-on-
// write and therefore never modify a table held by an older snapshot.
template <typename T>
class PersistentTable {
 private:
  struct Node;
  using NodePtr = std::shared_ptr<const Node>;
  using ValuePtr = std::shared_ptr<const T>;

  struct Node {
    NodePtr left;
    ValuePtr value;
    NodePtr right;
    std::size_t size{};
    int height{};
  };

  static std::size_t SizeOf(const NodePtr& node) noexcept {
    return node ? node->size : 0U;
  }

  static int HeightOf(const NodePtr& node) noexcept {
    return node ? node->height : 0;
  }

  static NodePtr MakeNode(NodePtr left, ValuePtr value, NodePtr right) {
    const auto size = 1U + SizeOf(left) + SizeOf(right);
    const auto height = 1 + std::max(HeightOf(left), HeightOf(right));
    return std::make_shared<const Node>(
        Node{std::move(left), std::move(value), std::move(right), size,
             height});
  }

  static NodePtr RotateLeft(const NodePtr& node) {
    const auto& pivot = node->right;
    auto left = MakeNode(node->left, node->value, pivot->left);
    return MakeNode(std::move(left), pivot->value, pivot->right);
  }

  static NodePtr RotateRight(const NodePtr& node) {
    const auto& pivot = node->left;
    auto right = MakeNode(pivot->right, node->value, node->right);
    return MakeNode(pivot->left, pivot->value, std::move(right));
  }

  static NodePtr Balance(NodePtr node) {
    const auto factor = HeightOf(node->left) - HeightOf(node->right);
    if (factor > 1) {
      if (HeightOf(node->left->left) < HeightOf(node->left->right)) {
        node = MakeNode(RotateLeft(node->left), node->value, node->right);
      }
      return RotateRight(node);
    }
    if (factor < -1) {
      if (HeightOf(node->right->right) < HeightOf(node->right->left)) {
        node = MakeNode(node->left, node->value, RotateRight(node->right));
      }
      return RotateLeft(node);
    }
    return node;
  }

  static NodePtr Insert(const NodePtr& node, std::size_t index,
                        ValuePtr value) {
    if (!node) {
      return MakeNode({}, std::move(value), {});
    }
    const auto left_size = SizeOf(node->left);
    if (index <= left_size) {
      return Balance(MakeNode(Insert(node->left, index, std::move(value)),
                              node->value, node->right));
    }
    return Balance(MakeNode(
        node->left, node->value,
        Insert(node->right, index - left_size - 1U, std::move(value))));
  }

  static std::pair<NodePtr, ValuePtr> RemoveFirst(const NodePtr& node) {
    if (!node->left) {
      return {node->right, node->value};
    }
    auto [left, value] = RemoveFirst(node->left);
    return {Balance(MakeNode(std::move(left), node->value, node->right)),
            std::move(value)};
  }

  static NodePtr Erase(const NodePtr& node, std::size_t index) {
    const auto left_size = SizeOf(node->left);
    if (index < left_size) {
      return Balance(MakeNode(Erase(node->left, index), node->value,
                              node->right));
    }
    if (index > left_size) {
      return Balance(MakeNode(
          node->left, node->value,
          Erase(node->right, index - left_size - 1U)));
    }
    if (!node->left) {
      return node->right;
    }
    if (!node->right) {
      return node->left;
    }
    auto [right, successor] = RemoveFirst(node->right);
    return Balance(
        MakeNode(node->left, std::move(successor), std::move(right)));
  }

  static NodePtr Replace(const NodePtr& node, std::size_t index,
                         ValuePtr value) {
    const auto left_size = SizeOf(node->left);
    if (index < left_size) {
      return MakeNode(Replace(node->left, index, std::move(value)),
                      node->value, node->right);
    }
    if (index > left_size) {
      return MakeNode(node->left, node->value,
                      Replace(node->right, index - left_size - 1U,
                              std::move(value)));
    }
    return MakeNode(node->left, std::move(value), node->right);
  }

  static const T& At(const NodePtr& node, std::size_t index) {
    auto current = node;
    while (current) {
      const auto left_size = SizeOf(current->left);
      if (index < left_size) {
        current = current->left;
      } else if (index > left_size) {
        index -= left_size + 1U;
        current = current->right;
      } else {
        return *current->value;
      }
    }
    throw std::out_of_range("PersistentTable index is out of range");
  }

  static NodePtr Build(const std::vector<ValuePtr>& values, std::size_t first,
                       std::size_t last) {
    if (first == last) {
      return {};
    }
    const auto middle = first + (last - first) / 2U;
    return MakeNode(Build(values, first, middle), values[middle],
                    Build(values, middle + 1U, last));
  }

 public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using const_reference = const T&;
  using reference = T&;

  class const_iterator {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using iterator_concept = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    const_iterator() = default;

    reference operator*() const { return At(root_, index_); }
    pointer operator->() const { return &At(root_, index_); }
    reference operator[](difference_type offset) const {
      return At(root_, static_cast<size_type>(
                           static_cast<difference_type>(index_) + offset));
    }

    const_iterator& operator++() {
      ++index_;
      return *this;
    }
    const_iterator operator++(int) {
      auto copy = *this;
      ++*this;
      return copy;
    }
    const_iterator& operator--() {
      --index_;
      return *this;
    }
    const_iterator operator--(int) {
      auto copy = *this;
      --*this;
      return copy;
    }
    const_iterator& operator+=(difference_type offset) {
      index_ = static_cast<size_type>(static_cast<difference_type>(index_) +
                                      offset);
      return *this;
    }
    const_iterator& operator-=(difference_type offset) {
      return *this += -offset;
    }

    friend const_iterator operator+(const_iterator iterator,
                                    difference_type offset) {
      iterator += offset;
      return iterator;
    }
    friend const_iterator operator+(difference_type offset,
                                    const_iterator iterator) {
      iterator += offset;
      return iterator;
    }
    friend const_iterator operator-(const_iterator iterator,
                                    difference_type offset) {
      iterator -= offset;
      return iterator;
    }
    friend difference_type operator-(const const_iterator& lhs,
                                     const const_iterator& rhs) {
      return static_cast<difference_type>(lhs.index_) -
             static_cast<difference_type>(rhs.index_);
    }
    friend bool operator==(const const_iterator& lhs,
                           const const_iterator& rhs) {
      return lhs.index_ == rhs.index_ && lhs.root_.get() == rhs.root_.get();
    }
    friend bool operator<(const const_iterator& lhs,
                          const const_iterator& rhs) {
      return lhs.index_ < rhs.index_;
    }
    friend bool operator>(const const_iterator& lhs,
                          const const_iterator& rhs) {
      return rhs < lhs;
    }
    friend bool operator<=(const const_iterator& lhs,
                           const const_iterator& rhs) {
      return !(rhs < lhs);
    }
    friend bool operator>=(const const_iterator& lhs,
                           const const_iterator& rhs) {
      return !(lhs < rhs);
    }

   private:
    friend class PersistentTable;
    const_iterator(NodePtr root, size_type index)
        : root_(std::move(root)), index_(index) {}

    NodePtr root_;
    size_type index_{};
  };

  using iterator = const_iterator;

  PersistentTable() = default;

  PersistentTable(std::initializer_list<T> values) {
    assign(std::vector<T>(values));
  }

  explicit PersistentTable(std::vector<T> values) {
    assign(std::move(values));
  }

  [[nodiscard]] bool empty() const noexcept { return !root_; }
  [[nodiscard]] size_type size() const noexcept { return SizeOf(root_); }

  const_reference operator[](size_type index) const { return At(root_, index); }
  reference operator[](size_type index) { return Edit(index); }
  const_reference at(size_type index) const {
    if (index >= size()) {
      throw std::out_of_range("PersistentTable index is out of range");
    }
    return At(root_, index);
  }
  reference at(size_type index) {
    if (index >= size()) {
      throw std::out_of_range("PersistentTable index is out of range");
    }
    return Edit(index);
  }
  const_reference front() const { return At(root_, 0U); }
  reference front() { return Edit(0U); }
  const_reference back() const { return At(root_, size() - 1U); }
  reference back() { return Edit(size() - 1U); }

  const_iterator begin() const noexcept { return {root_, 0U}; }
  const_iterator end() const noexcept { return {root_, size()}; }
  const_iterator cbegin() const noexcept { return begin(); }
  const_iterator cend() const noexcept { return end(); }
  iterator begin() noexcept { return {root_, 0U}; }
  iterator end() noexcept { return {root_, size()}; }

  void clear() noexcept { root_.reset(); }
  void reserve(size_type) noexcept {}

  void assign(std::vector<T> values) {
    std::vector<ValuePtr> shared;
    shared.reserve(values.size());
    for (auto& value : values) {
      shared.push_back(std::make_shared<const T>(std::move(value)));
    }
    root_ = Build(shared, 0U, shared.size());
  }

  void push_back(const T& value) { insert(end(), value); }
  void push_back(T&& value) { insert(end(), std::move(value)); }

  template <typename... Args>
  reference emplace_back(Args&&... args) {
    push_back(T(std::forward<Args>(args)...));
    return back();
  }

  iterator insert(const_iterator position, const T& value) {
    return InsertValue(position, std::make_shared<const T>(value));
  }
  iterator insert(const_iterator position, T&& value) {
    return InsertValue(position,
                       std::make_shared<const T>(std::move(value)));
  }

  iterator erase(const_iterator position) {
    if (position.index_ >= size()) {
      return end();
    }
    const auto index = position.index_;
    root_ = Erase(root_, index);
    return {root_, index};
  }

  void replace(size_type index, const T& value) {
    ReplaceValue(index, std::make_shared<const T>(value));
  }
  void replace(size_type index, T&& value) {
    ReplaceValue(index, std::make_shared<const T>(std::move(value)));
  }

  // Identity is useful for validating structural sharing without exposing the
  // tree representation itself.
  [[nodiscard]] const void* record_identity(size_type index) const {
    return &At(root_, index);
  }

 private:
  reference Edit(size_type index) {
    if (index >= size()) {
      throw std::out_of_range("PersistentTable index is out of range");
    }
    auto value = std::make_shared<T>(At(root_, index));
    auto* edited = value.get();
    root_ = Replace(root_, index, std::move(value));
    return *edited;
  }

  iterator InsertValue(const_iterator position, ValuePtr value) {
    const auto index = std::min(position.index_, size());
    root_ = Insert(root_, index, std::move(value));
    return {root_, index};
  }

  void ReplaceValue(size_type index, ValuePtr value) {
    if (index >= size()) {
      throw std::out_of_range("PersistentTable index is out of range");
    }
    root_ = Replace(root_, index, std::move(value));
  }

  NodePtr root_;
};

}  // namespace merlin::extraction
