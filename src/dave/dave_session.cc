#include "dave/dave_session.h"

#include <dave/logger.h>
#include <dave/persisted_key_pair.h>
#include <dave/version.h>

#include <algorithm>

#include "common/log.h"

namespace vv {

using namespace discord::dave;

namespace {

std::string Base64(const Bytes& data)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = data[i] << 16 | data[i + 1] << 8 | data[i + 2];
        out += tbl[v >> 18 & 63], out += tbl[v >> 12 & 63], out += tbl[v >> 6 & 63], out += tbl[v & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t v = data[i] << 16;
        out += tbl[v >> 18 & 63], out += tbl[v >> 12 & 63], out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v = data[i] << 16 | data[i + 1] << 8;
        out += tbl[v >> 18 & 63], out += tbl[v >> 12 & 63], out += tbl[v >> 6 & 63], out += '=';
    }
    return out;
}

void DaveLog(LoggingSeverity severity, const char* file, int line, const std::string& message)
{
    LogLevel level = severity == LS_ERROR ? LogLevel::Error : severity == LS_WARNING ? LogLevel::Warning
                     : severity == LS_INFO ? LogLevel::Info : LogLevel::Verbose;
    Log(level, "dave", "%s:%d %s", file ? file : "", line, message.c_str());
}

} // namespace

DaveSession::DaveSession(std::string selfUserId, std::string signingKeyId, FailureCallback onFailure, StateCallback onState)
    : selfUserId_(std::move(selfUserId)), onState_(std::move(onState))
{
    static std::once_flag once;
    std::call_once(once, [] { SetLogSink(DaveLog); });
    session_ = mls::CreateSession("", signingKeyId, [cb = std::move(onFailure)](const std::string& s, const std::string& r) {
        VV_LOGE("dave", "MLS failure: %s %s", s.c_str(), r.c_str());
        if (cb) cb(s, r);
    });
    encryptor_ = CreateEncryptor();
    // libdave calls this synchronously from SetPassthroughMode, which only SetupRatchetLocked calls: mutex_
    // is already held here (locking it again deadlocked the JS thread when a transition turned DAVE off)
    encryptor_->SetProtocolVersionChangedCallback([this] {
        encryptorVersion_ = encryptor_->GetProtocolVersion();
        NotifyStateLocked();
    });
}

DaveSession::~DaveSession()
{
    std::lock_guard lock(mutex_);
    if (encryptor_) encryptor_->SetProtocolVersionChangedCallback(nullptr);
}

std::set<std::string> DaveSession::RecognizedUsers() const
{
    auto ids = users_;
    ids.insert(selfUserId_);
    return ids;
}

void DaveSession::CreateUser(const std::string& userId)
{
    if (userId == selfUserId_) return;
    std::lock_guard lock(mutex_);
    users_.insert(userId);
    if (!decryptors_.count(userId)) decryptors_[userId] = CreateDecryptor();
    SetupRatchetLocked(userId, latestPreparedVersion_);
}

void DaveSession::DestroyUser(const std::string& userId)
{
    std::lock_guard lock(mutex_);
    users_.erase(userId);
    decryptors_.erase(userId);
}

void DaveSession::SetupRatchetLocked(const std::string& userId, int version)
{
    std::unique_ptr<IKeyRatchet> ratchet;
    if (version != kDisabledVersion) ratchet = session_->GetKeyRatchet(userId);

    if (userId == selfUserId_) {
        if (ratchet) {
            encryptor_->SetKeyRatchet(std::move(ratchet));
            encryptor_->SetPassthroughMode(false);
        } else {
            encryptor_->SetPassthroughMode(true);
        }
        return;
    }
    auto it = decryptors_.find(userId);
    if (it == decryptors_.end()) return;
    if (ratchet) {
        it->second->TransitionToKeyRatchet(std::move(ratchet));
        it->second->TransitionToPassthroughMode(false);
    } else {
        it->second->TransitionToPassthroughMode(true);
    }
}

void DaveSession::PrepareRatchetsLocked(int transitionId, int version)
{
    engaged_ = true;
    for (auto& id : users_) SetupRatchetLocked(id, version);
    if (transitionId == kInitTransitionId) {
        SetupRatchetLocked(selfUserId_, version);
        NotifyStateLocked();
    } else {
        transitions_[transitionId] = version;
    }
    latestPreparedVersion_ = version;
}

void DaveSession::PrepareTransition(int transitionId, int version)
{
    std::lock_guard lock(mutex_);
    VV_LOGI("dave", "prepare transition %d version %d", transitionId, version);
    PrepareRatchetsLocked(transitionId, version);
}

void DaveSession::PrepareEpoch(const std::string& epoch, int version, const std::string& groupId)
{
    std::lock_guard lock(mutex_);
    VV_LOGI("dave", "prepare epoch %s version %d group %s", epoch.c_str(), version, groupId.c_str());
    engaged_ = true;
    if (epoch == "1") {
        uint64_t group = 0;
        try {
            group = std::stoull(groupId);
        } catch (...) {
        }
        session_->Init(static_cast<discord::dave::ProtocolVersion>(version), group, selfUserId_, transientKey_);
        NotifyStateLocked();
    } else {
        session_->SetProtocolVersion(static_cast<discord::dave::ProtocolVersion>(version));
    }
}

void DaveSession::ExecuteTransition(int transitionId)
{
    std::lock_guard lock(mutex_);
    auto it = transitions_.find(transitionId);
    if (it == transitions_.end()) {
        VV_LOGW("dave", "transition %d not found, ignoring", transitionId);
        return;
    }
    int version = it->second;
    transitions_.erase(it);
    VV_LOGI("dave", "execute transition %d version %d", transitionId, version);
    if (version == kDisabledVersion) session_->Reset();
    SetupRatchetLocked(selfUserId_, version);
    NotifyStateLocked();
}

void DaveSession::UpdateExternalSender(const Bytes& package)
{
    std::lock_guard lock(mutex_);
    session_->SetExternalSender(package);
}

Bytes DaveSession::GetKeyPackage()
{
    std::lock_guard lock(mutex_);
    return session_->GetMarshalledKeyPackage();
}

std::optional<Bytes> DaveSession::ProcessProposals(const Bytes& proposals)
{
    std::lock_guard lock(mutex_);
    return session_->ProcessProposals(proposals, RecognizedUsers());
}

DaveSession::Roster DaveSession::ToRoster(const RosterMap& map)
{
    Roster roster;
    for (auto& [id, key] : map) roster[std::to_string(id)] = key;
    return roster;
}

std::optional<std::pair<bool, DaveSession::Roster>> DaveSession::ProcessCommit(int transitionId, const Bytes& commit)
{
    std::lock_guard lock(mutex_);
    auto result = session_->ProcessCommit(commit);
    if (std::holds_alternative<ignored_t>(result)) return std::nullopt;
    if (std::holds_alternative<failed_t>(result)) return std::make_pair(false, Roster{});
    PrepareRatchetsLocked(transitionId, session_->GetProtocolVersion());
    return std::make_pair(true, ToRoster(std::get<RosterMap>(result)));
}

std::pair<bool, DaveSession::Roster> DaveSession::ProcessWelcome(int transitionId, const Bytes& welcome)
{
    std::lock_guard lock(mutex_);
    auto roster = session_->ProcessWelcome(welcome, RecognizedUsers());
    if (!roster) return {false, {}};
    PrepareRatchetsLocked(transitionId, session_->GetProtocolVersion());
    return {true, ToRoster(*roster)};
}

int DaveSession::ProtocolVersion() const
{
    std::lock_guard lock(mutex_);
    return session_->GetProtocolVersion();
}

void DaveSession::GetPairwiseFingerprint(int version, const std::string& userId, std::function<void(const Bytes&)> cb)
{
    std::lock_guard lock(mutex_);
    session_->GetPairwiseFingerprint(static_cast<uint16_t>(version), userId, std::move(cb));
}

void DaveSession::NotifyStateLocked()
{
    State state;
    state.version = encryptorVersion_;
    if (state.version != kDisabledVersion) state.epochAuthenticator = Base64(session_->GetLastEpochAuthenticator());
    if (lastState_ && lastState_->version == state.version && lastState_->epochAuthenticator == state.epochAuthenticator)
        return;
    lastState_ = state;
    VV_LOGI("dave", "state update: version %d", state.version);
    if (onState_) onState_(state);
}

void DaveSession::AssignSsrc(uint32_t ssrc, Codec codec)
{
    std::lock_guard lock(mutex_);
    encryptor_->AssignSsrcToCodec(ssrc, codec);
}

bool DaveSession::Encrypt(MediaType type, uint32_t ssrc, const uint8_t* frame, size_t size, Bytes& out)
{
    std::lock_guard lock(mutex_);
    if (!engaged_) {
        out.assign(frame, frame + size);
        return true;
    }
    out.resize(encryptor_->GetMaxCiphertextByteSize(type, size));
    size_t written = 0;
    auto rc = encryptor_->Encrypt(type, ssrc, MakeArrayView(frame, size), MakeArrayView(out.data(), out.size()), &written);
    if (rc != IEncryptor::Success) {
        // Opus silence frames may go out in clear (receivers pass them through), e.g. before the first key
        if (type == MediaType::Audio && size == kOpusSilencePacket.size() &&
            std::equal(frame, frame + size, kOpusSilencePacket.begin())) {
            out.assign(frame, frame + size);
            return true;
        }
        return false;
    }
    out.resize(written);
    return true;
}

bool DaveSession::Decrypt(const std::string& userId, MediaType type, const uint8_t* frame, size_t size, Bytes& out)
{
    std::lock_guard lock(mutex_);
    auto it = decryptors_.find(userId);
    if (!engaged_ || it == decryptors_.end()) {
        out.assign(frame, frame + size);
        return true;
    }
    out.resize(it->second->GetMaxPlaintextByteSize(type, size));
    size_t written = 0;
    auto rc = it->second->Decrypt(type, MakeArrayView(frame, size), MakeArrayView(out.data(), out.size()), &written);
    if (rc != IDecryptor::Success) return false;
    out.resize(written);
    return true;
}

size_t DaveSession::MaxCiphertextSize(MediaType type, size_t frameSize) const
{
    std::lock_guard lock(mutex_);
    return encryptor_->GetMaxCiphertextByteSize(type, frameSize);
}

EncryptorStats DaveSession::EncryptorStats(MediaType type) const
{
    std::lock_guard lock(mutex_);
    return encryptor_->GetStats(type);
}

DecryptorStats DaveSession::DecryptorStats(const std::string& userId, MediaType type) const
{
    std::lock_guard lock(mutex_);
    auto it = decryptors_.find(userId);
    if (it == decryptors_.end()) return {};
    return it->second->GetStats(type);
}

Bytes DaveSession::MlsSigningKey(const std::string& sessionId, int keyVersion, Bytes& signature)
{
    auto res = mls::GetPersistedPublicKey("", sessionId, static_cast<SignatureVersion>(keyVersion));
    signature = res.signature;
    return res.key;
}

} // namespace vv
