#include "window.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <string>

namespace merlin::viewport {
namespace {

Key TranslateKey(int key) noexcept {
  switch (key) {
    case GLFW_KEY_ESCAPE: return Key::Escape;
    case GLFW_KEY_LEFT: return Key::Left;
    case GLFW_KEY_RIGHT: return Key::Right;
    case GLFW_KEY_UP: return Key::Up;
    case GLFW_KEY_DOWN: return Key::Down;
    case GLFW_KEY_S: return Key::Screenshot;
    default: return Key::Unknown;
  }
}

std::runtime_error GlfwError(std::string_view operation) {
  const char* detail{};
  const auto code = glfwGetError(&detail);
  return std::runtime_error(std::string(operation) + " failed (GLFW " +
                            std::to_string(code) + ")" +
                            (detail == nullptr ? "" : ": " +
                                                     std::string(detail)));
}

class GlfwWindow final : public Window {
 public:
  GlfwWindow(std::string_view title, std::uint32_t width,
             std::uint32_t height, bool visible) {
    if (glfwInit() != GLFW_TRUE) {
      throw GlfwError("initialize GLFW");
    }
    initialized_ = true;
    if (glfwVulkanSupported() != GLFW_TRUE) {
      throw GlfwError("query GLFW Vulkan support");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    const std::string owned_title(title);
    window_ = glfwCreateWindow(static_cast<int>(width),
                               static_cast<int>(height), owned_title.c_str(),
                               nullptr, nullptr);
    if (window_ == nullptr) {
      throw GlfwError("create GLFW viewport window");
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowCloseCallback(window_, [](GLFWwindow* window) {
      auto& self = Self(window);
      self.events_.push_back({EventType::Close});
      glfwSetWindowShouldClose(window, GLFW_FALSE);
    });
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* window, int width,
                                               int height) {
      auto& self = Self(window);
      self.width_ = width < 0 ? 0U : static_cast<std::uint32_t>(width);
      self.height_ = height < 0 ? 0U : static_cast<std::uint32_t>(height);
      Event event{EventType::Resize};
      event.width = self.width_;
      event.height = self.height_;
      self.events_.push_back(event);
    });
    glfwSetKeyCallback(window_, [](GLFWwindow* window, int key, int,
                                   int action, int) {
      if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        Event event{EventType::KeyDown};
        event.key = TranslateKey(key);
        Self(window).events_.push_back(event);
      }
    });
    glfwSetMouseButtonCallback(window_, [](GLFWwindow* window, int button,
                                           int action, int) {
      if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        double x{};
        double y{};
        glfwGetCursorPos(window, &x, &y);
        Event event{EventType::PointerDown};
        event.x = static_cast<std::int32_t>(x);
        event.y = static_cast<std::int32_t>(y);
        Self(window).events_.push_back(event);
      }
    });
    glfwSetCursorPosCallback(window_, [](GLFWwindow* window, double x,
                                        double y) {
      Event event{EventType::PointerMove};
      event.x = static_cast<std::int32_t>(x);
      event.y = static_cast<std::int32_t>(y);
      Self(window).events_.push_back(event);
    });
    int framebuffer_width{};
    int framebuffer_height{};
    glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);
    width_ = static_cast<std::uint32_t>(std::max(0, framebuffer_width));
    height_ = static_cast<std::uint32_t>(std::max(0, framebuffer_height));
  }

  ~GlfwWindow() override {
    if (window_ != nullptr) {
      glfwDestroyWindow(window_);
    }
    if (initialized_) {
      glfwTerminate();
    }
  }

  bool PollEvent(Event& event) override {
    glfwPollEvents();
    if (events_.empty()) {
      return false;
    }
    event = events_.front();
    events_.pop_front();
    return true;
  }

  void WaitForEvent() override { glfwWaitEvents(); }

  void SetTitle(std::string_view title) override {
    const std::string owned(title);
    glfwSetWindowTitle(window_, owned.c_str());
  }

  void SetSize(std::uint32_t width, std::uint32_t height) override {
    glfwSetWindowSize(window_, static_cast<int>(width),
                      static_cast<int>(height));
  }

  void* native_window() const noexcept override { return window_; }
  std::uint32_t width() const noexcept override { return width_; }
  std::uint32_t height() const noexcept override { return height_; }

 private:
  static GlfwWindow& Self(GLFWwindow* window) {
    return *static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
  }

  bool initialized_{};
  GLFWwindow* window_{};
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::deque<Event> events_;
};

}  // namespace

std::unique_ptr<Window> Window::Create(std::string_view title,
                                       std::uint32_t width,
                                       std::uint32_t height, bool visible) {
  return std::make_unique<GlfwWindow>(title, width, height, visible);
}

}  // namespace merlin::viewport
