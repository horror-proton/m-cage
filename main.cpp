#include <functional>
#include <memory>
#include <optional>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
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

W_CLASS_BEGIN(output_layout, wlr_output_layout)
using create_fn = decltype([]() { return wlr_output_layout_create(); });
using destroy_fn =
    decltype([](wlr_output_layout *ptr) { wlr_output_layout_destroy(ptr); });
W_CLASS_END

W_CLASS_BEGIN(cursor, wlr_cursor)
using create_fn = decltype([]() { return wlr_cursor_create(); });
using destroy_fn = decltype([](wlr_cursor *ptr) { wlr_cursor_destroy(ptr); });
void attach_output_layout(output_layout &layout) {
  wlr_cursor_attach_output_layout(get(), layout.get());
}

void attach_input_device(wlr_input_device *device) {
  wlr_cursor_attach_input_device(get(), device);
}
W_CLASS_END

W_CLASS_BEGIN(scene, wlr_scene)
using create_fn = decltype([]() { return wlr_scene_create(); });
using destroy_fn = std::default_delete<wlr_scene>;
W_CLASS_END

W_CLASS_BEGIN(seat, wlr_seat)
using create_fn = decltype([](display &d, const char *name) {
  return wlr_seat_create(d.get(), name);
});
using destroy_fn = decltype([](wlr_seat *ptr) { wlr_seat_destroy(ptr); });

void pointer_notify_frame() { wlr_seat_pointer_notify_frame(get()); }
W_CLASS_END

W_CLASS_BEGIN(xcursor_manager, wlr_xcursor_manager)
using create_fn = decltype([](const char *name, uint32_t size) {
  return wlr_xcursor_manager_create(name, size);
});
using destroy_fn = decltype([](wlr_xcursor_manager *ptr) {
  wlr_xcursor_manager_destroy(ptr);
});
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

    wl_signal_add(&m_backend.get()->events.new_output, &m_listener_new_output);

    m_output_layout = output_layout::try_create().value();
    m_scene = scene::try_create().value();

    m_scene_output_layout =
        wlr_scene_attach_output_layout(m_scene.get(), m_output_layout.get());

    m_cursor = cursor::try_create().value();
    m_cursor.attach_output_layout(m_output_layout);

    wl_signal_add(&m_backend.get()->events.new_input, &m_listener_new_input);

    wl_signal_add(&m_cursor.get()->events.motion, &m_listener_cursor_motion);

    wl_signal_add(&m_cursor.get()->events.frame, &m_listener_cursor_frame);

    m_seat = seat::try_create(m_display, "seat0").value();

    wl_signal_add(&m_seat.get()->events.request_set_cursor,
                  &m_listener_request_cursor);

    m_xcursor_manager = xcursor_manager::try_create("default", 24).value();

    // ================
    const char *socket = wl_display_add_socket_auto(m_display.get());
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

  scene m_scene;
  output_layout m_output_layout;

  wlr_scene_output_layout *m_scene_output_layout;

  cursor m_cursor;
  seat m_seat;

  xcursor_manager m_xcursor_manager;

private:
  template <std::size_t Offset, typename Data = void, typename F>
  static constexpr auto get_listener_fn(F)
      -> decltype(std::declval<wl_listener>().notify) {
    return [](wl_listener *listener, void *data) -> void {
      // NOLINTBEGIN
      auto *self = reinterpret_cast<server *>(
          reinterpret_cast<char *>(listener) - Offset);
      // NOLINTEND
      std::invoke(F{}, self, static_cast<Data *>(data));
    };
  }

  template <std::size_t Offset, typename Data = void, typename F>
  static constexpr wl_listener get_listener(F) {
    return wl_listener{
        .notify = get_listener_fn<Offset, Data>(F{}),
    };
  }

  // NOLINTBEGIN
#define SERVER_LISTENER_MEMBER(Name, Type)                                     \
  wl_listener m_listener_##Name =                                              \
      get_listener<offsetof(server, m_listener_##Name), Type>
  // NOLINTEND

  SERVER_LISTENER_MEMBER(new_output, wlr_output)
  ([](server *self, wlr_output *output) {
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

    auto *l_output =
        wlr_output_layout_add_auto(self->m_output_layout.get(), output);
    auto *scene_output = wlr_scene_output_create(self->m_scene.get(), output);
    wlr_scene_output_layout_add_output(self->m_scene_output_layout, l_output,
                                       scene_output);
  });

  SERVER_LISTENER_MEMBER(new_input, wlr_input_device)
  ([](server *self, wlr_input_device *device) {
    switch (device->type) {
    case WLR_INPUT_DEVICE_POINTER: {
      wlr_log(WLR_DEBUG, "New pointer device: %s", device->name);
      self->m_cursor.attach_input_device(device);
      break;
    }
    case WLR_INPUT_DEVICE_KEYBOARD: {
      break;
    }
    default:
      break;
    }
  });

  SERVER_LISTENER_MEMBER(request_cursor,
                         wlr_seat_pointer_request_set_cursor_event)
  ([](server *self, wlr_seat_pointer_request_set_cursor_event *event) {
    auto *client = self->m_seat.get()->pointer_state.focused_client;
    wlr_log(WLR_DEBUG, "request cursor %p", client);
    if (client == event->seat_client)
      wlr_cursor_set_surface(self->m_cursor.get(), event->surface,
                             event->hotspot_x, event->hotspot_y);
  });

  SERVER_LISTENER_MEMBER(cursor_frame, void)
  ([](server *self, void *) { self->m_seat.pointer_notify_frame(); });

  SERVER_LISTENER_MEMBER(cursor_motion, wlr_pointer_motion_event)
  ([](server *self, wlr_pointer_motion_event *event) {
    wlr_cursor_move(self->m_cursor.get(), &event->pointer->base, event->delta_x,
                    event->delta_y);
    wlr_cursor_set_xcursor(self->m_cursor.get(), self->m_xcursor_manager.get(),
                           "default");
  });

#undef SERVER_LISTENER_MEMBER
};
} // namespace mcage

int main() {
  wlr_log_init(WLR_DEBUG, nullptr);
  mcage::server g{};
}
