#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace merlin::viewport {

enum class EventType {
  Close,
  Resize,
  KeyDown,
  PointerDown,
  PointerMove,
};

enum class Key {
  Unknown,
  Escape,
  Left,
  Right,
  Up,
  Down,
  Screenshot,
};

struct Event {
  EventType type{EventType::Close};
  Key key{Key::Unknown};
  std::uint32_t width{};
  std::uint32_t height{};
  std::int32_t x{};
  std::int32_t y{};
};

class Window {
 public:
  static std::unique_ptr<Window> Create(std::string_view title,
                                        std::uint32_t width,
                                        std::uint32_t height, bool visible);
  virtual ~Window() = default;

  [[nodiscard]] virtual bool PollEvent(Event& event) = 0;
  virtual void WaitForEvent() = 0;
  virtual void SetTitle(std::string_view title) = 0;
  virtual void SetSize(std::uint32_t width, std::uint32_t height) = 0;
  [[nodiscard]] virtual void* native_window() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
};

}  // namespace merlin::viewport
