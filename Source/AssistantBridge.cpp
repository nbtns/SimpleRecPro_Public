#include "AssistantBridge.h"

#include <cmath>
#if JUCE_WINDOWS
 #include <process.h>
#else
 #include <unistd.h>
#endif

namespace
{
constexpr juce::int64 maximumRequestBytes = 65536;
constexpr int maximumPerTick = 8;

bool isRequestId(const juce::String& id)
{
    return id.length() == 32 && id.containsOnly("0123456789abcdef");
}

juce::var object(std::initializer_list<std::pair<juce::Identifier, juce::var>> fields)
{
    auto* result = new juce::DynamicObject();
    for (const auto& field : fields)
        result->setProperty(field.first, field.second);
    return juce::var(result);
}
}

AssistantBridge::AssistantBridge(std::function<juce::var(const juce::var&)> callback)
    : handler(std::move(callback))
{
    const auto overridePath = juce::SystemStats::getEnvironmentVariable("SIMPLE_REC_CONTROL_DIR", {});
    root = overridePath.isNotEmpty() ? juce::File(overridePath)
        : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("SimpleRecPro").getChildFile("CodexControl");
    auto lockPath = root.getFullPathName();
   #if JUCE_WINDOWS
    lockPath = lockPath.toLowerCase();
   #endif
    instanceLock = std::make_unique<juce::InterProcessLock>(
        "SimpleRecProCodexControl_" + juce::String::toHexString(lockPath.hashCode64()));
}

AssistantBridge::~AssistantBridge() { stop(); }

bool AssistantBridge::writeAtomic(const juce::File& destination, const juce::var& value)
{
    juce::TemporaryFile temporary(destination);
    return temporary.getFile().replaceWithText(juce::JSON::toString(value, true))
        && temporary.overwriteTargetFileWithTemporary();
}

bool AssistantBridge::writeManifest()
{
    lastHeartbeat = juce::Time::currentTimeMillis();
    return writeAtomic(root.getChildFile("manifest.json"), object({
        { "protocol", 1 }, { "session", session }, { "token", token },
        { "pid", static_cast<juce::int64>(
           #if JUCE_WINDOWS
            _getpid()
           #else
            getpid()
           #endif
          ) },
        { "heartbeat_ms", lastHeartbeat },
        { "request_dir", requests.getRelativePathFrom(root) },
        { "response_dir", responses.getRelativePathFrom(root) }
    }));
}

void AssistantBridge::start()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (ownsLock)
        return;
    if (!instanceLock->enter(0))
    {
        status = juce::String::fromUTF8("別のSimpleRecProがCodexに接続しています");
        return;
    }
    ownsLock = true;
    session = juce::Uuid().toString();
    token = juce::Uuid().toString() + juce::Uuid().toString();
    requests = root.getChildFile(session).getChildFile("requests");
    responses = root.getChildFile(session).getChildFile("responses");
    if (requests.createDirectory().failed() || responses.createDirectory().failed() || !writeManifest())
    {
        stop();
        status = juce::String::fromUTF8("Codex接続用フォルダーを作成できませんでした");
        return;
    }
    completed.clear();
    status = juce::String::fromUTF8("Codexから操作できます（このPC内）");
    startTimer(50);
}

void AssistantBridge::stop()
{
    stopTimer();
    if (ownsLock)
    {
        const auto manifest = root.getChildFile("manifest.json");
        if (juce::JSON::parse(manifest)["session"].toString() == session)
            manifest.deleteFile();
        instanceLock->exit();
        ownsLock = false;
    }
    status = juce::String::fromUTF8("Codex接続は停止中です");
}

void AssistantBridge::timerCallback()
{
    const auto now = juce::Time::currentTimeMillis();
    if (now - lastHeartbeat >= 1000 && !writeManifest())
    {
        stop();
        status = juce::String::fromUTF8("Codex接続用フォルダーにアクセスできません");
        return;
    }
    int count = 0;
    for (const auto& entry : juce::RangedDirectoryIterator(requests, false, "*.json", juce::File::findFiles))
    {
        if (++count > maximumPerTick)
            break;
        const auto file = entry.getFile();
        const auto id = file.getFileNameWithoutExtension();
        if (!isRequestId(id) || file.getSize() > maximumRequestBytes)
        {
            file.deleteFile();
            continue;
        }
        const auto request = juce::JSON::parse(file);
        file.deleteFile(); // Claim before dispatch: requests never execute twice after a write failure.
        if (!request.isObject() || request["id"].toString() != id
            || request["session"].toString() != session || request["token"].toString() != token)
            continue;
        if (completed.count(id) != 0)
            continue;
        const auto deadlineValue = request["expires_at_ms"];
        const auto deadline = static_cast<double>(deadlineValue);
        juce::var result;
        juce::String error;
        if (!(deadlineValue.isInt() || deadlineValue.isInt64() || deadlineValue.isDouble())
            || !std::isfinite(deadline) || deadline <= static_cast<double>(now)
            || deadline > static_cast<double>(now + 60000))
            error = "Request expired or invalid deadline; no edit applied.";
        else if (!request["action"].isString() || !request["args"].isObject())
            error = "Invalid action or arguments; no edit applied.";
        else if (completed.size() >= 100000)
            error = "Control session request limit reached. Restart SimpleRecPro.";
        else
        {
            completed.insert(id);
            try { result = handler(request); }
            catch (const std::exception& exception) { error = juce::String(exception.what()); }
            catch (...) { error = "Control operation failed."; }
            if (result.isObject() && result.hasProperty("error"))
                error = result["error"].toString();
        }
        auto response = object({ { "id", id }, { "session", session }, { "ok", error.isEmpty() } });
        response.getDynamicObject()->setProperty(error.isEmpty() ? "result" : "error",
                                                error.isEmpty() ? result : juce::var(error));
        if (!writeAtomic(responses.getChildFile(id + ".json"), response))
            status = juce::String::fromUTF8("Codexへの結果送信に失敗しました。現在の状態を確認してください");
    }
}
