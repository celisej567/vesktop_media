// DAVE (audio & video E2EE) state of one connection over libdave.
// Mirrors what Discord's web client does in its DaveSessionManager, with the native callback shapes:
//   prepareMLSCommitTransition / processMLSWelcome -> cb(ok, protocolVersion, roster{userId: ArrayBuffer})
//   secure frames state -> {version, epochAuthenticator (base64)}
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <dave/dave_interfaces.h>

namespace vv {

using Bytes = std::vector<uint8_t>;

class DaveSession {
public:
    struct State {
        int version = 0;
        std::string epochAuthenticator;
    };
    using Roster = std::map<std::string, Bytes>;
    using StateCallback = std::function<void(const State&)>;
    using FailureCallback = std::function<void(const std::string& source, const std::string& reason)>;

    DaveSession(std::string selfUserId, std::string signingKeyId, FailureCallback onFailure, StateCallback onState);
    ~DaveSession();

    // Users
    void CreateUser(const std::string& userId);
    void DestroyUser(const std::string& userId);

    // JS entry points (JS thread)
    void PrepareTransition(int transitionId, int version);
    void PrepareEpoch(const std::string& epoch, int version, const std::string& groupId);
    void ExecuteTransition(int transitionId);
    void UpdateExternalSender(const Bytes& package);
    Bytes GetKeyPackage();
    std::optional<Bytes> ProcessProposals(const Bytes& proposals);
    // nullopt = ignored (no callback), {false} = failed, {true, roster} = joined
    std::optional<std::pair<bool, Roster>> ProcessCommit(int transitionId, const Bytes& commit);
    std::pair<bool, Roster> ProcessWelcome(int transitionId, const Bytes& welcome);
    int ProtocolVersion() const;
    void GetPairwiseFingerprint(int version, const std::string& userId, std::function<void(const Bytes&)> cb);

    // Media (any thread)
    bool Engaged() const { return engaged_; }
    void AssignSsrc(uint32_t ssrc, discord::dave::Codec codec);
    // false = drop the frame
    bool Encrypt(discord::dave::MediaType type, uint32_t ssrc, const uint8_t* frame, size_t size, Bytes& out);
    bool Decrypt(const std::string& userId, discord::dave::MediaType type, const uint8_t* frame, size_t size, Bytes& out);
    discord::dave::EncryptorStats EncryptorStats(discord::dave::MediaType type) const;
    size_t MaxCiphertextSize(discord::dave::MediaType type, size_t frameSize) const;
    discord::dave::DecryptorStats DecryptorStats(const std::string& userId, discord::dave::MediaType type) const;

    static Bytes MlsSigningKey(const std::string& sessionId, int keyVersion, Bytes& signature);

private:
    std::set<std::string> RecognizedUsers() const;
    void SetupRatchetLocked(const std::string& userId, int version);
    void PrepareRatchetsLocked(int transitionId, int version);
    void NotifyStateLocked();
    static Roster ToRoster(const discord::dave::RosterMap& map);

    const std::string selfUserId_;
    mutable std::mutex mutex_;
    std::unique_ptr<discord::dave::mls::ISession> session_;
    std::unique_ptr<discord::dave::IEncryptor> encryptor_;
    std::map<std::string, std::unique_ptr<discord::dave::IDecryptor>> decryptors_;
    std::set<std::string> users_;
    std::map<int, int> transitions_;
    int latestPreparedVersion_ = 0;
    std::shared_ptr<::mlspp::SignaturePrivateKey> transientKey_;
    bool engaged_ = false;
    int encryptorVersion_ = 0; // as the web client: updated when the encryptor reports a version change
    std::optional<State> lastState_;
    StateCallback onState_;
};

} // namespace vv
