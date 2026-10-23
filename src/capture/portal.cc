#include "capture/portal.h"

#include <unistd.h>

#include <cstdlib>

#include <chrono>

#include <api/make_ref_counted.h>

#include "capture/gio_api.h"
#include "capture/pipewire_capturer.h"
#include "video/frame_source.h"
#include "common/log.h"

namespace vv {

namespace {

constexpr const char* kPortal = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";

const GVariantType* Type(const char* t)
{
    return reinterpret_cast<const GVariantType*>(t);
}

std::string ErrorText(GError* e)
{
    std::string s = e && e->message ? e->message : "unknown error";
    if (e) Gio()->g_error_free(e);
    return s;
}

// One portal Request: subscribe to its Response, call the method, wait for the answer
class Call {
public:
    Call(GDBusConnection* bus, GMainContext* ctx, std::string token) : bus_(bus), ctx_(ctx), token_(std::move(token))
    {
        auto* g = Gio();
        std::string sender = g->g_dbus_connection_get_unique_name(bus) + 1; // ":1.42" -> "1_42"
        for (auto& c : sender)
            if (c == '.') c = '_';
        path_ = std::string(kPortalPath) + "/request/" + sender + "/" + token_;
        sub_ = g->g_dbus_connection_signal_subscribe(bus, kPortal, "org.freedesktop.portal.Request", "Response", path_.c_str(),
                                                     nullptr, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, &Call::OnResponse, this, nullptr);
    }
    ~Call()
    {
        Gio()->g_dbus_connection_signal_unsubscribe(bus_, sub_);
        if (results_) Gio()->g_variant_unref(results_);
    }

    // `params` is consumed. Returns the response code (0 ok, 1 cancelled, 2 other) or -1 on a call error
    int Invoke(const char* method, GVariant* params, int timeoutSec, std::string& error)
    {
        auto* g = Gio();
        GError* err = nullptr;
        GVariant* reply = g->g_dbus_connection_call_sync(bus_, kPortal, kPortalPath, kScreenCast, method, params, Type("(o)"),
                                                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
        if (!reply) {
            error = std::string(method) + ": " + ErrorText(err);
            return -1;
        }
        g->g_variant_unref(reply);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
        while (!done_ && std::chrono::steady_clock::now() < deadline) g->g_main_context_iteration(ctx_, TRUE);
        if (!done_) {
            error = std::string(method) + ": no response";
            return -1;
        }
        return code_;
    }
    GVariant* Results() const { return results_; }
    const std::string& Token() const { return token_; }

private:
    static void OnResponse(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer self)
    {
        auto* c = static_cast<Call*>(self);
        guint32 code = 2;
        GVariant* results = nullptr;
        Gio()->g_variant_get(params, "(u@a{sv})", &code, &results);
        c->code_ = static_cast<int>(code);
        c->results_ = results;
        c->done_ = true;
    }

    GDBusConnection* bus_;
    GMainContext* ctx_;
    std::string token_, path_;
    guint sub_ = 0;
    bool done_ = false;
    int code_ = 2;
    GVariant* results_ = nullptr;
};

} // namespace

ScreenCastPortal::~ScreenCastPortal() = default;

GDBusConnection* ScreenCastPortal::Bus(GError** err)
{
    // The portal ties a session to the D-Bus connection that created it and closes it (the compositor then
    // destroys the stream node) as soon as that connection goes. GLib keeps the session bus singleton only by
    // a weak reference, so this one is held for the life of the process
    std::lock_guard lock(busMutex_);
    if (!bus_) {
        bus_ = Gio()->g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, err);
        if (bus_) Gio()->g_dbus_connection_set_exit_on_close(bus_, FALSE);
    }
    return bus_;
}

ScreenCastPortal& ScreenCastPortal::Get()
{
    static auto* portal = new ScreenCastPortal();
    return *portal;
}

void ScreenCastPortal::Present(const std::string& style, std::function<void(Outcome, const std::string&)> done)
{
    std::lock_guard lock(mutex_);
    if (busy_) {
        done(Outcome::Error, "picker already open");
        return;
    }
    if (!Gio()) {
        done(Outcome::Error, "GIO not available");
        return;
    }
    busy_ = true;
    CloseSessionLocked(); // one global session
    if (thread_.joinable()) thread_.join();
    uint32_t types = style == "window" ? 2 : 1;
    thread_ = std::thread([this, types, done = std::move(done)] { Run(types, done); });
}

void ScreenCastPortal::Run(uint32_t types, std::function<void(Outcome, const std::string&)> done)
{
    auto* g = Gio();
    GMainContext* ctx = g->g_main_context_new();
    g->g_main_context_push_thread_default(ctx);
    std::string error;
    Outcome outcome = Outcome::Error;
    GError* err = nullptr;
    GDBusConnection* bus = Bus(&err);
    std::string session;
    PickedSource picked;

    auto token = [this] {
        std::lock_guard lock(mutex_);
        return "vesktop_media" + std::to_string(getpid()) + "_" + std::to_string(++counter_);
    };
    auto options = [&](const std::string& handleToken) {
        GVariantBuilder* b = g->g_variant_builder_new(Type("a{sv}"));
        g->g_variant_builder_add(b, "{sv}", "handle_token", g->g_variant_new_string(handleToken.c_str()));
        return b;
    };

    do {
        if (!bus) {
            error = "session bus: " + ErrorText(err);
            break;
        }

        // CreateSession
        {
            Call call(bus, ctx, token());
            auto* b = options(call.Token());
            auto sessionToken = token();
            g->g_variant_builder_add(b, "{sv}", "session_handle_token", g->g_variant_new_string(sessionToken.c_str()));
            int code = call.Invoke("CreateSession", g->g_variant_new("(a{sv})", b), 30, error);
            g->g_variant_builder_unref(b);
            if (code != 0) {
                if (code > 0) error = "CreateSession failed (" + std::to_string(code) + ")";
                break;
            }
            const char* handle = nullptr;
            if (!g->g_variant_lookup(call.Results(), "session_handle", "&s", &handle) || !handle) {
                error = "CreateSession: no session handle";
                break;
            }
            session = handle;
        }

        // Cursor drawn into the frames when the portal can (AvailableCursorModes bit 2 = embedded)
        uint32_t cursorModes = 0;
        {
            GVariant* r = g->g_dbus_connection_call_sync(bus, kPortal, kPortalPath, "org.freedesktop.DBus.Properties", "Get",
                                                         g->g_variant_new("(ss)", kScreenCast, "AvailableCursorModes"),
                                                         Type("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
            if (r) {
                GVariant* v = nullptr;
                g->g_variant_get(r, "(v)", &v);
                if (v) {
                    cursorModes = g->g_variant_get_uint32(v);
                    g->g_variant_unref(v);
                }
                g->g_variant_unref(r);
            }
        }

        // SelectSources
        {
            Call call(bus, ctx, token());
            auto* b = options(call.Token());
            g->g_variant_builder_add(b, "{sv}", "types", g->g_variant_new_uint32(types));
            g->g_variant_builder_add(b, "{sv}", "multiple", g->g_variant_new_boolean(FALSE));
            if (cursorModes & 2) g->g_variant_builder_add(b, "{sv}", "cursor_mode", g->g_variant_new_uint32(2));
            int code = call.Invoke("SelectSources", g->g_variant_new("(oa{sv})", session.c_str(), b), 30, error);
            g->g_variant_builder_unref(b);
            if (code != 0) {
                if (code == 1) outcome = Outcome::Cancelled;
                else if (code > 0) error = "SelectSources failed (" + std::to_string(code) + ")";
                break;
            }
        }

        // Start: the portal shows its dialog here
        {
            Call call(bus, ctx, token());
            auto* b = options(call.Token());
            int code = call.Invoke("Start", g->g_variant_new("(osa{sv})", session.c_str(), "", b), 600, error);
            g->g_variant_builder_unref(b);
            if (code != 0) {
                if (code == 1) outcome = Outcome::Cancelled;
                else if (code > 0) error = "Start failed (" + std::to_string(code) + ")";
                break;
            }
            GVariant* streams = g->g_variant_lookup_value(call.Results(), "streams", Type("a(ua{sv})"));
            if (!streams || g->g_variant_n_children(streams) == 0) {
                if (streams) g->g_variant_unref(streams);
                error = "Start: no stream";
                break;
            }
            GVariant* first = g->g_variant_get_child_value(streams, 0);
            GVariant* props = nullptr;
            g->g_variant_get(first, "(u@a{sv})", &picked.nodeId, &props);
            if (props) {
                g->g_variant_lookup(props, "size", "(ii)", &picked.width, &picked.height);
                g->g_variant_lookup(props, "source_type", "u", &picked.sourceType);
                g->g_variant_unref(props);
            }
            g->g_variant_unref(first);
            g->g_variant_unref(streams);
        }

        // OpenPipeWireRemote
        {
            GVariantBuilder* b = g->g_variant_builder_new(Type("a{sv}"));
            GUnixFDList* fds = nullptr;
            GVariant* r = g->g_dbus_connection_call_with_unix_fd_list_sync(
                bus, kPortal, kPortalPath, kScreenCast, "OpenPipeWireRemote", g->g_variant_new("(oa{sv})", session.c_str(), b),
                Type("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &err);
            g->g_variant_builder_unref(b);
            if (!r) {
                error = "OpenPipeWireRemote: " + ErrorText(err);
                break;
            }
            gint32 index = 0;
            g->g_variant_get(r, "(h)", &index);
            g->g_variant_unref(r);
            picked.fd = fds ? g->g_unix_fd_list_get(fds, index, nullptr) : -1;
            if (fds) g->g_object_unref(fds);
            if (picked.fd < 0) {
                error = "OpenPipeWireRemote: no fd";
                break;
            }
        }
        outcome = Outcome::Picked;
    } while (false);

    if (outcome != Outcome::Picked && !session.empty() && bus)
        g->g_dbus_connection_call_sync(bus, kPortal, session.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                                       nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    std::unique_ptr<PipeWireCapturer> capturer;
    webrtc::scoped_refptr<FrameSource> source;
    if (outcome == Outcome::Picked) {
        // connect now, before the compositor gives up on the unused stream
        PickedSource mine = picked;
        mine.fd = dup(picked.fd);
        source = webrtc::make_ref_counted<FrameSource>(true, "desktop");
        capturer = std::make_unique<PipeWireCapturer>(mine, source, [](const std::string&) {});
        if (capturer->Start()) {
            picked.appId = capturer->SourceAppId();
        } else {
            capturer.reset();
            source = nullptr;
            outcome = Outcome::Error;
            error = "cannot connect to the screen capture stream";
            if (picked.fd >= 0) close(picked.fd);
            if (g)
                g->g_dbus_connection_call_sync(bus, kPortal, session.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
        }
    }
    g->g_main_context_pop_thread_default(ctx);
    g->g_main_context_unref(ctx);

    {
        std::lock_guard lock(mutex_);
        busy_ = false;
        if (outcome == Outcome::Picked) {
            session_ = session;
            picked_ = picked;
            capturer_ = std::move(capturer);
            source_ = std::move(source);
        }
    }
    if (outcome == Outcome::Picked)
        VV_LOGI("portal", "Portal picker: node_id=%u, fd=%d, %dx%d, source type %u, app %s", picked.nodeId, picked.fd,
                picked.width, picked.height, picked.sourceType, picked.appId.empty() ? "-" : picked.appId.c_str());
    else if (outcome == Outcome::Cancelled) VV_LOGI("portal", "picker cancelled");
    else VV_LOGE("portal", "picker failed: %s", error.c_str());
    done(outcome, error);
}

json ScreenCastPortal::PickedInfo()
{
    std::lock_guard lock(mutex_);
    if (session_.empty()) return nullptr;
    // the app arrives with the node's info, after the pick: ask the capture that runs for it
    std::string appId = picked_.appId;
    if (appId.empty() && capturer_) appId = capturer_->SourceAppId();
    if (appId.empty()) appId = PipeWireCapturer::LastAppId();
    return {{"sourceType", picked_.sourceType == 2 ? "window" : "screen"}, {"appId", appId}};
}

bool ScreenCastPortal::TakeCapture(std::unique_ptr<PipeWireCapturer>& capturer, webrtc::scoped_refptr<FrameSource>& source)
{
    std::lock_guard lock(mutex_);
    if (!capturer_) return false;
    capturer = std::move(capturer_);
    source = std::move(source_);
    return true;
}

bool ScreenCastPortal::TakePicked(PickedSource& out)
{
    // test hook (tools/fake-sfu.js --screen): capture this node of the default PipeWire daemon, no portal
    if (const char* node = getenv("VESKTOP_MEDIA_PIPEWIRE_NODE")) {
        out = {};
        out.fd = -1;
        out.nodeId = static_cast<uint32_t>(strtoul(node, nullptr, 10));
        return out.nodeId != 0;
    }
    std::lock_guard lock(mutex_);
    if (session_.empty() || picked_.fd < 0) return false;
    out = picked_;
    out.fd = dup(picked_.fd);
    return out.fd >= 0;
}

void ScreenCastPortal::Release()
{
    std::lock_guard lock(mutex_);
    if (capturer_) CloseSessionLocked();
}

void ScreenCastPortal::CloseSession()
{
    std::lock_guard lock(mutex_);
    CloseSessionLocked();
}

void ScreenCastPortal::CloseSessionLocked()
{
    capturer_.reset(); // an unused pick: stops its capture before the session goes
    source_ = nullptr;
    if (picked_.fd >= 0) close(picked_.fd);
    picked_ = {};
    if (session_.empty()) return;
    auto* g = Gio();
    if (GDBusConnection* bus = g ? Bus(nullptr) : nullptr)
        g->g_dbus_connection_call_sync(bus, kPortal, session_.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                                       nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    VV_LOGI("portal", "portal session %s closed", session_.c_str());
    session_.clear();
}

} // namespace vv
