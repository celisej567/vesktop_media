// GIO / GLib functions used for the xdg-desktop-portal D-Bus calls, resolved at runtime (Electron links GLib,
// so it is always loaded). Prototypes come from the system headers through decltype; nothing is linked.
#pragma once

#define G_DISABLE_CHECKS // G_VARIANT_TYPE as a plain cast instead of a function call
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

namespace vv {

#define VV_GIO_FUNCTIONS(X)                                                                                            \
    X(g_bus_get_sync)                                                                                                  \
    X(g_dbus_connection_get_unique_name)                                                                               \
    X(g_dbus_connection_set_exit_on_close)                                                                               \
    X(g_dbus_connection_call_sync)                                                                                     \
    X(g_dbus_connection_call_with_unix_fd_list_sync)                                                                   \
    X(g_dbus_connection_signal_subscribe)                                                                              \
    X(g_dbus_connection_signal_unsubscribe)                                                                            \
    X(g_variant_new)                                                                                                   \
    X(g_variant_new_string)                                                                                            \
    X(g_variant_new_uint32)                                                                                            \
    X(g_variant_new_boolean)                                                                                           \
    X(g_variant_builder_new)                                                                                           \
    X(g_variant_builder_add)                                                                                           \
    X(g_variant_builder_unref)                                                                                         \
    X(g_variant_get)                                                                                                   \
    X(g_variant_get_child_value)                                                                                       \
    X(g_variant_get_variant)                                                                                           \
    X(g_variant_get_uint32)                                                                                            \
    X(g_variant_lookup)                                                                                                \
    X(g_variant_lookup_value)                                                                                          \
    X(g_variant_iter_init)                                                                                             \
    X(g_variant_iter_next)                                                                                             \
    X(g_variant_n_children)                                                                                            \
    X(g_variant_unref)                                                                                                 \
    X(g_variant_ref_sink)                                                                                              \
    X(g_unix_fd_list_get)                                                                                              \
    X(g_object_unref)                                                                                                  \
    X(g_error_free)                                                                                                    \
    X(g_free)                                                                                                          \
    X(g_main_context_new)                                                                                              \
    X(g_main_context_unref)                                                                                            \
    X(g_main_context_push_thread_default)                                                                              \
    X(g_main_context_pop_thread_default)                                                                               \
    X(g_main_context_iteration)                                                                                        \
    X(g_main_context_wakeup)

struct GioApi {
#define VV_GIO_MEMBER(name) decltype(&::name) name = nullptr;
    VV_GIO_FUNCTIONS(VV_GIO_MEMBER)
#undef VV_GIO_MEMBER
};

// null when GIO is not available
const GioApi* Gio();

} // namespace vv
