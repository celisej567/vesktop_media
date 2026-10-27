// Windows has no xdg-desktop-portal and no PipeWire: the system picker reports an error and portal captures
// never start. Windows captures the screen / window ids of Electron's picker instead (video_send.cc)
#include "capture/pipewire_capturer.h"
#include "capture/portal.h"

namespace vv {

ScreenCastPortal& ScreenCastPortal::Get()
{
    static ScreenCastPortal portal;
    return portal;
}

ScreenCastPortal::~ScreenCastPortal() = default;

void ScreenCastPortal::Present(const std::string&, std::function<void(Outcome, const std::string&)> done)
{
    done(Outcome::Error, "there is no system screen share picker on Windows");
}

bool ScreenCastPortal::TakePicked(PickedSource&)
{
    return false;
}

bool ScreenCastPortal::TakeCapture(std::unique_ptr<PipeWireCapturer>&, webrtc::scoped_refptr<FrameSource>&)
{
    return false;
}

json ScreenCastPortal::PickedInfo()
{
    return json::object();
}

void ScreenCastPortal::Release() {}
void ScreenCastPortal::CloseSession() {}

struct PipeWireCapturer::Impl {};

PipeWireCapturer::PipeWireCapturer(PickedSource, webrtc::scoped_refptr<FrameSource>, std::function<void(const std::string&)>)
{
}

PipeWireCapturer::~PipeWireCapturer() = default;

bool PipeWireCapturer::Start()
{
    return false;
}

void PipeWireCapturer::SetEnded(std::function<void(const std::string&)> ended)
{
    if (ended) ended("PipeWire is not available on Windows");
}

std::string PipeWireCapturer::SourceAppId() const
{
    return "";
}

std::string PipeWireCapturer::LastAppId()
{
    return "";
}

} // namespace vv
