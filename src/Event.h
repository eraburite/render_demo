#pragma once
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// EventManager indexes listeners by type-name string.  notify() copies the
// listener list before invoking callbacks so callbacks may add/remove
// listeners without invalidating iteration.
class EventManager {
public:
    using Listener = std::function<void()>;

    template <typename Event>
    void addListener(Listener listener)
    {
        m_listeners[typeName<Event>()].push_back(std::move(listener));
    }

    template <typename Event>
    void notify()
    {
        auto it = m_listeners.find(typeName<Event>());
        if (it == m_listeners.end()) {
            return;
        }
        std::vector<Listener> snapshot = it->second;
        for (auto &listener : snapshot) {
            listener();
        }
    }

private:
    template <typename Event>
    static std::string typeName()
    {
        return typeid(Event).name();
    }

    std::unordered_map<std::string, std::vector<Listener>> m_listeners;
};

struct WindowEvent {
    enum class Type { eCreate, eResize, eDestroy };
    Type type;
};

struct SwapChainEvent {
    enum class Type { eReCreate };
    Type type;
};
