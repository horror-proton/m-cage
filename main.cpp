#include <cassert>
#include <csignal>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <wayland-server-core.h>

extern "C" {
#include <spawn.h>
#include <wait.h>
}

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
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
}

namespace mcage {
using std::optional;
using std::unique_ptr;

template <typename T> struct default_free {
  void operator()(T *ptr) const {
    std::free(ptr); // NOLINT
  }
};

// TODO: replace this with unique_ptr
template <typename Derived, typename T> class w_ptr_wrapper_base {
public:
  using base = w_ptr_wrapper_base<Derived, T>;

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

  auto &events() { return m_ptr->events; }

private:
  T *m_ptr = nullptr;
};

class renderer;

class display : public w_ptr_wrapper_base<display, wl_display> {
public:
  using base::base;
  using create_fn = decltype([]() { return wl_display_create(); });
  using destroy_fn = decltype([](wl_display *ptr) { wl_display_destroy(ptr); });

  void run() { wl_display_run(get()); }

  void terminate() { wl_display_terminate(get()); }

  const char *add_socket_auto() { return wl_display_add_socket_auto(get()); }

  auto *init_xdg_shell(uint32_t version) {
    if (m_xdg_shell == nullptr)
      m_xdg_shell = wlr_xdg_shell_create(get(), version);
    return m_xdg_shell;
  }

  auto &xdg_shell_events() {
    assert(m_xdg_shell != nullptr);
    return m_xdg_shell->events;
  }

  auto *init_compositor(uint32_t version, renderer &renderer);

  auto *init_subcompositor() {
    if (m_subcompositor == nullptr)
      m_subcompositor = wlr_subcompositor_create(get());
    return m_subcompositor;
  }

  auto *init_data_device_manager() {
    if (m_data_device_manager == nullptr)
      m_data_device_manager = wlr_data_device_manager_create(get());
    return m_data_device_manager;
  }

private:
  // will be destroyed by the display
  wlr_xdg_shell *m_xdg_shell = nullptr;
  wlr_compositor *m_compositor = nullptr;
  wlr_subcompositor *m_subcompositor = nullptr;
  wlr_data_device_manager *m_data_device_manager = nullptr;
};

class backend : public w_ptr_wrapper_base<backend, wlr_backend> {
public:
  using base::base;
  using create_fn = decltype([](display &d) {
    return wlr_backend_autocreate(d.get(), nullptr);
  });
  using destroy_fn =
      decltype([](wlr_backend *ptr) { wlr_backend_destroy(ptr); });

  bool start() { return wlr_backend_start(get()); }
};

class renderer : public w_ptr_wrapper_base<renderer, wlr_renderer> {
public:
  using base::base;
  using create_fn =
      decltype([](backend &b) { return wlr_renderer_autocreate(b.get()); });
  using destroy_fn =
      decltype([](wlr_renderer *ptr) { wlr_renderer_destroy(ptr); });

  void init_wl_display(display &d) {
    wlr_renderer_init_wl_display(get(), d.get());
  }
};

auto *display::init_compositor(uint32_t version, renderer &renderer) {
  if (m_compositor == nullptr)
    m_compositor = wlr_compositor_create(get(), version, renderer.get());
  return m_compositor;
}

class allocator : public w_ptr_wrapper_base<allocator, wlr_allocator> {
public:
  using base::base;
  using create_fn = decltype([](backend &b, renderer &r) {
    return wlr_allocator_autocreate(b.get(), r.get());
  });
  using destroy_fn =
      decltype([](wlr_allocator *ptr) { wlr_allocator_destroy(ptr); });
};

class output_layout
    : public w_ptr_wrapper_base<output_layout, wlr_output_layout> {
public:
  using base::base;
  using create_fn = decltype([]() { return wlr_output_layout_create(); });
  using destroy_fn =
      decltype([](wlr_output_layout *ptr) { wlr_output_layout_destroy(ptr); });
};

class cursor : public w_ptr_wrapper_base<cursor, wlr_cursor> {
public:
  using base::base;
  using create_fn = decltype([]() { return wlr_cursor_create(); });
  using destroy_fn = decltype([](wlr_cursor *ptr) { wlr_cursor_destroy(ptr); });
  void attach_output_layout(output_layout &layout) {
    wlr_cursor_attach_output_layout(get(), layout.get());
  }

  void move(double dx, double dy, wlr_input_device *device = nullptr) {
    wlr_cursor_move(get(), device, dx, dy);
  }

  void attach_input_device(wlr_input_device *device) {
    wlr_cursor_attach_input_device(get(), device);
  }

  void set_xcursor(wlr_xcursor_manager *manager, const char *name) {
    wlr_cursor_set_xcursor(get(), manager, name);
  }

  void set_surface(wlr_surface *surface = nullptr, int32_t hotspot_x = 0,
                   int32_t hotspot_y = 0) {
    wlr_cursor_set_surface(get(), surface, hotspot_x, hotspot_y);
  }
};

class scene : public w_ptr_wrapper_base<scene, wlr_scene> {
public:
  using base::base;
  using create_fn = decltype([]() { return wlr_scene_create(); });
  using destroy_fn = default_free<wlr_scene>;
};

class seat : public w_ptr_wrapper_base<seat, wlr_seat> {
public:
  using base::base;
  using create_fn = decltype([](display &d, const char *name) {
    return wlr_seat_create(d.get(), name);
  });
  using destroy_fn = decltype([](wlr_seat *ptr) { wlr_seat_destroy(ptr); });

  void pointer_notify_frame() { wlr_seat_pointer_notify_frame(get()); }

  void set_keyboard(wlr_keyboard *kbd) { wlr_seat_set_keyboard(get(), kbd); }
};

class xcursor_manager
    : public w_ptr_wrapper_base<xcursor_manager, wlr_xcursor_manager> {
public:
  using base::base;
  using create_fn = decltype([](const char *name, uint32_t size) {
    return wlr_xcursor_manager_create(name, size);
  });
  using destroy_fn = decltype([](wlr_xcursor_manager *ptr) {
    wlr_xcursor_manager_destroy(ptr);
  });
};

namespace detail {
template <typename Struct, typename Data = void> struct listener_base {
public:
  listener_base(const listener_base &) = delete;
  listener_base(listener_base &&) = delete;
  listener_base &operator=(const listener_base &) = delete;
  listener_base &operator=(listener_base &&) = delete;

  template <typename F>
  listener_base(Struct *self, F)
      : m_self_ptr(self),
        m_listener{.notify = [](wl_listener *listener, void *data) -> void {
          // NOLINTBEGIN
          auto *pl = reinterpret_cast<listener_base *>(
              reinterpret_cast<char *>(listener) -
              offsetof(listener_base, m_listener));
          // NOLINTEND
          std::invoke(F{}, pl->m_self_ptr, static_cast<Data *>(data));
        }} {}

  ~listener_base() { wl_list_remove(&m_listener.link); }

  constexpr wl_listener *get() { return std::addressof(m_listener); }

  constexpr void add_to_signal(wl_signal &signal) {
    wl_signal_add(&signal, get());
  }

private:
  wl_listener m_listener; // mutable ?
  Struct *m_self_ptr;
};
} // namespace detail
class server {
public:
  server() {
    m_display = display::try_create().value();
    m_backend = backend::try_create(m_display).value();
    m_renderer = renderer::try_create(m_backend).value();
    m_renderer.init_wl_display(m_display);
    m_allocator = allocator::try_create(m_backend, m_renderer).value();

    m_display.init_compositor(5, m_renderer);
    m_display.init_subcompositor();
    m_display.init_data_device_manager();

    m_listener_new_output.add_to_signal(m_backend.events().new_output);

    m_output_layout = output_layout::try_create().value();
    m_scene = scene::try_create().value();

    m_scene_output_layout =
        wlr_scene_attach_output_layout(m_scene.get(), m_output_layout.get());

    m_cursor = cursor::try_create().value();
    m_cursor.attach_output_layout(m_output_layout);

    m_listener_new_input.add_to_signal(m_backend.events().new_input);

    m_listener_cursor_motion.add_to_signal(m_cursor.events().motion);

    m_listener_cursor_frame.add_to_signal(m_cursor.events().frame);

    m_seat = seat::try_create(m_display, "seat0").value();

    m_listener_request_cursor.add_to_signal(m_seat.events().request_set_cursor);

    m_xcursor_manager = xcursor_manager::try_create(nullptr, 32).value();

    m_display.init_xdg_shell(3);
    m_listener_new_xdg_toplevel.add_to_signal(
        m_display.xdg_shell_events().new_surface);
  }

  auto &get_display() { return m_display; }
  auto &get_backend() { return m_backend; }

private:
  display m_display;
  backend m_backend;
  renderer m_renderer;
  allocator m_allocator;

  wlr_output *m_last_output;

  scene m_scene;
  output_layout m_output_layout;

  wlr_scene_output_layout *m_scene_output_layout;

  cursor m_cursor;
  seat m_seat;

  xcursor_manager m_xcursor_manager;

private:
  template <typename Data> using listener = detail::listener_base<server, Data>;

  listener<wlr_output> m_listener_new_output{
      this, [](server *self, wlr_output *output) {
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

        self->m_last_output = output;
        self->m_listener_output_frame.add_to_signal(output->events.frame);

        auto *l_output =
            wlr_output_layout_add_auto(self->m_output_layout.get(), output);
        auto *scene_output =
            wlr_scene_output_create(self->m_scene.get(), output);
        wlr_scene_output_layout_add_output(self->m_scene_output_layout,
                                           l_output, scene_output);
      }};

  listener<wlr_input_device> m_listener_new_input{
      this, [](server *self, wlr_input_device *device) {
        switch (device->type) {
        case WLR_INPUT_DEVICE_POINTER: {
          wlr_log(WLR_DEBUG, "New pointer device: %s", device->name);
          self->m_cursor.attach_input_device(device);
          break;
        }
        case WLR_INPUT_DEVICE_KEYBOARD: {
          wlr_keyboard *kbd = wlr_keyboard_from_input_device(device);
          self->m_seat.set_keyboard(kbd);
          wlr_log(WLR_DEBUG, "New keyboard device: %s", device->name);
          break;
        }
        default:
          break;
        }
      }};

  listener<wlr_seat_pointer_request_set_cursor_event> m_listener_request_cursor{
      this, [](server *self, wlr_seat_pointer_request_set_cursor_event *event) {
        auto *client = self->m_seat.get()->pointer_state.focused_client;
        wlr_log(WLR_DEBUG, "request cursor %p", client);
        if (client == event->seat_client)
          self->m_cursor.set_surface(event->surface, event->hotspot_x,
                                     event->hotspot_y);
      }};

  listener<void> m_listener_cursor_frame{
      this, [](server *self, void *) { self->m_seat.pointer_notify_frame(); }};

  listener<wlr_pointer_motion_event> m_listener_cursor_motion{
      this, [](server *self, wlr_pointer_motion_event *event) {
        auto &c = self->m_cursor;
        c.move(event->delta_x, event->delta_y, &event->pointer->base);
        c.set_xcursor(self->m_xcursor_manager.get(), "default");
      }};

  listener<wlr_xdg_surface> m_listener_new_xdg_toplevel{
      this, [](server *self, wlr_xdg_surface *surface) {
        if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
          return;
        switch (surface->role) {
        case WLR_XDG_SURFACE_ROLE_TOPLEVEL: {
          auto *toplevel = surface->toplevel;
          wlr_log(WLR_DEBUG, "New xdg toplevel: %s", toplevel->title);
          auto *scene_tree = wlr_scene_xdg_surface_create(
              &self->m_scene.get()->tree, toplevel->base);
          wlr_scene_node_raise_to_top(&scene_tree->node);
          break;
        }
        default:
          break;
        }
      }};

  listener<void> m_listener_output_frame{
      this, [](server *self, void *) {
        auto *scene = self->m_scene.get();
        auto *scene_output =
            wlr_scene_get_scene_output(scene, self->m_last_output);
        wlr_scene_output_commit(scene_output, nullptr);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(scene_output, &now);
      }};
};
} // namespace mcage

std::counting_semaphore<1> sem{0};

int main() {
  wlr_log_init(WLR_DEBUG, nullptr);
  mcage::server s{};
  const char *socket = s.get_display().add_socket_auto();
  wlr_log(WLR_INFO, "Running compositor on wayland display '%s'", socket);
  s.get_backend().start();

  setenv("WAYLAND_DISPLAY", socket, 1);

  pid_t child_pid{};
  {
    ::posix_spawn_file_actions_t fa{};
    ::posix_spawn_file_actions_init(&fa);
    std::string file = "foot";
    const std::array<char *, 2> argv = {file.data(), nullptr};
    int res = ::posix_spawnp(&child_pid, file.c_str(), nullptr, nullptr,
                             argv.data(), environ);
    std::printf("Spawned %d\n", child_pid);
  }

  // run in another thread to prevent deadlock between client and compositor
  std::jthread sig_thread([&, child_pid]() {
    sem.acquire();
    std::printf("Killing child %d\n", child_pid);
    ::kill(child_pid, SIGTERM);
    ::waitpid(child_pid, nullptr, 0);
    std::printf("Terminating display\n");
    s.get_display().terminate();
  });

  {
    struct sigaction action {};
    action.sa_handler = [](int) { sem.release(); };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
  }

  s.get_display().run();
}
