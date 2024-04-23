#include <functional>
#include <memory>
#include <optional>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>
}

namespace mcage {
using std::optional;
using std::unique_ptr;

// TODO: replace this with unique_ptr
template <typename Derived, typename T> class w_ptr_wrapper_base {
public:
  w_ptr_wrapper_base() = default;
  w_ptr_wrapper_base(const w_ptr_wrapper_base &) = delete;
  w_ptr_wrapper_base &operator=(const w_ptr_wrapper_base &) = delete;
  w_ptr_wrapper_base(w_ptr_wrapper_base &&other) noexcept : m_ptr(other.m_ptr) {
    other.m_ptr = nullptr;
  };
  w_ptr_wrapper_base &operator=(w_ptr_wrapper_base &&other) noexcept {
    std::swap(m_ptr, other.m_ptr);
    return *this;
  };

  ~w_ptr_wrapper_base() {
    if (m_ptr != nullptr)
      std::invoke(typename Derived::destroy_fn{}, m_ptr);
  }
  explicit w_ptr_wrapper_base(T *m_ptr) : m_ptr(m_ptr) {}

  constexpr auto *get() { return m_ptr; }
  [[nodiscard]] constexpr const auto *get() const { return m_ptr; }

  template <typename... Args>
  static optional<Derived> try_create(Args &&...args) {
    if (auto *ptr = std::invoke(typename Derived::create_fn{},
                                std::forward<Args>(args)...)) [[likely]]
      return Derived{ptr};
    return {};
  }

private:
  T *m_ptr = nullptr;
};

#define W_CLASS_BEGIN(Class, Type)                                             \
  class Class : public w_ptr_wrapper_base<Class, Type> {                       \
  public:                                                                      \
    using Base = w_ptr_wrapper_base<Class, Type>;                              \
    using Base::Base;

#define W_CLASS_END                                                            \
  }                                                                            \
  ;

W_CLASS_BEGIN(display, wl_display)
using create_fn = decltype([]() { return wl_display_create(); });
using destroy_fn = decltype([](wl_display *ptr) { wl_display_destroy(ptr); });

void run() { wl_display_run(get()); }
W_CLASS_END

W_CLASS_BEGIN(backend, wlr_backend)
using create_fn = decltype([](display &d) {
  return wlr_backend_autocreate(d.get(), nullptr);
});
using destroy_fn = decltype([](wlr_backend *ptr) { wlr_backend_destroy(ptr); });

bool start() { return wlr_backend_start(get()); }

W_CLASS_END

W_CLASS_BEGIN(renderer, wlr_renderer)
using create_fn =
    decltype([](backend &b) { return wlr_renderer_autocreate(b.get()); });
using destroy_fn =
    decltype([](wlr_renderer *ptr) { wlr_renderer_destroy(ptr); });

void init_wl_display(display &d) {
  wlr_renderer_init_wl_display(get(), d.get());
}
W_CLASS_END

W_CLASS_BEGIN(allocator, wlr_allocator)
using create_fn = decltype([](backend &b, renderer &r) {
  return wlr_allocator_autocreate(b.get(), r.get());
});
using destroy_fn =
    decltype([](wlr_allocator *ptr) { wlr_allocator_destroy(ptr); });
W_CLASS_END

class server {
public:
  server() {
    m_display = display::try_create().value();
    m_backend = backend::try_create(m_display).value();
    m_renderer = renderer::try_create(m_backend).value();
    m_renderer.init_wl_display(m_display);
    m_allocator = allocator::try_create(m_backend, m_renderer).value();

    m_compositor = wlr_compositor_create(m_display.get(), 5, m_renderer.get());
    m_subcompositor = wlr_subcompositor_create(m_display.get());
    m_data_device_manager = wlr_data_device_manager_create(m_display.get());

    wl_signal_add(&m_backend.get()->events.new_output, &m_new_output_listener);

    // ================
    m_backend.start();
    m_display.run();
  }

private:
  display m_display;
  backend m_backend;
  renderer m_renderer;
  allocator m_allocator;

  // will be destroyed by the display
  wlr_compositor *m_compositor;
  wlr_subcompositor *m_subcompositor;
  wlr_data_device_manager *m_data_device_manager;

private:
  wl_listener m_new_output_listener = {
      .notify = [](wl_listener *listener, void *data) {
        server *self = wl_container_of(listener, self, m_new_output_listener);
        auto *output = static_cast<wlr_output *>(data);

        wlr_output_init_render(output, self->m_allocator.get(),
                               self->m_renderer.get());

        {
          wlr_output_state output_state{};
          wlr_output_state_init(&output_state);
          wlr_output_state_set_enabled(&output_state, true);
          wlr_output_mode *mode = wlr_output_preferred_mode(output);
          if (mode != nullptr)
            wlr_output_state_set_mode(&output_state, mode);
          wlr_output_commit_state(output, &output_state);
          wlr_output_state_finish(&output_state);
        }
      }};

#define UNIUQUE_PTR_MEMBER(Class, Name)                                        \
  unique_ptr<Class, decltype(&Class##_destroy)> Name {                         \
    nullptr, Class##_destroy                                                   \
  }
  // UNIUQUE_PTR_MEMBER(wlr_backend, backend);
};
} // namespace mcage

int main() {
  wlr_log_init(WLR_DEBUG, nullptr);
  mcage::server g{};
}
